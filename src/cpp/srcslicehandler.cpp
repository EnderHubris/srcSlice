// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicehandler.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include <srcslicehandler.hpp>

// Create custom buffers with callbacks
CallbackBuf cbOut(coutHandler);
CallbackBuf cbErr(cerrHandler);

// Number of CPU cores
int CPUCount() {
#ifdef _WIN64
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (int) sysinfo.dwNumberOfProcessors;
#elif __APPLE__
    int count = 0;
    size_t len = sizeof(count);
    sysctlbyname("machdep.cpu.core_count", &count, &len, NULL, 0);
    return count;
#else
    cpu_set_t cs;
    CPU_ZERO(&cs);
    sched_getaffinity(0, sizeof(cs), &cs);
    return CPU_COUNT(&cs);
#endif
}

SrcSliceHandler::SrcSliceHandler(const CliInfo& info) {
    verboseMode = info.verboseMode;
    progressMode = info.progressMode;
    calculateControlEdges = info.calculateControlEdges;
    iterations = info.iterations;
    expandAliases = info.expandAliases;

    // if an invalid number is passed default to 1 thread
    threadCount = (info.threadCount > 0) ? info.threadCount : 1;

    unitsScaned.store(false, std::memory_order_release);

    std::thread t = std::thread(&SrcSliceHandler::ManageThreads, this);

    // argument must be a c string (const char*) or else a saxError is thrown
    srcSAXController control(info.inputFile.c_str());
    srcDispatch::srcDispatcher<srcDispatch::UnitPolicy> handler(this);
    
    if (progressMode) {
        // Save original buffers and Redirect cout and cerr
        IdleBar idlebar;
        std::cout << "[Slicing Started]" << "\n";

        auto start = std::chrono::steady_clock::now();
        control.parse(&handler); // Start parsing
        
        unitsScaned.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(backlogMutex); }
        backlogCv.notify_all();

        if (t.joinable()) t.join();

        // Restore original buffers
        idlebar.Finish("[srcSAXController Parse]");

        idlebar.Reset();
        Stage2();
        idlebar.Finish("[Stage-2]");

        idlebar.Reset();
        Stage3();
        idlebar.Finish("[Stage-3]");
        
        std::cout << "[Slicing Finished]" << "\n";
        
        auto end = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        elapsed_ms /= 1000;
        elapsed_ms /= 60;
        std::cout << "Estimate Elapsed Time: " << elapsed_ms << " min\n";
    } else {
        control.parse(&handler); // Start parsing

        unitsScaned.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(backlogMutex); }
        backlogCv.notify_all();

        if (t.joinable()) t.join();

        Stage2();
        Stage3();
    }
}

// Use string srcml buffer ctor of srcSAXController
SrcSliceHandler::SrcSliceHandler(std::string& sourceCodeStr, const TestArg& info): verboseMode(false) {
    calculateControlEdges = info.calculateControlEdges;
    expandAliases = info.expandAliases;

    // test-suite contains single-unit tests => multiple threads would be wasteful
    threadCount = 1;

    srcSAXController control(sourceCodeStr);
    srcDispatch::srcDispatcher<srcDispatch::UnitPolicy> handler(this);

    unitsScaned.store(false, std::memory_order_release);

    std::thread t = std::thread(&SrcSliceHandler::ManageThreads, this);

    control.parse(&handler); // Start parsing

    unitsScaned.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(backlogMutex); }
    backlogCv.notify_all();

    if (t.joinable()) t.join();

    Stage2();
    Stage3();
}

SrcSliceHandler::~SrcSliceHandler() {
    while (!backlog.empty()) {
        if (backlog.front() != nullptr) {
            delete backlog.front();
        }
        backlog.pop();
    }
}

void SrcSliceHandler::Notify(const srcDispatch::PolicyDispatcher *policy, const srcDispatch::srcSAXEventContext &ctx) {
    std::shared_ptr<srcDispatch::UnitData> unit = policy->Data<srcDispatch::UnitData>();
    if (unit) {
        // track what files are including to be used for fragment gluing
        for (const auto& includeData : unit->includes) {
            fileDependencyTable[ctx.currentFilePath].push_back(includeData->path.ToString());
        }

        {
            // lock the backlog because main and p_thread access this queue obj
            std::lock_guard<std::mutex> lock(backlogMutex);
    
            // push worker into queue
            backlog.push(
                new SrcSliceWorker(unit, ctx, verboseMode, calculateControlEdges)
            );
        }
        
        backlogCv.notify_one();
    }
}

void SrcSliceHandler::consumeWorker(SrcSliceWorker* worker) {
    if (worker == nullptr) return;
    Blob& data = worker->data;

    // manually move map entry values, merge will not merge if the keys between both maps exist
    for (auto& profileGroup : data.profileMap) {
        auto& dest = profileMap[profileGroup.first];
        dest.insert(
            dest.end(),
            std::make_move_iterator(profileGroup.second.begin()),
            std::make_move_iterator(profileGroup.second.end())
        );
    }

    // manually move map entry values, merge will not merge if the keys between both maps exist
    for (auto& funcSig : data.functionSigMap) {
        auto& dest = functionSigMap[funcSig.first];
        dest.insert(
            dest.end(),
            std::make_move_iterator(funcSig.second.begin()),
            std::make_move_iterator(funcSig.second.end())
        );
    }

    // all keys are expected to be unique
    callsMap.merge(data.callsMap);
    
    // make_move_iterator prevents making copies
    loopdata.insert(loopdata.end(),
        std::make_move_iterator(data.loopdata.begin()),
        std::make_move_iterator(data.loopdata.end())
    );
    forloopdata.insert(forloopdata.end(),
        std::make_move_iterator(data.forloopdata.begin()),
        std::make_move_iterator(data.forloopdata.end())
    );
    whileloopdata.insert(whileloopdata.end(),
        std::make_move_iterator(data.whileloopdata.begin()),
        std::make_move_iterator(data.whileloopdata.end())
    );
    dowhileloopdata.insert(dowhileloopdata.end(),
        std::make_move_iterator(data.dowhileloopdata.begin()),
        std::make_move_iterator(data.dowhileloopdata.end())
    );
    
    ifdata.insert(ifdata.end(),
        std::make_move_iterator(data.ifdata.begin()),
        std::make_move_iterator(data.ifdata.end())
    );
    ifStmts.insert(ifStmts.end(),
        std::make_move_iterator(data.ifStmts.begin()),
        std::make_move_iterator(data.ifStmts.end())
    );
    elsedata.insert(elsedata.end(),
        std::make_move_iterator(data.elsedata.begin()),
        std::make_move_iterator(data.elsedata.end())
    );
    elseifdata.insert(elseifdata.end(),
        std::make_move_iterator(data.elseifdata.begin()),
        std::make_move_iterator(data.elseifdata.end())
    );
};

void SrcSliceHandler::replaceWorker(SrcSliceWorker** worker) {
    // in order for wait to use the lock it must be unique_lock
    // to allow responsibility delegation
    // lock_gaurds cannot be moved nor copied!!!
    std::unique_lock<std::mutex> lock(backlogMutex);

    // if (!backlog.empty()) {
    //     if (backlog.front() != nullptr) {
    //         // start worker
    //         *worker = backlog.front();
    //         (*worker)->Start();
    //     }

    //     // remove from backlog
    //     backlog.pop();
    // }

    // wait for the backlogCv.notify_one() in Notify to ping
    // this wait to continue execution to avoid race-conditions
    backlogCv.wait(lock, [this] {
        bool hasWork    = !backlog.empty();
        bool scansFinished = unitsScaned.load(std::memory_order_acquire);
        // when true stop waiting
        return hasWork || scansFinished;
    });

    *worker = nullptr;
    if (backlog.empty()) return;

    *worker = backlog.front();
    backlog.pop();
};

void SrcSliceHandler::ManageThreads() {
    std::vector<std::thread> pool;
    pool.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        pool.emplace_back([this] {
            // thread body (main)
            SrcSliceWorker* w = nullptr;
            for (;;) {
                // ensures that if there are jobs in
                // the backlog that theyre inserted
                // in the pool and conpleted
                replaceWorker(&w);
                if (w == nullptr) return;

                w->Perform();

                {
                    std::lock_guard<std::mutex> lk(mergeMutex);
                    consumeWorker(w);
                }
                delete w;
            }
        });
    }

    for (auto& job : pool) job.join();
}



std::string SrcSliceHandler::Profile2Str(const SliceProfile& profile) const {
    std::ostringstream out;

    out << "    \"file\":\"" << profile.file << "\"," << "\n";
    out << "    \"language\":\"" << profile.language << "\"," << "\n";
    
    out << "    \"namespace\":[";
    bool first = true;
    for (auto nameSpace : profile.containingNameSpaces) {
        if (!first) {
            out << ",";
        }
        out << "\"" << nameSpace.substr(0, nameSpace.find(' ')) << "\"";
        first = false;
    }
    out << "]," << "\n";

    out << "    \"class\":\"" << profile.nameOfContainingClass << "\"," << "\n";
    out << "    \"function\":\"" << profile.function << "\"," << "\n";
    out << "    \"type\":\"" << profile.variableType << "\"," << "\n";
    out << "    \"name\":\"" << profile.variableName << "\"," << "\n";
    
    out << "    \"decl\":" << profile.declPosition.StartToString() << "," << "\n";

    out << "    \"dependence\":[";
    first = true;
    for (auto dvar : profile.dvars) {
        if (!first) {
            out << ",";
        }
        out << "{\"" << dvar.first << "\":" << dvar.second.StartToString() << "}";
        first = false;
    }
    out << "]," << "\n";

    out << "    \"aliases\":[";
    first = true;
    for (auto alias : profile.aliases) {
        if (!first) {
            out << ",";
        }
        out << "{\"" << alias.first << "\":" << alias.second.StartToString() << "}";
        first = false;
    }
    out << "]," << "\n";

    out << "    \"calls\":[";
    first = true;
    for (auto cfunc : profile.cfunctions) {
        if (!first) {
            out << ",";
        }

        bool innerFirst = true;
        for (const CallBlob& call : callsMap.at(cfunc.callKey).calls) {
            if (!innerFirst) {
                out << ",";
            }
            out << "{" <<
                        "\"functionName\":\"" << call.functionName;
                        out << "\",\"parameter\":\"" << call.parameterIndex;
                        out << "\",\"definitionPosition\":" << call.funcPos.StartToString();
                        out << ",\"invoke\":" << call.invokePosition.StartToString() <<
                    "}";

            innerFirst = false;
        }

        first = false;
    }
    out << "]," << "\n";

    // "use": [ { "start": "2:10", "end": "2:17" }, ... ]
    out << "    \"use\":[";
    first = true;
    for (auto use : profile.uses) {
        if (!first) {
            out << ",";
        }
        out << use.StartToString();
        first = false;
    }
    out << "]," << "\n";

    // "definition": [ { "start": "2:10", "end": "2:17" }, ... ]
    out << "    \"definition\":[";
    first = true;
    for (auto def : profile.definitions) {
        if (!first) {
            out << ",";
        }
        out << def.StartToString();
        first = false;
    }

    if (profile.showControlEdges) {
        out << "]," << "\n";

        out << "    \"controlEdges\":[";
        first = true;
        for (auto edge : profile.controlEdges) {
            if (!first) {
                out << ",";
            }
            out << "[" << edge.first.StartToString() <<
            "," << edge.second.StartToString() << "]";
            first = false;
        }
        out << "]" << "\n";
    } else {
        out << "]" << "\n";
    }

    return out.str();
}

void SrcSliceHandler::ToJSON(std::ostream& out, bool strip_hash) const {
    bool firstWrite = true;

    // start of JSON obj
    out << "{\n";
    
    for (auto& profiles : profileMap) {
        for (auto& slice : profiles.second) {
            if (!slice.containsDeclaration)
                continue;

            if (!firstWrite)
                out << ",\n";
            firstWrite = false;

            std::string name(slice.variableName + '-' + slice.declPosition.ToNameString() + '-' + slice.checksum);

            std::string profileIdentifier = (strip_hash) ? slice.shortKey() : slice.jsonKey();

            out << "\"" << profileIdentifier << "\":{\n";
            out << Profile2Str(slice);
            out << "}";
        }
    }

    // end of JSON obj
    out << "\n}";
}



std::optional<std::vector<SliceProfile>::iterator> SrcSliceHandler::ArgumentProfile(const std::string& funcName, FunctionSignatureData& funcSig, int paramIndex) {
    std::optional<std::vector<SliceProfile>::iterator> profile;

    if (
        funcSig.parameters.empty() ||
        paramIndex < 0 ||
        paramIndex >= funcSig.parameters.size()
    ) return profile;

    const auto& target = funcSig.parameters[paramIndex];
    if (!target || !target->name) return profile;

    auto sliceGroup = profileMap.find(target->name.ToString());
    if (sliceGroup == profileMap.end()) {
        if (verboseMode) {
            std::cout << "[-] " << __FUNCTION__ << ":" << __LINE__ << " | Could not find SliceProfile for parameter '"
            << target->name.ToString() << "' of function '" << funcName << "'" << "\n";
        }
        return profile;
    }

    for (
        auto spi = sliceGroup->second.begin();
        spi != sliceGroup->second.end();
        ++spi
    ) {
        // attempt to finger-print based on contained file
        if (spi->file != funcSig.currentFilePath)
            continue;

        // attempt to finger-print based on decl position
        SlicePosition paramPos(
            funcSig.parameters.at(paramIndex)->name->startPosition,
            funcSig.parameters.at(paramIndex)->name->endPosition,
            funcSig.currentFilePath
        );
        if (spi->declPosition != paramPos)
            continue;

        profile = spi;
        if (spi->visited) return profile;
        ResolveCalls(*spi);
        break;
    }

    return profile;
}



void SrcSliceHandler::Stage2() {
    if (verboseMode || progressMode) std::cout << "[*] Performing Stage-2" << "\n";

    /** Merge Fragement Slices + Update as many undefined calls as possible */

    for (auto& [name, profiles] : profileMap) {
        mergeFragments(profiles);
    }

    /*
        There will only be one blob per key in the map
        meaning we only care about the first index of
        the entry's vector.

        resolveCalls will append resolved CallBlobs into
        that vector which is later printed by the handler.
    */
    for (auto& entry : callsMap) {
        if (!entry.second.calls.front().funcPos.HasDefinition()) {
            UpdateCalls(entry.second.calls);
        }
        
        // for overloaded situations the number of Slice profile iterators
        // will match the size of the number of calls
        entry.second.paramSlices.resize(entry.second.calls.size());
    }

    if (verboseMode || progressMode) std::cout << "[*] Finished Stage-2" << "\n";
}

void SrcSliceHandler::mergeFragments(std::vector<SliceProfile>& profiles) {
    typedef std::vector<SliceProfile>::iterator Spi;
    std::unordered_map<std::string, std::vector<Spi>> possibleRootFragments;

    // move fragments to the back so roots are at the front
    std::partition(profiles.begin(), profiles.end(), [](const SliceProfile& p) {
        // root fragments remain at the front of the vector
        // while fragement profiles are moved to the back
        // to ensure upon iteration we mark all root fragments
        // before handling a fragment
        return !p.isFragment;
    });
    
    for (auto itr = profiles.begin(); itr != profiles.end();) {
        if (itr->isFragment) {
            // find the root fragment and merge data
            // then remove fragment and continue
            auto dFiles = fileDependencyTable.find(itr->file);

            auto mergeFragment = [&possibleRootFragments](Spi itr, std::vector<std::string> includedFiles) {
                for (auto& [baseFile, rootFragments] : possibleRootFragments) {
                    for (auto& rootFragment : rootFragments) {
                        // root fragment and partial fragment exist in the same file
                        if (rootFragment->file == itr->file) {
                            rootFragment->merge(*itr);
                            return;
                        }

                        // inclusions in the file the fragment originates from
                        for (auto& includeName : includedFiles) {
                            // search from end of rootFragment file string
                            // if includeName substring occurs (C++20 string)
                            if (rootFragment->file.ends_with(includeName)) {
                                // merge fragment (itr) with rootFragment
                                rootFragment->merge(*itr);
                                return;
                            }
                        }
                    }
                }
            };
            mergeFragment(itr, (dFiles != fileDependencyTable.end()) ? dFiles->second : std::vector<std::string>{});
            itr = profiles.erase(itr);
        } else {
            // check if profile is potential root
            if (itr->isGlobal || itr->classMemberVar) {
                possibleRootFragments[itr->file].push_back(itr);
            }
            ++itr;
        }
    }
}

void SrcSliceHandler::UpdateCalls(std::vector<CallBlob>& calls) {
    CallBlob& callData = calls.front();
    if (callData.funcPos.HasDefinition()) return;

    auto funcSig = functionSigMap.find(
        callData.functionName
    );

    if (funcSig == functionSigMap.end()) return;

    if (funcSig->second.size() == 1) {
        // if there is only one record of a function signature
        if (funcSig->second[0].parameters.empty()) return;
        if (callData.argumentCount > funcSig->second[0].parameters.size()) return;

        // update funcPos
        callData.funcPos = funcSig->second[0].position;
    } else {
        // if a function is overloaded

        std::vector<FunctionSignatureData> potentials;

        for (auto& funcData : funcSig->second) {
            if (callData.argumentCount == funcData.parameters.size()) {
                potentials.push_back(funcData);
            }
        }

        if (potentials.empty()) {
            /** try to catch methods with pre-defined parameters */
            for (auto& funcData : funcSig->second) {
                if (callData.argumentCount == 0 || callData.argumentCount >= funcData.parameters.size()) continue;
                // 1 <= argCount < paramCount
                potentials.push_back(funcData);
            }
        }
        if (potentials.empty()) return;

        // update the original call
        callData.funcPos = potentials[0].position;

        // insert additional calls
        for (size_t i = 1; i < potentials.size(); ++i) {
            calls.push_back(calls.back());
            calls.back().funcPos = potentials[i].position;
        }
    }
}



void SrcSliceHandler::Stage3() {
    if (verboseMode || progressMode)
        std::cout << "[*] Starting Stage-3" << "\n";
    
    // Handles Collecting Control-Edges
    if (calculateControlEdges) ComputeControlPaths();
    
    // populates Aliases attribute in slice profiles and
    // performs crude interprocedural to connect use/def data
    if (expandAliases) ComputeAliasInterprocedural();
    
    ComputeInterprocedural();

    if (verboseMode || progressMode)
        std::cout << "[*] Finished Stage-3" << "\n";
}

void SrcSliceHandler::ComputeInterprocedural() {
    for (int i = 0; i < iterations; ++i) {
        if (progressMode || verboseMode) 
            std::cout << "[!] Interprocedural-Iteration: " << i+1 << "/" << iterations << "\n";
        
        for (auto& [name, profiles] : profileMap) {
            for (auto& sp : profiles) {
                // ensure the resolve calls on
                // other iterations does not
                // instant-return
                if (i > 0) sp.visited = false;
                ResolveCalls(sp);
            }
        }
    }
}

void SrcSliceHandler::InheritProfile(SliceProfile& sp, std::vector<SliceProfile>::iterator argSliceItr) {
    // sp unions slice data from the argSliceItr because
    // sp is passed into some method and resolves to argSliceItr

    if (!argSliceItr->isReference && !argSliceItr->isPointer) {
        // pass by value
        sp.uses.insert(
            argSliceItr->definitions.begin(),
            argSliceItr->definitions.end());
    } else {
        // pass by reference
        sp.definitions.insert(
            argSliceItr->definitions.begin(),
            argSliceItr->definitions.end());
    }

    // ensure we do not remove the initial decl from a slices definitions set
    if (sp.declPosition != argSliceItr->declPosition) {
        // Parameter initial declaration def line is considered a use towards the argument
        sp.definitions.erase(argSliceItr->declPosition);
    }
    sp.uses.insert(argSliceItr->declPosition);

    sp.uses.insert(
        argSliceItr->uses.begin(),
        argSliceItr->uses.end());

    // ensure dependencies and aliases are within local-scope
    if (sp.function == argSliceItr->function) {
        sp.aliases.insert(
            argSliceItr->aliases.begin(),
            argSliceItr->aliases.end()
        );
        sp.dvars.insert(
            argSliceItr->dvars.begin(),
            argSliceItr->dvars.end()
        );
    }

    // move the control edges over
    sp.controlEdges.insert(
        argSliceItr->controlEdges.begin(),
        argSliceItr->controlEdges.end());
}

void SrcSliceHandler::ResolveCalls(SliceProfile &sp) {
    // safety to ensure we avoid circular profile linkage
    if (sp.visited) return;
    // ignore profiles with no value
    if ((sp.variableName == "*LITERAL*")) return;

    // label the profile as visited to avoid circular profile linkage
    sp.visited = true;

    if (sp.cfunctions.empty()) return;

    // set of call data an argument (sp) inherits
    // from its associated parameter ( ArgumentProfile(...) )
    std::set<FunctionCallData> callsToInsert;

    for (auto& cfunc : sp.cfunctions) {
        // if a cfunc ignore flag is enabled skip this index and continue
        if (cfunc.ignore)
            continue;

        auto& paramData = callsMap.at(cfunc.callKey);

        for (size_t i = 0; i < paramData.calls.size(); ++i) {
            auto& data = paramData.calls[i];
            auto& paramSlice = paramData.paramSlices[i];

            if (!paramSlice.has_value()) {
                auto funcSigCollection = functionSigMap.find(data.functionName);

                // no func. sig. group found
                if (funcSigCollection == functionSigMap.end()) {
                    if (verboseMode) {
                        std::cout << "[-] " << __FUNCTION__ << ":" << __LINE__ <<
                        " | Cannot find Function Signature Collection for '" << data.functionName << "'" << "\n";
                    }
                    continue;
                }
                
                // Attempt to fingerprint the right signature based on
                // function call definition line and called function
                // def line data
                auto funcItr = std::find_if(
                    funcSigCollection->second.begin(),
                    funcSigCollection->second.end(),
                    [&data](const FunctionSignatureData& sig) {
                        return !sig.name.empty() &&
                               !sig.parameters.empty() &&
                               data.funcPos == sig.position;
                    }
                );
        
                if (funcItr == funcSigCollection->second.end()) continue;
        
                std::string simpleFunctionName = SrcSliceOperations::GetSimpleFunctionName(funcItr->name);
        
                unsigned int ArgProfParam = data.parameterIndex - 1;
        
                // Ensure before we run ArgumentProfile that parameters has non-zero size and can be indexed safely
        
                /*** @todo look for potential issues with overload case */
        
                bool matchingName = data.functionName == simpleFunctionName;
                bool validArgIndex = ArgProfParam < funcItr->parameters.size();
        
                if (!matchingName || !validArgIndex) {
                    continue;
                }
        
                if (!funcItr->parameters[ArgProfParam]->name) {
                    continue;
                }
        
                // Only run this section if the parameter name can be extracted
                paramSlice = ArgumentProfile(
                    data.functionName,
                    *funcItr,
                    ArgProfParam
                );
        
                if (!paramSlice.has_value()) {
                    if (verboseMode) {
                        std::cout << "[-] " << __FUNCTION__ << ":" << __LINE__ << " | ArgumentProfile could not resolve SliceProfile for Call '"
                        << data.functionName << "[" << ArgProfParam << "]" << "'" << "\n";
                    }
                    continue;
                }
            }

            auto argSliceItr = paramSlice.value();

            InheritProfile(sp, argSliceItr);

            // after iterating all cfuncs we move over
            // cfunc data from these complement profiles
            // to avoid extra iterations over visited calls &
            // not invalidate the loop
            for (const auto& cFunc : argSliceItr->cfunctions) {
                callsToInsert.insert(cFunc);
            }
        }
    }

    // move cfunc data over
    for (const auto& cFunc : callsToInsert) {
        sp.cfunctions.insert(cFunc);
    }
}

// Similiar to ComputeInterprocedural but focuses on building
// a more detailed alias listing for slices of pointers
void SrcSliceHandler::ComputeAliasInterprocedural() {
    // speed problems origin
    std::unordered_set <std::string> visited_alias;

    for (auto& sliceGroup : profileMap) {
        // expand list of potential targets (aliases)
        for (auto& sp : sliceGroup.second) {
            if (!sp.containsDeclaration) continue;
            for (auto& alias : sp.aliases) {
                // check if the alias has been visited
                if (visited_alias.find(alias.first) != visited_alias.end())
                    break;

                // view aliases of the slice profile
                auto aliasProfileGroup = profileMap.find(alias.first);
                if (aliasProfileGroup == profileMap.end()) continue;

                // fingerprint the profile based on contained use
                for (auto& aspi : aliasProfileGroup->second) {
                    if (!aspi.containsDeclaration) continue;
                    
                    auto usesItr = aspi.uses.find(alias.second);
                    if (usesItr == aspi.uses.end()) continue;
                    
                    // determine if the potential target is a pointer or reference
                    if (!aspi.isPointer && !aspi.isReference) continue;

                    // mark alias as visited so we dont review this alias entry again (circular dependence protection)
                    visited_alias.insert(aspi.jsonKey());

                    // push_back alias slice profile's aliases into the source slice aliases
                    sp.aliases.insert(aspi.aliases.begin(), aspi.aliases.end());
                    break;
                }
            }
        }
    }
}




// Find forward control-edges between conditional conditions. ie: if -> else if -> else
void SrcSliceHandler::ComputeOuterPaths(std::set<std::pair<SlicePosition,SlicePosition>>& controlEdges, std::vector<SlicePosition>& sLines) {
    /** @todo
     * Checking metadata within each SlicePosition, connect sline[i] to a sline[k] where k > i
     * corresponding if conditions to preceding elif conditions or the first sline within an else
    */

    for (size_t i = 0; i < sLines.size() - 1; ++i) {
        // sline[i] must be within the condition block of an if or else-if
        if (!sLines[i].GetData().isIfCondition && !sLines[i].GetData().isElifCondition)
            continue;

        // find the condition context base
        SlicePosition baseBlock;
        for (SlicePosition& data : ifdata) {
            if (sLines[i] < data) continue;
            if (IsContained(sLines[i], data)) {
                baseBlock = data;
            }
        }
        for (SlicePosition& data : elseifdata) {
            if (sLines[i] < data) continue;
            if (IsContained(sLines[i], data)) {
                baseBlock = data;
            }
        }

        if (baseBlock.GetFileName().empty()) continue;

        // find the preceding sline[k]
        size_t k = i+1;
        while (k < sLines.size()) {
            if (!IsContained(sLines[k], baseBlock)) break;
            ++k;
        }
        if (k == sLines.size()) continue;
        
        controlEdges.insert(std::make_pair(sLines[i], sLines[k]));
    }
}

// Collects the forward exiting control-flows of conditionals. ie: exiting conditional body
void SrcSliceHandler::ComputeExitPaths(std::set<std::pair<SlicePosition,SlicePosition>>& controlEdges, std::vector<SlicePosition>& sLines) {
    /** @todo
     * Find the context if_stmt the sline[i] is within to find the first sline[k] not contained in the context if_stmt where k > i
     * sline[i] must be the sline that occurs LAST within the inner context of the if_stmt context (specific if, elif, else block)
    */

    for (size_t i = 0; i < sLines.size() - 1; ++i) {
        // skip slines that are within the conditions
        if (sLines[i].GetData().isIfCondition || sLines[i].GetData().isElifCondition)
            continue;

        // sline[i] must be contained within the block of either an if, else-if, or else statement
        size_t ctxIndex = FindContextBlock(sLines[i], ifStmts);
        if (ctxIndex == -1) continue;

        // find inner-most context of sline[i]
        SlicePosition innerContext;
        for (SlicePosition& data : ifdata) {
            if (IsContained(sLines[i], data)) {
                innerContext = data;
            }
        }
        for (SlicePosition& data : elseifdata) {
            // if the inner context has been found skip this loop
            if (!innerContext.HasDefinition()) break;

            if (IsContained(sLines[i], data)) {
                innerContext = data;
            }
        }
        for (SlicePosition& data : elsedata) {
            // if the inner context has been found skip this loop
            if (!innerContext.HasDefinition()) break;

            if (IsContained(sLines[i], data)) {
                innerContext = data;
            }
        }

        // check if sline[i] is the last sline within the inner-most context
        bool isExitPath = false;
        size_t k = i+1;
        while (k < sLines.size()) {
            // if sline[k] is contained within the inner-most context
            // then sline[i], sline[k] is not an exit-path
            if (IsContained(sLines[k], innerContext)) {
                break;
            }

            // check if sline[k] is outside of the if_stmt context
            if (!IsContained(sLines[k], ifStmts[ctxIndex])) {
                isExitPath = true;
                break;
            }

            ++k;
        }
        if (!isExitPath) continue;

        
        controlEdges.insert(std::make_pair(sLines[i], sLines[k]));
    }
}

// srcSlice focuses on Forward-Slicing, therefor our Control-Flows are going to be forward-flowing
// we are not focusing on backwards-flows.
void SrcSliceHandler::ComputeControlPaths() {
    auto InBlock = [this](SlicePosition& sline, std::vector<SlicePosition>& groups) -> bool {
        // groups are focused on if, elif, else position ranges
        size_t ctxIndex = -1;
        if (groups.size() > 1) {
            ctxIndex = FindContextBlock(sline, ifStmts);
            if (ctxIndex == -1) return false;
        }

        for (auto& data : groups) {
            // if there is nesting we only want to be concerned with data related to the if_stmt context
            if (ctxIndex != -1 && IsContained(ifStmts[ctxIndex], data)) {
                continue;
            }

            if (IsContained(sline, data)) {
                return true;
            }
        }
        return false;
    };
    
    auto InLoop = [](SlicePosition& sline, std::vector<SlicePosition>& loopdata) -> bool {
        size_t ctxIndex = -1;
        if (loopdata.size() > 1) {
            ctxIndex = FindContextBlock(sline, loopdata);
            if (ctxIndex == -1) return false;
        }

        for (auto& data : loopdata) {
            if (ctxIndex != -1 && IsContained(loopdata[ctxIndex], data)) {
                continue;
            }

            if (IsContained(sline, data)) {
                return true;
            }
        }
        return false;
    };

    for (auto& varGroup : profileMap) {
        for (auto& profile : varGroup.second) {
            if (!profile.containsDeclaration) continue;

            // Collect the slice lines and put them in numerical order
            std::set<SlicePosition> sLinesOrdered;
            sLinesOrdered.insert(profile.definitions.begin(), profile.definitions.end());
            sLinesOrdered.insert(profile.uses.begin(), profile.uses.end());

            // Convert set into vector using vector ctor
            std::vector<SlicePosition> sLines(sLinesOrdered.begin(), sLinesOrdered.end());

            // Handles controledges based from: for-loops, while-loops, do-while-loops
            for (auto& loop : loopdata) {
                SlicePosition predecessor;    // origin position
                SlicePosition falseSuccessor; // where to go when loop-condition is false
                SlicePosition trueSuccessor;  // where to go when loop-condition is true (first sline within loop block)

                for (auto& sline : sLines) {
                    // see if the sline is before the loop or in the loop-control statement
                    if (predecessor.GetFileName().empty() && IsContained(sline, loop)) {
                        predecessor = sline;
                        continue;
                    }

                    // find the first sline outside the loop-block
                    if (falseSuccessor.GetFileName().empty() && sline > loop && !IsContained(sline, loop)) {
                        falseSuccessor = sline;
                        continue;
                    }

                    // find the first sline inside the loop-block
                    if (trueSuccessor.GetFileName().empty() && IsContained(sline, loop)) {
                        trueSuccessor = sline;
                        continue;
                    }
                }

                bool validPositions = !predecessor.GetFileName().empty() && !trueSuccessor.GetFileName().empty() && !falseSuccessor.GetFileName().empty();
                if (validPositions && predecessor < falseSuccessor) {
                    profile.controlEdges.insert(
                        std::make_pair(predecessor, trueSuccessor)
                    );
                    profile.controlEdges.insert(
                        std::make_pair(predecessor, falseSuccessor)
                    );
                }
            }

            // Create sline pairs between successive lines with case of if-else matching
            for (size_t i = 0; i < sLines.size() - 1; ++i) {
                if ( !(InBlock(sLines[i], ifdata) && (InBlock(sLines[i+1], elseifdata) || InBlock(sLines[i+1], elsedata)) ) ) {
                    profile.controlEdges.insert(
                        std::make_pair(sLines[i], sLines[i+1])
                    );
                }

                // if the sline[i] does not belong to any loop or condition block
                // then connect this sline to the first successive sline[k] that
                // does not also belong to any loop or condiiton block
                if (!InLoop(sLines[i], loopdata) && !InBlock(sLines[i], ifdata) && !InBlock(sLines[i], elseifdata) && !InBlock(sLines[i], elsedata)) {
                    profile.controlEdges.insert(
                        std::make_pair(sLines[i], sLines[i+1])
                    );
                }
            }

            // Find Exit-Paths and Outer-Paths that are not initially found from this algorithm
            ComputeExitPaths(profile.controlEdges, sLines);
            ComputeOuterPaths(profile.controlEdges, sLines);
        }
    }
}
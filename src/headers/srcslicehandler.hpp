// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicehandler.hpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#ifndef SRCSLICEHANDLER
#define SRCSLICEHANDLER

#include <srcsliceprofile.hpp>
#include <srcsliceprogress.hpp>
#include <srcsliceparse.hpp>
#include <srcsliceworker.hpp>

#include <condition_variable>
#include <mutex>
#include <queue>

#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <CLI11.hpp>

#include <srcDispatcher.hpp>
#include <srcSAXHandler.hpp>

#include <IfStmtPolicy.hpp>
#include <SwitchPolicy.hpp>
#include <WhilePolicy.hpp>
#include <ForPolicy.hpp>
#include <DoPolicy.hpp>
#include <TryPolicy.hpp>
#include <FunctionPolicy.hpp>
#include <ClassPolicy.hpp>
#include <UnitPolicy.hpp>

#if _WIN64
    #include <windows.h>
#elif __APPLE__
    #include <sys/types.h>
    #include <sys/sysctl.h>
#else
    #include <sched.h>
#endif

int CPUCount();

// type aliasing
typedef std::vector<srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::DeclStmtData>>> DeclStmts;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::DeclData>> DeclInfo;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::ExpressionData>> ExprInfo;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::FunctionData>> FunctionInfo;
typedef srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::BlockData>> BlockInfo;
typedef std::vector<srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::FunctionData>>> Functions;
typedef std::vector<srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::ClassData>>> Classes;

inline std::atomic_bool unitsScaned{false};

struct CliInfo {
    std::string inputFile;
    std::string outputFile;
    int threadCount;
    bool calculateControlEdges;
    int iterations;
    bool expandAliases;
    bool verboseMode;
    bool progressMode;
};
struct TestArg {
    bool calculateControlEdges;
    bool expandAliases;
};

class SrcSliceHandler : public srcDispatch::PolicyListener {
public:
    ~SrcSliceHandler();

    // Use information from cli (main.cpp)
    SrcSliceHandler(const CliInfo& info);

    // Use string srcml buffer ctor of srcSAXController
    SrcSliceHandler(std::string& sourceCodeStr, const TestArg& info);

    void Notify(const srcDispatch::PolicyDispatcher *policy, const srcDispatch::srcSAXEventContext &ctx) override;

    void NotifyWrite(const srcDispatch::PolicyDispatcher *policy [[maybe_unused]], srcDispatch::srcSAXEventContext &ctx [[maybe_unused]]) {};

    void ToJSON(std::ostream& out, bool strip_hash = false) const;

private:
    /*
        srcSlice focuses on Forward-Slicing, therefor our Control-Flows are going to be forward-flowing
        we are not focusing on backwards-flows.
    */
    void ComputeControlPaths();
    void ComputeOuterPaths(std::set<std::pair<SlicePosition,SlicePosition>>& controlEdges, std::vector<SlicePosition>& sLines);
    void ComputeExitPaths(std::set<std::pair<SlicePosition,SlicePosition>>& controlEdges, std::vector<SlicePosition>& sLines);

    std::optional<std::vector<SliceProfile>::iterator> ArgumentProfile(const std::string& funcName, FunctionSignatureData& funcSig, int paramIndex);

    // Need to track Aliases we have already read through
    // InterProcedural from the normal call should also be reflected
    // if a profile with alias(s) is used in function calls
    void ComputeAliasInterprocedural();
    void ComputeInterprocedural();

    void InheritProfile(SliceProfile& sp, std::vector<SliceProfile>::iterator argSliceItr);
    
    void Stage2();
    void Stage3();

    void mergeFragments(std::vector<SliceProfile>& profiles);
    void UpdateCalls(std::vector<CallBlob>& calls);
    void ResolveCalls(SliceProfile& sp);

    // thread target for managing SrcSliceWorker jobs
    void ManageThreads();
    void replaceWorker(SrcSliceWorker** worker);
    // merge the workers sub-structure into the main-structure in the handler
    void consumeWorker(SrcSliceWorker* worker);

    void WriteProfile(std::ostream& out, const SliceProfile& profile) const;
    

    std::unordered_map<std::string, std::vector<SliceProfile>> profileMap;
    std::unordered_map<size_t, ParamSlice> callsMap;
    std::unordered_set <std::string> visited_func;

    std::vector<SlicePosition> loopdata;
    std::vector<SlicePosition> forloopdata;
    std::vector<SlicePosition> whileloopdata;
    std::vector<SlicePosition> dowhileloopdata;

    std::vector<SlicePosition> ifdata;
    std::vector<SlicePosition> ifStmts;
    std::vector<SlicePosition> elseifdata;
    std::vector<SlicePosition> elsedata;

    std::unordered_map<std::string, std::vector<FunctionSignatureData>> functionSigMap;

    bool verboseMode = false;
    bool calculateControlEdges = false;
    bool progressMode = false;
    bool expandAliases = false;

    int iterations = 1;

    std::mutex backlogMutex;
    std::condition_variable backlogCv;

    std::mutex mergeMutex;

    int threadCount;
    std::queue<SrcSliceWorker*> backlog;

    std::unordered_map<std::string, std::vector<std::string>> fileDependencyTable;

    SliceCtx sctx;
};

#endif
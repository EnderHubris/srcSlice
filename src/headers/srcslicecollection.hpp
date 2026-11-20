// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file srcslicecollection.hpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#ifndef SRCSLICECOLLETION
#define SRCSLICECOLLETION

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <utility>
#include <memory>
#include <any>
#include <set>
#include <exception>

#include <srcSAXHandler.hpp>
#include <srcDispatcher.hpp>
#include <FunctionPolicy.hpp>
#include <ClassPolicy.hpp>
#include <UnitPolicy.hpp>

struct PositionMeta {
    bool isIfCondition = false;
    bool isElifCondition = false;
};

class SlicePosition {
public:
    SlicePosition();
    SlicePosition(srcDispatch::DeltaElement<srcDispatch::Position> start,
            srcDispatch::DeltaElement<srcDispatch::Position> end,
            std::string filename);

    SlicePosition(const SlicePosition& position);
    SlicePosition& operator=(const SlicePosition& rhs);

    bool operator==(const SlicePosition& rhs) const;
    bool operator!=(const SlicePosition& rhs) const;

    bool operator<(const SlicePosition& rhs) const;
    bool operator<=(const SlicePosition& rhs) const;
    bool operator>(const SlicePosition& rhs) const;
    bool operator>=(const SlicePosition& rhs) const;

    std::string StartToString() const;
    std::string EndToString() const;

    std::string RangeToString() const;
    std::string ToNameString() const;
    srcDispatch::DeltaElement<srcDispatch::Position> GetStart() const;
    srcDispatch::DeltaElement<srcDispatch::Position> GetEnd() const;
    std::string GetFileName() const;
    bool HasDefinition() const;
    PositionMeta& GetData();
private:
    std::string filename;
    srcDispatch::DeltaElement<srcDispatch::Position> start;
    srcDispatch::DeltaElement<srcDispatch::Position> end;
    PositionMeta data;
};

// return if SlicePosition a is contained in the scope of SlicePosition b
bool IsContained(SlicePosition& a, SlicePosition b);

// return distance between SlicePosition a from b
size_t GetDistance(SlicePosition& a, SlicePosition& b);

// return index to a group element context that a sline is contained in
// i.e if group = if_stmt group, return index to if_stmt the sline is contained in
size_t FindContextBlock(SlicePosition& sline, std::vector<SlicePosition>& group);

// Hold context data srcslice uses based off srcSAXEventContext without copying
// the entirety of srcSAXEventContext
class SliceCtx {
public:
    SliceCtx(): defined(false) {}
    SliceCtx(const srcDispatch::srcSAXEventContext& ctx);

    SliceCtx(const SliceCtx& rhs);

    std::string currentFilePath;
    std::string currentFileChecksum;
    std::string currentFileLanguage;
    std::vector<std::string> containingNamespaces;
    bool defined = false;
};

// Store Data about Function Signatures by grouping functions of the same name
// like Slices in profileMap
class FunctionSignatureData {
public:
    FunctionSignatureData(){};
    FunctionSignatureData(srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::FunctionData>>& func,
                            std::string className, bool blockDefined, const SliceCtx& ctx);
    SlicePosition position;
    std::string name;
    std::string returnType;
    std::vector<srcDispatch::DeltaElement<std::shared_ptr<srcDispatch::DeclData>>> parameters;
    std::string containingClass;
    std::string currentFilePath;
    std::string currentFileChecksum;
    std::string currentFileLanguage;
    std::vector<std::string> containingNamespaces;
    bool isDefined;
};

struct CallBlob {
    SlicePosition invokePosition;
    SlicePosition funcPos;
    std::string functionName;
    unsigned int parameterIndex;
    unsigned int argumentCount;

    std::string ToString() const {
        std::ostringstream os;
        os << functionName << "_"
        << invokePosition.StartToString() << "_"
        << parameterIndex << "_"
        << argumentCount << "_"
        << funcPos.StartToString();
        return os.str();
    }
};

// Store meta-data about a function-call by tracking:
// name of the function being called, line where the call is made, index of the parameter,
// and the line where the function is potentially defined
class FunctionCallData {
public:
    FunctionCallData(){}
    FunctionCallData(
        size_t dataHash,
        bool ignore_ = false
    );

    FunctionCallData(const FunctionCallData& rhs);

    FunctionCallData& operator=(const FunctionCallData& rhs);

    bool operator==(const FunctionCallData& rhs) const;
    bool operator!=(const FunctionCallData& rhs) const;

    bool operator<(const FunctionCallData& rhs) const;
    bool operator>(const FunctionCallData& rhs) const;

    
    size_t callKey; // std::hash of the string representation of CallBlob

    // general metadata not displayed to the user
    bool ignore = false;
};

#endif
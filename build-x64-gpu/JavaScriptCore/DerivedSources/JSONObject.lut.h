// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/JSONObject.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex jsonTableIndex[4] = {
    { -1, -1 },
    { 0, -1 },
    { -1, -1 },
    { 1, -1 },
};

static constinit const struct HashTableValue jsonTableValues[2] = {
   { "parse"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, jsonProtoFuncParse, 2 } },
   { "stringify"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, jsonProtoFuncStringify, 3 } },
};

static constinit const struct HashTable jsonTable =
    { 2, 3, false, nullptr, jsonTableValues, jsonTableIndex };

} // namespace JSC

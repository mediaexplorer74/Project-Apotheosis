// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/NumberPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex numberPrototypeTableIndex[17] = {
    { -1, -1 },
    { -1, -1 },
    { 1, 16 },
    { 4, -1 },
    { 3, -1 },
    { -1, -1 },
    { 0, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 2, -1 },
};

static constinit const struct HashTableValue numberPrototypeTableValues[5] = {
   { "toLocaleString"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, numberProtoFuncToLocaleString, 0 } },
   { "valueOf"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, numberProtoFuncValueOf, 0 } },
   { "toFixed"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, numberProtoFuncToFixed, 1 } },
   { "toExponential"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, numberProtoFuncToExponential, 1 } },
   { "toPrecision"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, numberProtoFuncToPrecision, 1 } },
};

static constinit const struct HashTable numberPrototypeTable =
    { 5, 15, false, nullptr, numberPrototypeTableValues, numberPrototypeTableIndex };

} // namespace JSC

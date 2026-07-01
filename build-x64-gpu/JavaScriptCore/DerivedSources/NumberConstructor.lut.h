// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/NumberConstructor.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex numberConstructorTableIndex[8] = {
    { -1, -1 },
    { 2, -1 },
    { -1, -1 },
    { 0, -1 },
    { -1, -1 },
    { 1, -1 },
    { -1, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue numberConstructorTableValues[3] = {
   { "isFinite"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NumberIsFiniteIntrinsic, { HashTableValue::NativeFunctionType, numberConstructorFuncIsFinite, 1 } },
   { "isNaN"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NumberIsNaNIntrinsic, { HashTableValue::NativeFunctionType, numberConstructorFuncIsNaN, 1 } },
   { "isSafeInteger"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NumberIsSafeIntegerIntrinsic, { HashTableValue::NativeFunctionType, numberConstructorFuncIsSafeInteger, 1 } },
};

static constinit const struct HashTable numberConstructorTable =
    { 3, 7, false, nullptr, numberConstructorTableValues, numberConstructorTableIndex };

} // namespace JSC

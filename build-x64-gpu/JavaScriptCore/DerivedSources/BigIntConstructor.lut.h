// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/BigIntConstructor.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex bigIntConstructorTableIndex[4] = {
    { 1, -1 },
    { 0, -1 },
    { -1, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue bigIntConstructorTableValues[2] = {
   { "asUintN"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, bigIntConstructorFuncAsUintN, 2 } },
   { "asIntN"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, bigIntConstructorFuncAsIntN, 2 } },
};

static constinit const struct HashTable bigIntConstructorTable =
    { 2, 3, false, nullptr, bigIntConstructorTableValues, bigIntConstructorTableIndex };

} // namespace JSC

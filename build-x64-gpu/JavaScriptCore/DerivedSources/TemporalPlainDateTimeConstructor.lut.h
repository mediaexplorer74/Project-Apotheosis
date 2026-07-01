// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/TemporalPlainDateTimeConstructor.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex temporalPlainDateTimeConstructorTableIndex[4] = {
    { 1, -1 },
    { -1, -1 },
    { 0, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue temporalPlainDateTimeConstructorTableValues[2] = {
   { "from"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalPlainDateTimeConstructorFuncFrom, 1 } },
   { "compare"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalPlainDateTimeConstructorFuncCompare, 2 } },
};

static constinit const struct HashTable temporalPlainDateTimeConstructorTable =
    { 2, 3, false, nullptr, temporalPlainDateTimeConstructorTableValues, temporalPlainDateTimeConstructorTableIndex };

} // namespace JSC

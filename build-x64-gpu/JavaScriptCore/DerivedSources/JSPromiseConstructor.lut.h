// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/JSPromiseConstructor.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex promiseConstructorTableIndex[18] = {
    { 2, -1 },
    { -1, -1 },
    { 0, 16 },
    { -1, -1 },
    { 6, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 4, 17 },
    { -1, -1 },
    { -1, -1 },
    { 1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 3, -1 },
    { 5, -1 },
};

static constinit const struct HashTableValue promiseConstructorTableValues[7] = {
   { "resolve"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), PromiseConstructorResolveIntrinsic, { HashTableValue::NativeFunctionType, promiseConstructorFuncResolve, 1 } },
   { "reject"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), PromiseConstructorRejectIntrinsic, { HashTableValue::NativeFunctionType, promiseConstructorFuncReject, 1 } },
   { "race"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, promiseConstructorFuncRace, 1 } },
   { "all"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, promiseConstructorFuncAll, 1 } },
   { "allSettled"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, promiseConstructorFuncAllSettled, 1 } },
   { "any"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, promiseConstructorFuncAny, 1 } },
   { "withResolvers"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, promiseConstructorFuncWithResolvers, 0 } },
};

static constinit const struct HashTable promiseConstructorTable =
    { 7, 15, false, nullptr, promiseConstructorTableValues, promiseConstructorTableIndex };

} // namespace JSC

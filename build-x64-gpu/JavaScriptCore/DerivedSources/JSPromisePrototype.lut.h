// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/JSPromisePrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex promisePrototypeTableIndex[2] = {
    { -1, -1 },
    { 0, -1 },
};

static constinit const struct HashTableValue promisePrototypeTableValues[1] = {
   { "finally"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, promiseProtoFuncFinally, 1 } },
};

static constinit const struct HashTable promisePrototypeTable =
    { 1, 1, false, nullptr, promisePrototypeTableValues, promisePrototypeTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/AsyncFromSyncIteratorPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "JSCBuiltins.h"
#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex asyncFromSyncIteratorPrototypeTableIndex[8] = {
    { -1, -1 },
    { 0, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 2, -1 },
    { 1, -1 },
};

static constinit const struct HashTableValue asyncFromSyncIteratorPrototypeTableValues[3] = {
   { "next"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, asyncFromSyncIteratorPrototypeNextCodeGenerator, 1 } },
   { "return"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, asyncFromSyncIteratorPrototypeReturnCodeGenerator, 1 } },
   { "throw"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, asyncFromSyncIteratorPrototypeThrowCodeGenerator, 1 } },
};

static constinit const struct HashTable asyncFromSyncIteratorPrototypeTable =
    { 3, 7, false, nullptr, asyncFromSyncIteratorPrototypeTableValues, asyncFromSyncIteratorPrototypeTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/JSIteratorHelperPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "JSCBuiltins.h"
#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex jsIteratorHelperPrototypeTableIndex[4] = {
    { -1, -1 },
    { 0, -1 },
    { -1, -1 },
    { 1, -1 },
};

static constinit const struct HashTableValue jsIteratorHelperPrototypeTableValues[2] = {
   { "next"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, jsIteratorHelperPrototypeNextCodeGenerator, 0 } },
   { "return"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, jsIteratorHelperPrototypeReturnCodeGenerator, 0 } },
};

static constinit const struct HashTable jsIteratorHelperPrototypeTable =
    { 2, 3, false, nullptr, jsIteratorHelperPrototypeTableValues, jsIteratorHelperPrototypeTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/ArrayConstructor.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "JSCBuiltins.h"
#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex arrayConstructorTableIndex[2] = {
    { 0, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue arrayConstructorTableValues[1] = {
   { "from"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, arrayConstructorFromCodeGenerator, 1 } },
};

static constinit const struct HashTable arrayConstructorTable =
    { 1, 1, false, nullptr, arrayConstructorTableValues, arrayConstructorTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/ShadowRealmPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "JSCBuiltins.h"
#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex shadowRealmPrototypeTableIndex[5] = {
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 0, 4 },
    { 1, -1 },
};

static constinit const struct HashTableValue shadowRealmPrototypeTableValues[2] = {
   { "evaluate"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, shadowRealmPrototypeEvaluateCodeGenerator, 1 } },
   { "importValue"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, shadowRealmPrototypeImportValueCodeGenerator, 2 } },
};

static constinit const struct HashTable shadowRealmPrototypeTable =
    { 2, 3, false, nullptr, shadowRealmPrototypeTableValues, shadowRealmPrototypeTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/ErrorPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex errorPrototypeTableIndex[2] = {
    { 0, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue errorPrototypeTableValues[1] = {
   { "toString"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, errorProtoFuncToString, 0 } },
};

static constinit const struct HashTable errorPrototypeTable =
    { 1, 1, false, nullptr, errorPrototypeTableValues, errorPrototypeTableIndex };

} // namespace JSC

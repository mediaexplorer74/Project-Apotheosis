// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/wasm/js/WebAssemblyTablePrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex prototypeTableWebAssemblyTableIndex[16] = {
    { -1, -1 },
    { 0, -1 },
    { -1, -1 },
    { 1, -1 },
    { -1, -1 },
    { 2, -1 },
    { -1, -1 },
    { 3, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 4, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue prototypeTableWebAssemblyTableValues[5] = {
   { "length"_s, static_cast<unsigned>(PropertyAttribute::ReadOnly|PropertyAttribute::CustomAccessor), NoIntrinsic, { HashTableValue::GetterSetterType, webAssemblyTableProtoGetterLength, 0 } },
   { "grow"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyTableProtoFuncGrow, 1 } },
   { "get"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyTableProtoFuncGet, 1 } },
   { "set"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyTableProtoFuncSet, 1 } },
   { "type"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyTableProtoFuncType, 0 } },
};

static constinit const struct HashTable prototypeTableWebAssemblyTable =
    { 5, 15, true, nullptr, prototypeTableWebAssemblyTableValues, prototypeTableWebAssemblyTableIndex };

} // namespace JSC

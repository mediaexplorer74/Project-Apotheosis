// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/wasm/js/WebAssemblyMemoryPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex prototypeTableWebAssemblyMemoryIndex[10] = {
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 0, 8 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 1, 9 },
    { 2, -1 },
};

static constinit const struct HashTableValue prototypeTableWebAssemblyMemoryValues[3] = {
   { "grow"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyMemoryProtoFuncGrow, 1 } },
   { "buffer"_s, static_cast<unsigned>(PropertyAttribute::ReadOnly|PropertyAttribute::CustomAccessor), NoIntrinsic, { HashTableValue::GetterSetterType, webAssemblyMemoryProtoGetterBuffer, 0 } },
   { "type"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyMemoryProtoFuncType, 0 } },
};

static constinit const struct HashTable prototypeTableWebAssemblyMemory =
    { 3, 7, true, nullptr, prototypeTableWebAssemblyMemoryValues, prototypeTableWebAssemblyMemoryIndex };

} // namespace JSC

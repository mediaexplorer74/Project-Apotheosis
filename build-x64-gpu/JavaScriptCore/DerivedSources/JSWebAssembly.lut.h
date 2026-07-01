// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/wasm/js/JSWebAssembly.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex webAssemblyTableIndex[34] = {
    { -1, -1 },
    { -1, -1 },
    { 0, 32 },
    { -1, -1 },
    { -1, -1 },
    { 10, -1 },
    { 1, 33 },
    { 3, -1 },
    { -1, -1 },
    { -1, -1 },
    { 5, -1 },
    { 7, -1 },
    { -1, -1 },
    { 6, -1 },
    { 9, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 4, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 8, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 11, -1 },
    { 2, -1 },
    { 12, -1 },
};

static constinit const struct HashTableValue webAssemblyTableValues[13] = {
   { "CompileError"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyCompileError } },
   { "Exception"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyException } },
   { "Global"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyGlobal } },
   { "Instance"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyInstance } },
   { "LinkError"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyLinkError } },
   { "Memory"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyMemory } },
   { "Module"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyModule } },
   { "RuntimeError"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyRuntimeError } },
   { "Table"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyTable } },
   { "Tag"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::PropertyCallback), NoIntrinsic, { HashTableValue::LazyPropertyType, createWebAssemblyTag } },
   { "compile"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyCompileFunc, 1 } },
   { "instantiate"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyInstantiateFunc, 1 } },
   { "validate"_s, static_cast<unsigned>(PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, webAssemblyValidateFunc, 1 } },
};

static constinit const struct HashTable webAssemblyTable =
    { 13, 31, false, nullptr, webAssemblyTableValues, webAssemblyTableIndex };

} // namespace JSC

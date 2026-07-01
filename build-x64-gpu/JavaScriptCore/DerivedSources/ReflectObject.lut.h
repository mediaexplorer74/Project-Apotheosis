// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/ReflectObject.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "JSCBuiltins.h"
#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex reflectObjectTableIndex[35] = {
    { 12, -1 },
    { 1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 4, -1 },
    { -1, -1 },
    { 11, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 10, -1 },
    { 7, 34 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 2, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 5, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 3, 32 },
    { 0, 33 },
    { -1, -1 },
    { 6, -1 },
    { 8, -1 },
    { 9, -1 },
};

static constinit const struct HashTableValue reflectObjectTableValues[13] = {
   { "apply"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, reflectObjectApplyCodeGenerator, 3 } },
   { "construct"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectConstruct, 2 } },
   { "defineProperty"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectDefineProperty, 3 } },
   { "deleteProperty"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, reflectObjectDeletePropertyCodeGenerator, 2 } },
   { "get"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, reflectObjectGetCodeGenerator, 2 } },
   { "getOwnPropertyDescriptor"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectGetOwnPropertyDescriptor, 2 } },
   { "getPrototypeOf"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), ReflectGetPrototypeOfIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectGetPrototypeOf, 1 } },
   { "has"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, reflectObjectHasCodeGenerator, 2 } },
   { "isExtensible"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectIsExtensible, 1 } },
   { "ownKeys"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), ReflectOwnKeysIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectOwnKeys, 1 } },
   { "preventExtensions"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectPreventExtensions, 1 } },
   { "set"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectSet, 3 } },
   { "setPrototypeOf"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, reflectObjectSetPrototypeOf, 2 } },
};

static constinit const struct HashTable reflectObjectTable =
    { 13, 31, false, nullptr, reflectObjectTableValues, reflectObjectTableIndex };

} // namespace JSC

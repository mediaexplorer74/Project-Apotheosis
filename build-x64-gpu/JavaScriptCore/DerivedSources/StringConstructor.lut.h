// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/StringConstructor.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "JSCBuiltins.h"
#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex stringConstructorTableIndex[8] = {
    { -1, -1 },
    { 2, -1 },
    { -1, -1 },
    { -1, -1 },
    { 1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 0, -1 },
};

static constinit const struct HashTableValue stringConstructorTableValues[3] = {
   { "fromCharCode"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), FromCharCodeIntrinsic, { HashTableValue::NativeFunctionType, stringFromCharCode, 1 } },
   { "fromCodePoint"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, stringFromCodePoint, 1 } },
   { "raw"_s, ((static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function)) & ~PropertyAttribute::Function) | PropertyAttribute::Builtin, NoIntrinsic, { HashTableValue::BuiltinGeneratorType, stringConstructorRawCodeGenerator, 1 } },
};

static constinit const struct HashTable stringConstructorTable =
    { 3, 7, false, nullptr, stringConstructorTableValues, stringConstructorTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/IntlRelativeTimeFormatPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex relativeTimeFormatPrototypeTableIndex[9] = {
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 2, -1 },
    { -1, -1 },
    { 0, 8 },
    { -1, -1 },
    { -1, -1 },
    { 1, -1 },
};

static constinit const struct HashTableValue relativeTimeFormatPrototypeTableValues[3] = {
   { "format"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlRelativeTimeFormatPrototypeFuncFormat, 2 } },
   { "formatToParts"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlRelativeTimeFormatPrototypeFuncFormatToParts, 2 } },
   { "resolvedOptions"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlRelativeTimeFormatPrototypeFuncResolvedOptions, 0 } },
};

static constinit const struct HashTable relativeTimeFormatPrototypeTable =
    { 3, 7, false, nullptr, relativeTimeFormatPrototypeTableValues, relativeTimeFormatPrototypeTableIndex };

} // namespace JSC

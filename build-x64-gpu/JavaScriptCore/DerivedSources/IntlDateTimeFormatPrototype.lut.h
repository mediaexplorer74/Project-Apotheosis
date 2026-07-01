// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/IntlDateTimeFormatPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex dateTimeFormatPrototypeTableIndex[17] = {
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 1, -1 },
    { -1, -1 },
    { 4, -1 },
    { 2, -1 },
    { 0, 16 },
    { -1, -1 },
    { -1, -1 },
    { 3, -1 },
};

static constinit const struct HashTableValue dateTimeFormatPrototypeTableValues[5] = {
   { "format"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::ReadOnly|PropertyAttribute::CustomAccessor), NoIntrinsic, { HashTableValue::GetterSetterType, intlDateTimeFormatPrototypeGetterFormat, 0 } },
   { "formatRange"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlDateTimeFormatPrototypeFuncFormatRange, 2 } },
   { "formatRangeToParts"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlDateTimeFormatPrototypeFuncFormatRangeToParts, 2 } },
   { "formatToParts"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlDateTimeFormatPrototypeFuncFormatToParts, 1 } },
   { "resolvedOptions"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlDateTimeFormatPrototypeFuncResolvedOptions, 0 } },
};

static constinit const struct HashTable dateTimeFormatPrototypeTable =
    { 5, 15, true, nullptr, dateTimeFormatPrototypeTableValues, dateTimeFormatPrototypeTableIndex };

} // namespace JSC

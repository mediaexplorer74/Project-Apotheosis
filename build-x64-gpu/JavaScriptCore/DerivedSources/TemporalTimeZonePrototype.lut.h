// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/TemporalTimeZonePrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex temporalTimeZonePrototypeTableIndex[8] = {
    { 1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 0, -1 },
    { 2, -1 },
    { -1, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue temporalTimeZonePrototypeTableValues[3] = {
   { "toString"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalTimeZonePrototypeFuncToString, 0 } },
   { "toJSON"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalTimeZonePrototypeFuncToJSON, 0 } },
   { "id"_s, static_cast<unsigned>(PropertyAttribute::ReadOnly|PropertyAttribute::DontEnum|PropertyAttribute::CustomAccessor), NoIntrinsic, { HashTableValue::GetterSetterType, temporalTimeZonePrototypeGetterId, 0 } },
};

static constinit const struct HashTable temporalTimeZonePrototypeTable =
    { 3, 7, true, nullptr, temporalTimeZonePrototypeTableValues, temporalTimeZonePrototypeTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/TemporalNow.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex temporalNowTableIndex[4] = {
    { 1, -1 },
    { 0, -1 },
    { -1, -1 },
    { -1, -1 },
};

static constinit const struct HashTableValue temporalNowTableValues[2] = {
   { "instant"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalNowFuncInstant, 0 } },
   { "timeZoneId"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalNowFuncTimeZoneId, 0 } },
};

static constinit const struct HashTable temporalNowTable =
    { 2, 3, false, nullptr, temporalNowTableValues, temporalNowTableIndex };

} // namespace JSC

// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/TemporalCalendarPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex temporalCalendarPrototypeTableIndex[17] = {
    { -1, -1 },
    { 1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 4, -1 },
    { -1, -1 },
    { 3, -1 },
    { 2, -1 },
    { 6, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 5, -1 },
    { 0, 16 },
    { -1, -1 },
    { -1, -1 },
    { 7, -1 },
};

static constinit const struct HashTableValue temporalCalendarPrototypeTableValues[8] = {
   { "dateFromFields"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalCalendarPrototypeFuncDateFromFields, 1 } },
   { "dateAdd"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalCalendarPrototypeFuncDateAdd, 2 } },
   { "dateUntil"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalCalendarPrototypeFuncDateUntil, 2 } },
   { "fields"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalCalendarPrototypeFuncFields, 1 } },
   { "mergeFields"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalCalendarPrototypeFuncMergeFields, 2 } },
   { "toString"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalCalendarPrototypeFuncToString, 0 } },
   { "toJSON"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, temporalCalendarPrototypeFuncToJSON, 0 } },
   { "id"_s, static_cast<unsigned>(PropertyAttribute::ReadOnly|PropertyAttribute::DontEnum|PropertyAttribute::CustomAccessor), NoIntrinsic, { HashTableValue::GetterSetterType, temporalCalendarPrototypeGetterId, 0 } },
};

static constinit const struct HashTable temporalCalendarPrototypeTable =
    { 8, 15, true, nullptr, temporalCalendarPrototypeTableValues, temporalCalendarPrototypeTableIndex };

} // namespace JSC

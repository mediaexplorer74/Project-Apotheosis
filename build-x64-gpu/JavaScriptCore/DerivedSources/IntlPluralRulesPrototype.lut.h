// Automatically generated from C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/runtime/IntlPluralRulesPrototype.cpp using C:/Users/Admin/source/repos/!OpenCode/Apotheosis/WebKit/Source/JavaScriptCore/create_hash_table. DO NOT EDIT!

#include "Lookup.h"

namespace JSC {

static constinit const struct CompactHashIndex pluralRulesPrototypeTableIndex[9] = {
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 0, 8 },
    { 1, -1 },
    { -1, -1 },
    { -1, -1 },
    { -1, -1 },
    { 2, -1 },
};

static constinit const struct HashTableValue pluralRulesPrototypeTableValues[3] = {
   { "select"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlPluralRulesPrototypeFuncSelect, 1 } },
   { "selectRange"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlPluralRulesPrototypeFuncSelectRange, 2 } },
   { "resolvedOptions"_s, static_cast<unsigned>(PropertyAttribute::DontEnum|PropertyAttribute::Function), NoIntrinsic, { HashTableValue::NativeFunctionType, intlPluralRulesPrototypeFuncResolvedOptions, 0 } },
};

static constinit const struct HashTable pluralRulesPrototypeTable =
    { 3, 7, false, nullptr, pluralRulesPrototypeTableValues, pluralRulesPrototypeTableIndex };

} // namespace JSC

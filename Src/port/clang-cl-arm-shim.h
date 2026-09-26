/* clang-cl-arm-shim.h  —  supplies the MSVC-specific intrinsics clang-cl lacks on ARM32.
 * Force-included into every TU via the toolchain's /FI (before the STL headers).
 * Only active for clang-cl + ARM (32-bit); empty for every other compiler/arch.
 *
 * Background: MSVC STL's <bit> (__msvc_bit_utils.hpp) uses _CountLeadingZeros /
 *       _CountLeadingZeros64 (ARM CLZ intrinsics) in its _M_ARM branch, but with no
 *       !__clang__ guard, so clang-cl has no such declaration.
 */
#pragma once
#if defined(__clang__) && defined(_M_ARM)

#ifdef __cplusplus
extern "C" {
#endif

static __forceinline unsigned int _CountLeadingZeros(unsigned long _Val) {
    return _Val ? (unsigned int)__builtin_clz((unsigned int)_Val) : 32u;
}

static __forceinline unsigned int _CountLeadingZeros64(unsigned __int64 _Val) {
    return _Val ? (unsigned int)__builtin_clzll((unsigned long long)_Val) : 64u;
}

#ifdef __cplusplus
}
#endif

#endif /* __clang__ && _M_ARM */

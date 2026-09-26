# overlay triplet: arm-uwp — ARM32 UWP dependencies.
# Uses VS2022 v143 toolset (arm32 cross-compiler from MSVC 14.44).
#
# Use it explicitly -- vcpkg's own community arm-uwp triplet is not enough:
#   vcpkg install <ports>:arm-uwp --overlay-triplets=<repo>\Src\port\vcpkg-triplets
#
# Apotheosis: the load-bearing line here is VCPKG_CMAKE_SYSTEM_VERSION. Without it CMake picks the
# newest installed Windows SDK, 10.0.26100.0, which ships no Lib\...\um\arm at all -- so the ARM
# compiler compiles vcpkg's probe fine and the link then dies with
#   LINK : fatal error LNK1104: cannot open file 'WindowsApp.lib'
# inside vcpkg's own compiler-detection step, before any port is built. 19041 is the last SDK
# carrying ARM32 libraries and is what the whole tree is pinned to.
#
# Two settings were removed on 2026-08-17 because they broke instance matching. Together,
# VCPKG_VISUAL_STUDIO_PATH and VCPKG_PLATFORM_TOOLSET v143 made vcpkg report
#   error: in triplet arm-uwp: Unable to find a valid Visual Studio instance
#          at "C:/Program Files/Microsoft Visual Studio/2022/Community" with toolset version v143
# for the only instance installed, which does hold the v143 14.44 ARM toolset. vcpkg finds it on its
# own; pinning it only narrowed the match until nothing satisfied it. Do not add them back.
#
# One more prerequisite that cannot be expressed here: VS 2022 17.14 installs the ARM32 compiler but
# not vcvarsamd64_arm.bat / vcvarsx86_arm.bat, and vcpkg enumerates target architectures by looking
# for exactly those files -- without them it reports "available toolchain combinations are: x86,
# amd64, x86_arm64, amd64_arm64" and refuses arm before ever reading this triplet. Each wrapper is
# one line (`@call "%~dp0vcvarsall.bat" x64_arm %*`) and vcvarsall still supports the x64_arm target.
set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME WindowsStore)
set(VCPKG_CMAKE_SYSTEM_VERSION 10.0.19041.0)

# Apotheosis: every object built for this triplet carries `/DEFAULTLIB:volatileaccessu.lib` -- verified
# with `dumpbin /directives` on curl's altsvc.c.obj, which lists exactly three directives:
# volatileaccessu.lib, MSVCRTD and OLDNAMES. That library does not exist for ARM32 anywhere:
#   Lib\10.0.26100.0\um\{arm64,x64,x86}\volatileaccessu.lib   exist
#   Lib\10.0.19041.0\...                                      does not ship it at all
# so curl died with "LINK : fatal error LNK1104: cannot open file 'volatileaccessu.lib'". x64-uwp never
# notices because 26100 provides it there.
# The directive comes from neither the command line nor a header: bisecting curl's flags
# (/MDd /RTC1 /GS /EHsc /Z7 /MP /Od, with and without windows.h) never reproduced it, and the string
# does not appear anywhere in either SDK's include tree. Whatever emits it, the library is
# unobtainable for this architecture, so the only options are to exclude it or to fabricate an empty
# stand-in. Excluding it is the honest one, and it is expressed here -- in the repository -- rather
# than as another shim under C:\Program Files.
# If a real symbol from it is ever needed, this turns into a clear LNK2019 naming that symbol, which
# is a better failure than a missing file.
set(VCPKG_LINKER_FLAGS "/NODEFAULTLIB:volatileaccessu.lib")

# Apotheosis: and the root cause behind that exclusion. vcpkg calls vcvarsall with no SDK argument,
# so INCLUDE ends up pointing at the newest SDK (26100) while LIB has to be 19041 -- the only SDK with
# ARM32 libraries. 26100's winnt.h then supplies RtlFillVolatileMemory, an inline that needs
# RtlSetVolatileMemory from a library that does not exist for this architecture:
#   strdup.c.obj : error LNK2019: unresolved external symbol RtlSetVolatileMemory
# The fix is to put 19041's headers ahead of whatever vcvarsall chose, where that function simply does
# not exist, so curl compiles a plain memset. /I is searched before %INCLUDE%, which is why this works
# as plain compiler flags.
#
# Why not VCPKG_CHAINLOAD_TOOLCHAIN_FILE, which looks like the tidier place for this: setting it makes
# vcpkg switch VCPKG_PLATFORM_TOOLSET to "external" and skip setting up the MSVC environment
# altogether, so CMake then fails with "No CMAKE_C_COMPILER could be found". A chainload toolchain has
# to bring its own compiler, PATH, INCLUDE and LIB -- essentially re-implementing
# Src\port\arm32-uwp-env.ps1 in CMake. Not worth it for an include-order fix.
#
# The 8.3 short path is not decoration. vcpkg passes these flags as a single quoted -D argument, so
# spaces inside are fine but an embedded double quote terminates it early: the first attempt used
# /I"...Program Files (x86)..." and arrived at CMake as literally `-DVCPKG_C_FLAGS=/I`, silently doing
# nothing. C:\PROGRA~2\WI3CF2~1 has no spaces, so no quoting is needed at all. Verified reachable:
# winnt.h resolves through it.
set(_apo_sdk_inc "C:/PROGRA~2/WI3CF2~1/10/Include/100190~1.0")
set(VCPKG_C_FLAGS   "/I${_apo_sdk_inc}/ucrt /I${_apo_sdk_inc}/shared /I${_apo_sdk_inc}/um /I${_apo_sdk_inc}/winrt")
set(VCPKG_CXX_FLAGS "${VCPKG_C_FLAGS}")

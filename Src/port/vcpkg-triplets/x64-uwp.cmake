# overlay triplet: x64-uwp — for building x64 UWP dependencies
# Uses desktop VS detection (to bypass VS UWP validation bug in vcpkg-tool)
# but passes UWP compile/link defines explicitly.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Apotheosis: VCPKG_CMAKE_SYSTEM_NAME is set again as of 2026-08-21, and the header comment above
# (kept for the record) is now wrong about why it was omitted.
#
# It was left out to "bypass VS UWP instance validation which fails on VS 17.14". That failure was
# real but its cause was the VCPKG_VISUAL_STUDIO_PATH + VCPKG_PLATFORM_TOOLSET pair removed below,
# not WindowsStore: arm-uwp.cmake sets WindowsStore, does not set those two, and builds fine.
#
# Omitting it is not a harmless simplification, because vcpkg derives VCPKG_TARGET_IS_UWP from it and
# ports branch on that. icu's portfile is the case that exposed it:
#
#   elseif(VCPKG_TARGET_IS_UWP)
#       ... ac_cv_func_tzset=no ac_cv_func__tzset=no
#       -DU_PLATFORM_HAS_WINUWP_API=1
#       PKGDATA_OPTS=--windows-uwp-build
#
# With the flag false none of that applied, ICU compiled as if for desktop Windows, and the build died
# on putil.cpp(635): error C3861: '_tzset': identifier not found -- _tzset being outside the APP
# partition. Passing WINAPI_FAMILY=WINAPI_FAMILY_APP by hand, as this triplet did, tells the compiler
# but not vcpkg, so every port that adapts itself to UWP silently did not.
#
# SDK version is deliberately not pinned here, unlike arm-uwp: the x64 engine line is configured
# against 10.0.26100.0 and CMake's default choice of the newest installed SDK is the right one.
set(VCPKG_CMAKE_SYSTEM_NAME WindowsStore)

# Intentionally do NOT set VCPKG_CMAKE_SYSTEM_NAME=WindowsStore —
# that triggers VS UWP instance validation which fails on VS 17.14.
# Instead, pass UWP flags directly (sufficient for libraries).

# UWP compile flags: WINAPI_FAMILY=APP + suppress WRL default lib
set(VCPKG_CXX_FLAGS "/DWINAPI_FAMILY=WINAPI_FAMILY_APP /D__WRL_NO_DEFAULT_LIB__")
set(VCPKG_C_FLAGS "/DWINAPI_FAMILY=WINAPI_FAMILY_APP /D__WRL_NO_DEFAULT_LIB__")

# Linker: skip standard libs (we don't need WindowsApp.lib for libs)
set(VCPKG_LINKER_FLAGS "")

# Apotheosis: VCPKG_VISUAL_STUDIO_PATH and VCPKG_PLATFORM_TOOLSET v143 used to be set here and were
# removed on 2026-08-21. Together they make vcpkg reject the only installed instance:
#
#   error: in triplet x64-uwp: Unable to find a valid Visual Studio instance
#     at "C:/Program Files/Microsoft Visual Studio/2022/Community" with toolset version v143
#   The following Visual Studio instances were considered:
#     C:\Program Files\Microsoft Visual Studio\2022\Community
#
# -- it considers exactly the instance it was told to use, and still finds no match. The same pair
# was removed from arm-uwp.cmake on 2026-08-17 for the identical error, and that file says in so many
# words not to add them back; this triplet simply never got the same treatment, so `vcpkg install
# icu:x64-uwp` failed before building anything. vcpkg locates the toolchain perfectly well on its own.

# overlay triplet: x64-uwp — for building x64 UWP dependencies
# Uses desktop VS detection (to bypass VS UWP validation bug in vcpkg-tool)
# but passes UWP compile/link defines explicitly.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Intentionally do NOT set VCPKG_CMAKE_SYSTEM_NAME=WindowsStore —
# that triggers VS UWP instance validation which fails on VS 17.14.
# Instead, pass UWP flags directly (sufficient for libraries).

# UWP compile flags: WINAPI_FAMILY=APP + suppress WRL default lib
set(VCPKG_CXX_FLAGS "/DWINAPI_FAMILY=WINAPI_FAMILY_APP /D__WRL_NO_DEFAULT_LIB__")
set(VCPKG_C_FLAGS "/DWINAPI_FAMILY=WINAPI_FAMILY_APP /D__WRL_NO_DEFAULT_LIB__")

# Linker: skip standard libs (we don't need WindowsApp.lib for libs)
set(VCPKG_LINKER_FLAGS "")

set(VCPKG_VISUAL_STUDIO_PATH "C:/Program Files/Microsoft Visual Studio/2022/Community")
set(VCPKG_PLATFORM_TOOLSET v143)

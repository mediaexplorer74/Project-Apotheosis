# overlay triplet: x64-uwp — for building x64 UWP dependencies
# Uses VS2022 v143 toolset (natively available for x64, no vcvars hack needed).
# Usage: vcpkg install <pkg> --triplet x64-uwp --overlay-triplets=path\to\triplets

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME WindowsStore)
set(VCPKG_CMAKE_SYSTEM_VERSION 10.0.19041.0)

set(VCPKG_VISUAL_STUDIO_PATH "C:/Program Files/Microsoft Visual Studio/2022/Community")
set(VCPKG_PLATFORM_TOOLSET v143)

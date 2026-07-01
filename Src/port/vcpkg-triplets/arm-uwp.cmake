# overlay triplet: arm-uwp — ARM32 UWP dependencies.
# Uses VS2022 v143 toolset (arm32 cross-compiler from MSVC 14.44).
# Actual copy at C:\vcpkg-overlay\triplets\ (vcpkg needs ASCII path).
set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME WindowsStore)
set(VCPKG_CMAKE_SYSTEM_VERSION 10.0.19041.0)

set(VCPKG_VISUAL_STUDIO_PATH "C:/Program Files/Microsoft Visual Studio/2022/Community")
set(VCPKG_PLATFORM_TOOLSET v143)

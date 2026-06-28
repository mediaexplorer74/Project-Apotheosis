# Toolchain-x64-UWP-clang.cmake — clang-cl targeting x64 UWP (Windows Store / App Container)
# Based on Toolchain-ARM32-UWP-clang.cmake, adapted for x86_64.
# Compiler: LLVM clang-cl with MSVC v143 headers/libs + Win10 SDK (no ARM32 env needed).
# No clang-cl-arm-shim.h needed — x64 clang-cl has full intrinsic coverage.

set(CMAKE_SYSTEM_NAME       WindowsStore)
set(CMAKE_SYSTEM_VERSION    10.0)
set(CMAKE_SYSTEM_PROCESSOR  AMD64)
set(CMAKE_CROSSCOMPILING    TRUE)

set(_CLANG "C:/Program Files/LLVM/bin/clang-cl.exe")
set(CMAKE_C_COMPILER   "${_CLANG}")
set(CMAKE_CXX_COMPILER "${_CLANG}")

set(CMAKE_C_COMPILER_TARGET   "x86_64-unknown-windows-msvc")
set(CMAKE_CXX_COMPILER_TARGET "x86_64-unknown-windows-msvc")

set(CMAKE_C_COMPILER_WORKS   TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)

# App Container macros (same as ARM)
add_compile_definitions(
    WINAPI_FAMILY=WINAPI_FAMILY_APP
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE UNICODE
    _WIN32_WINNT=0x0A00
    __WRL_NO_DEFAULT_LIB__
    WK_WINUWP=1
)

# UWP umbrella lib only
set(CMAKE_C_STANDARD_LIBRARIES   "WindowsApp.lib" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "WindowsApp.lib" CACHE STRING "" FORCE)

# Compile options (no ARM shim needed on x64)
add_compile_options(
    /bigobj
    $<$<CONFIG:Release>:/O2>
)

# Link options
add_link_options(
    /APPCONTAINER
    /MANIFEST:NO
)

set(CMAKE_VS_WINRT_BY_DEFAULT OFF)

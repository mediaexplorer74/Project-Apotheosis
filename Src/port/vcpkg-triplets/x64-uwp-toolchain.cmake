# Custom UWP toolchain for x64 — sets compiler + UWP flags explicitly.
# This avoids vcpkg's VS instance UWP validation issue by providing
# the exact compiler path and all needed flags directly.

# Explicit compiler paths (from UWP vcvars environment)
set(CMAKE_C_COMPILER "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe" CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe" CACHE FILEPATH "C++ compiler")
set(CMAKE_LINKER "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/link.exe" CACHE FILEPATH "Linker")
set(CMAKE_ASM_COMPILER "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/ml64.exe" CACHE FILEPATH "ASM compiler")
set(CMAKE_MT "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe" CACHE FILEPATH "MT")

# Platform
set(CMAKE_SYSTEM_NAME WindowsStore)
set(CMAKE_SYSTEM_VERSION 10.0.19041.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# UWP compile flags
set(CMAKE_CXX_FLAGS_INIT "/DWIN32 /D_WINDOWS /D_UNICODE /DUNICODE /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D__WRL_NO_DEFAULT_LIB__ /DWIN32_LEAN_AND_MEAN /DNOMINMAX")
set(CMAKE_C_FLAGS_INIT "/DWIN32 /D_WINDOWS /D_UNICODE /DUNICODE /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D__WRL_NO_DEFAULT_LIB__ /DWIN32_LEAN_AND_MEAN /DNOMINMAX")

# CRT: dynamic (/MD) for UWP
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Linker: UWP requires /APPCONTAINER and WindowsApp.lib
set(CMAKE_EXE_LINKER_FLAGS_INIT "/APPCONTAINER /MANIFEST:NO /NXCOMPAT /DYNAMICBASE")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "/APPCONTAINER /MANIFEST:NO /NXCOMPAT /DYNAMICBASE")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "/APPCONTAINER /MANIFEST:NO /NXCOMPAT /DYNAMICBASE")

# Standard libs: WindowsApp.lib instead of kernel32.lib
set(CMAKE_C_STANDARD_LIBRARIES_INIT "WindowsApp.lib")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "WindowsApp.lib")

# Policy: need newer policies for WindowsStore
if(POLICY CMP0056)  # CMP0056: try_compile() source file extension
    cmake_policy(SET CMP0056 NEW)
endif()
if(POLICY CMP0066)  # CMP0066: ROOT variables in find_package
    cmake_policy(SET CMP0066 NEW)
endif()
if(POLICY CMP0067)  # CMP0067: MSVC debug information format
    cmake_policy(SET CMP0067 NEW)
endif()
if(POLICY CMP0137)  # CMP0137: try_compile and source locations
    cmake_policy(SET CMP0137 NEW)
endif()

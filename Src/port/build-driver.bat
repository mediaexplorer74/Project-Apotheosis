@echo off
REM ============================================================================
REM build-driver.bat — Build WebCoreDriver.dll (single file, no CMake/Ninja)
REM
REM Usage:
REM   build-driver.bat arm        (ARM32 for Lumia 950)
REM   build-driver.bat x64        (x64 for PC dev)
REM   build-driver.bat arm clean  (clean build)
REM
REM Requires: LLVM/clang-cl (C:\Program Files\LLVM\bin\)
REM           Pre-built WebKit in build-clang-gpu\ (ARM) or build-x64-gpu\ (x64)
REM ============================================================================
setlocal EnableDelayedExpansion

set PORT=%~dp0
set ROOT=%PORT%..\
set LLVM=C:\Program Files\LLVM\bin

REM --- Parse arguments ---
set ARCH=arm
set CLEAN=0
if "%~1"=="arm" set ARCH=arm
if "%~1"=="x64" set ARCH=x64
if "%~2"=="clean" set CLEAN=1

REM --- Set paths per architecture ---
if "%ARCH%"=="arm" (
    set BUILD=%ROOT%build-clang-gpu
    set TARGET=thumbv7-unknown-windows-msvc
    set MACHINE=ARM
    set DLLNAME=WebCoreDriver-arm.dll
    set LIBNAME=WebCoreDriver-arm.lib
    set OBJDIR=%PORT%obj-arm
    set ANGLEDIR=%ROOT%Src\angle\arm
    set VCPKG=C:\vcpkg\installed\arm-uwp\lib
    set ICU=C:\icu-arm-uwp\lib
    call "%PORT%arm32-uwp-env.ps1" >nul 2>&1
)
if "%ARCH%"=="x64" (
    set BUILD=%ROOT%build-x64-gpu
    set TARGET=x86_64-unknown-windows-msvc
    set MACHINE=X64
    set DLLNAME=WebCoreDriver-x64.dll
    set LIBNAME=WebCoreDriver-x64.lib
    set OBJDIR=%PORT%obj-x64
    set ANGLEDIR=%ROOT%Src\angle\x64
    set VCPKG=C:\vcpkg\installed\x64-uwp\lib
    set ICU=C:\icu-x64-uwp\lib
)

echo ============================================================
echo  Building WebCoreDriver %ARCH% (%DLLNAME%)
echo ============================================================

REM --- Clean ---
if "%CLEAN%"=="1" (
    if exist "%OBJDIR%" rmdir /s /q "%OBJDIR%"
    if exist "%PORT%%DLLNAME%" del "%PORT%%DLLNAME%"
    echo Cleaned build artifacts.
)

if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM --- Source files ---
set SRCS=core browser PortChromeClient LoadingFrameLoaderClient PortPlatformStrategies PortNetworkStorageSession webcore-driver-stubs stubs-crypto stubs-pasteboard stubs-network stubs-ax stubs-other stubs-loader

REM --- Common compiler flags ---
set DEFINES=/DENABLE_JIT=1 /DWK_WINUWP=1 /DBUILDING_WEBKIT=1 /DBUILDING_WITH_CMAKE=1 /DBUILDING_WebCore=1 /DHAVE_CONFIG_H=1 /DUNICODE=1 /D_UNICODE=1 /DWIN32_LEAN_AND_MEAN=1 /DNOMINMAX=1 /DNOCRYPT=1 /D_CRT_NONSTDC_NO_DEPRECATE=1 /D_CRT_SECURE_NO_WARNINGS=1 /D_ENABLE_EXTENDED_ALIGNED_STORAGE=1 /D_HAS_EXCEPTIONS=0 /D_SILENCE_CXX23_DENORM_DEPRECATION_WARNING=1 /D__STDC_CONSTANT_MACROS=1 /D__WRL_NO_DEFAULT_LIB__=1 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000004

set INCLUDES=-I"%BUILD%" -I"%BUILD%\WebCore\PrivateHeaders" -I"%BUILD%\WebCore\DerivedSources" -I"%ROOT%WebKit\Source\WebCore" -I"%ROOT%WebKit\Source\WebCore\Modules\ShapeDetection\Interfaces" -I"%ROOT%WebKit\Source\WebCore\Modules\WebGPU\InternalAPI" -I"%ROOT%WebKit\Source\WebKitLegacy\WebCoreSupport" -I"%ROOT%WebKit\Source\WebKitLegacy" -I"%PORT%" -I"%ROOT%Src\angle\include"

REM Add GPU-specific WebCore include paths (TextureMapper, GraphicsLayer)
set INCLUDES=%INCLUDES% -I"%BUILD%\WebCore\PrivateHeaders\WebCore\platform\graphics\texmap"
set INCLUDES=%INCLUDES% -I"%BUILD%\WebCore\PrivateHeaders\WebCore\platform\graphics\gpu"

set FLAGS=--target=%TARGET% -TP -std:c++23 /nologo /O2 /Zc:__cplusplus /EHs-c- /FIconfig.h

REM ============================================================
REM Step 1: Compile each source file
REM ============================================================
set ERRORS=0
for %%S in (%SRCS%) do (
    if exist "%PORT%%%S.cpp" (
        echo   compile %%S.cpp ...
        "%LLVM%\clang-cl.exe" %FLAGS% %DEFINES% %INCLUDES% /Fo"%OBJDIR%\%%S.obj" -c -- "%PORT%%%S.cpp" > "%PORT%compile-%ARCH%.log" 2>&1
        if errorlevel 1 (
            echo     FAILED - see compile-%ARCH%.log
            set /a ERRORS+=1
        ) else (
            echo     OK
        )
    ) else (
        echo   SKIP %%S.cpp (not found)
    )
)

if %ERRORS% gtr 0 (
    echo.
    echo !! %ERRORS% files failed to compile. See compile-%ARCH%.log
    goto :end
)

REM ============================================================
REM Step 2: Link into DLL
REM ============================================================
echo.
echo   Linking %DLLNAME% ...

set LIBS=WebCore.lib JavaScriptCore.lib PAL.lib WTF.lib JavaScriptCore.lib WTF.lib
set LIBS=%LIBS% libEGL.lib libGLESv2.lib libcurl.lib libssl.lib libcrypto.lib
set LIBS=%LIBS% cairo.lib pixman-1.lib freetype.lib harfbuzz.lib fontconfig.lib libexpat.lib
set LIBS=%LIBS% libxml2.lib sqlite3.lib z.lib bz2.lib brotlidec.lib brotlicommon.lib
set LIBS=%LIBS% jpeg.lib libpng16.lib libwebp.lib libwebpdemux.lib libsharpyuv.lib
set LIBS=%LIBS% icuuc.lib icuin.lib icudt.lib WindowsApp.lib

set OBJLIST=
for %%S in (%SRCS%) do (
    if exist "%OBJDIR%\%%S.obj" set OBJLIST=!OBJLIST! "%OBJDIR%\%%S.obj"
)

set LIBPATHS=/LIBPATH:"%BUILD%\lib" /LIBPATH:"%ANGLEDIR%" /LIBPATH:"%VCPKG%" /LIBPATH:"%ICU%"

"%LLVM%\lld-link.exe" /DLL /MACHINE:%MACHINE% /NOLOGO /OUT:"%PORT%%DLLNAME%" /DEF:"%PORT%WebCoreDriver.exports.def" /APPCONTAINER /MANIFEST:NO /OPT:REF /OPT:NOICF /INCREMENTAL:NO /errorlimit:0 %LIBPATHS% %OBJLIST% %LIBS% > "%PORT%link-%ARCH%.log" 2>&1

if errorlevel 1 (
    echo.
    echo !! LINK FAILED. See link-%ARCH%.log
    findstr /C:"error: undefined symbol" "%PORT%link-%ARCH%.log" >nul 2>&1
    if not errorlevel 1 (
        echo Unresolved symbols:
        findstr /C:"error: undefined symbol" "%PORT%link-%ARCH%.log" | more
    )
    goto :end
)

REM Create import library
"%LLVM%\llvm-lib.exe" /OUT:"%PORT%%LIBNAME%" /DEF:"%PORT%WebCoreDriver.exports.def" >nul 2>&1

echo.
echo ============================================================
echo  SUCCESS: %DLLNAME% built
echo  Exports: 30 functions (via .def)
echo  Import lib: %LIBNAME%
echo ============================================================

:end
endlocal

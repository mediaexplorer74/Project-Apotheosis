@echo off
set App=C:\Users\media\source\repos\Vibe\Apotheosis
set Build=%App%\build-x64-vs
set IcuRoot=C:\icu-x64-uwp
set cmake=C:\Program Files\CMake\bin\cmake.exe

if exist "%Build%" rmdir /s /q "%Build%"
mkdir "%Build%"

"%cmake%" -S "%App%\WebKit" -B "%Build%" ^
  -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 ^
  "-DCMAKE_PREFIX_PATH=%IcuRoot%;C:\vcpkg\installed\x64-uwp" ^
  -DPORT=WinUWP ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DENABLE_STATIC_JSC=ON ^
  -DENABLE_C_LOOP=OFF -DENABLE_JIT=ON -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF ^
  -DENABLE_SAMPLING_PROFILER=OFF ^
  -DAPOTHEOSIS_GPU=ON ^
  -DUSE_CURL=ON ^
  "-DCLANG_BUILTINS_LIBRARY=C:/Program Files/LLVM/lib/clang/22/lib/windows/clang_rt.builtins-x86_64.lib"

echo Exit code: %ERRORLEVEL%
if exist "%Build%\WebKit.slnx" echo SUCCESS: WebKit.slnx created
if exist "%Build%\WebKit.sln" echo SUCCESS: WebKit.sln created
dir "%Build%\*.sln*" 2>nul

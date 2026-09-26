@echo off
set App=C:\Users\media\source\repos\Vibe\Apotheosis
set Build=%App%\build-x64-vs
set IcuRoot=C:\icu-x64-uwp
set cmake=C:\Program Files\CMake\bin\cmake.exe

if exist "%Build%\CMakeCache.txt" del "%Build%\CMakeCache.txt"
if exist "%Build%\CMakeFiles" rmdir /s /q "%Build%\CMakeFiles"

"%cmake%" -S "%App%\WebKit" -B "%Build%" -G "Visual Studio 18 2026" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 "-DCMAKE_PREFIX_PATH=%IcuRoot%;C:\vcpkg\installed\x64-uwp" -DPORT=WinUWP -DCMAKE_BUILD_TYPE=Release -DENABLE_STATIC_JSC=ON -DENABLE_C_LOOP=OFF -DENABLE_JIT=ON -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF -DENABLE_SAMPLING_PROFILER=OFF -DAPOTHEOSIS_GPU=ON

echo Exit code: %ERRORLEVEL%

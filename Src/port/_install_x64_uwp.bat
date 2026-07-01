@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_uwp
if errorlevel 1 exit /b 1
"C:\vcpkg\vcpkg.exe" install pixman expat zlib --triplet x64-uwp --overlay-triplets="C:\Users\Admin\source\repos\!OpenCode\Apotheosis\Src\port\vcpkg-triplets" 2>&1

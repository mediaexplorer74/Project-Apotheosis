@echo off
rem Apotheosis: build the ARM32 **C++ harness** (Harness.vcxproj), not the Apotheosis app package --
rem for that one this is the wrong file, use build-app-arm.bat. Mirror of build-harness-x64.bat, and it
rem exists for the same reason: msbuild needs a full VS environment, and the nested quoting for vcvars
rem does not survive being passed through a POSIX shell.
rem
rem vcvars64 is deliberate, not a mistake. It is here only to put msbuild on PATH; the ARM toolchain
rem itself comes from the project, which pins Platform=ARM, toolset v143 14.44.35207 and SDK
rem 10.0.19041.0 (the only SDK with ARM32 libraries). Sourcing the ARM environment here instead would
rem put Lib\10.0.19041.0\um\arm on the path for every tool msbuild runs, including the host ones.
rem
rem Never pass /t:Rebuild: Clean deletes Generated Files\MainPage.g.hpp and the XAML compiler does not
rem write it back, so the next compile dies with C1083. See CLAUDE.md.
rem
rem The engine DLLs come from build-arm32-gpu\bin via the ARM ItemGroup in Harness.vcxproj, so rebuild
rem the engine first if WTF, JavaScriptCore or WebCore changed:
rem   pwsh -File Src\port\link-driver-gpu-arm32.ps1   (driver)
rem   ninja -C build-arm32-gpu JavaScriptCore WebCore -j1   (engine, from an ARM-primed shell)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0..\.."
msbuild Src\harness\Harness.vcxproj /p:Configuration=Release /p:Platform=ARM /p:AppxPackage=true /v:m /nologo
exit /b %errorlevel%

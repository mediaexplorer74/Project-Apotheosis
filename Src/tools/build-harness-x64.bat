@echo off
rem Apotheosis: build the x64 **C++ harness** (Harness.vcxproj), not the Apotheosis app package -- for
rem that one use build-app-x64.bat. msbuild needs a full VS environment (vcvars64) and the nested
rem quoting for that does not survive being passed through a POSIX shell, so the two steps live in a
rem file instead.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0..\.."
rem MinimalTest is deliberately NOT passed: it swaps MainPage.xaml.cpp for the MainPage.minimal.cpp
rem crash-isolation stub and drops every engine .lib from the link, which builds an appx with no
rem browser in it. It exists to bisect static-init crashes, not to build the product.
msbuild Src\harness\Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true /v:m /nologo
exit /b %errorlevel%

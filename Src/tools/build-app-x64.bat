@echo off
rem Apotheosis: build the C# UWP package (ME.Apotheosis) for the x64 bench.
rem
rem Separate from build-harness-x64.bat, which builds the C++ harness (Harness.vcxproj) -- the two
rem lines are different projects with different toolchains, and a build of one never implied a
rem build of the other. The command line is not obvious: AppxBundle=Always plus SideloadOnly is
rem what emits Apotheosis_<ver>_x64_Debug.appxbundle, which is the artifact
rem install-apotheosis-x64.ps1 installs. A plain build produces .msixupload and no bundle.
rem
rem vcvars is sourced because the .NET Native toolchain (ILC) needs the MSVC toolset on PATH.
rem AppxPackageDir is deliberately NOT passed: "%~dp0..\AppPackages\" ends in a backslash, which
rem escapes the closing quote, so the next switch is eaten as part of the path and msbuild fails
rem with MSB4184 ("Illegal characters in path" naming /v:m /nologo). The default output directory
rem is already the project's AppPackages folder, which is where the installer looks.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0..\.."
msbuild Src\Apotheosis\Apotheosis.csproj /p:Configuration=Debug /p:Platform=x64 /p:AppxBundle=Always /p:AppxBundlePlatforms=x64 /p:UapAppxPackageBuildMode=SideloadOnly /v:m /nologo
exit /b %errorlevel%

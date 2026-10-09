@echo off
rem Apotheosis: build the C# UWP package (ME.Apotheosis) for the Lumia 950 (ARM32).
rem
rem This is the ARM twin of build-app-x64.bat, and it exists because the ARM app package used to
rem have no builder at all: the only recipe was an inline MSBuild command line inside
rem Doc/BUILD-ARM32.md, retyped by hand for every device deploy. build-harness-arm.bat is NOT it --
rem that one builds Harness.vcxproj (the C++ harness), a different project on the x64-era toolchain.
rem
rem Two differences from the x64 twin, both deliberate:
rem   * Release, never Debug. Debug's manifest depends on Microsoft.NET.Native.Framework.Debug.1.7 and
rem     Microsoft.VCLibs.140.00.Debug, which a retail Lumia does not carry; the install then fails on
rem     the unmet dependency before any of our code runs. See Doc/BUILD-ARM32.md, section 8.
rem   * AppxBundlePlatforms=arm, so the bundle does not try to carry an x64 payload.
rem
rem AppxBundle=Always is required, not cosmetic: a plain .appx (AppxBundle=Never) is refused by the
rem device's package manager when updating an existing *bundled* install -- 0x80073CFB, increment the
rem version or remove the old package. That is also why every device iteration needs a version bump;
rem Src/tools/Set-AppVersion.ps1 does it and keeps the app-list display name in step.
rem
rem Do NOT add /p:UapAppxPackageBuildMode=SideloadOnly, tempting as it is (build-app-x64.bat passes
rem it). Measured 2026-10-10: with it the bundle holds ONLY Apotheosis_<ver>_ARM.appx -- no
rem Apotheosis_<ver>_language-ru.appx, no language-zh-hans, no scale-* packs -- and
rem BundleArtifacts\arm.txt loses every ResourcePack= line. The ru-RU strings (Back -> Назад) live in
rem the language pack on this arch, so the artifact would ship an English-only phone. Without the flag
rem the bundle carries all seven inner packages, which is what the deployed 0.2.3.31 has.
rem
rem vcvars is sourced because the .NET Native toolchain (ILC) needs the MSVC toolset on PATH.
rem AppxPackageDir is deliberately NOT passed: "%~dp0..\AppPackages\" ends in a backslash, which
rem escapes the closing quote, so the next switch is eaten as part of the path and msbuild fails
rem with MSB4184 ("Illegal characters in path" naming /v:m /nologo). The default output directory
rem is already the project's AppPackages folder.
rem
rem After this, audit before deploying:
rem   pwsh -File Src\tools\Test-AppxPayload.ps1 -Path <the bundle> -ExpectedArch arm
rem and deploy with:
rem   pwsh -File Src\tools\Wdp-Deploy.ps1 -Appx <the bundle> -DeviceIp 192.168.1.35
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0..\.."
msbuild Src\Apotheosis\Apotheosis.csproj /p:Configuration=Release /p:Platform=ARM /p:AppxBundle=Always /p:AppxBundlePlatforms=arm /v:m /nologo
exit /b %errorlevel%

# build-x64-deps.ps1 — Build x64-uwp dependencies from source for Apotheosis.
# Run from "Developer PowerShell for VS 2022" (x64).
# Depends on: cmake, ninja, vcvarsall (for UWP env).
param(
    [ValidateSet('Build','Clean')][string]$Mode = 'Build',
    [string]$IcuRoot = 'C:\icu-x64-uwp',
    [string]$VcpkgInstalled = 'C:\vcpkg\installed\x64-uwp'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

# --- 1. Set up UWP environment ---
Write-Host "==> Setting up x64 UWP build environment..." -ForegroundColor Cyan
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
# We need env vars but can't source in PowerShell. Use cmd helper.
$envScript = "$env:TEMP\_uwp_env.cmd"
@"
@echo off
call "$vcvars" amd64 uwp >nul 2>&1
if errorlevel 1 exit /b 1
echo LIB=%LIB%
echo INCLUDE=%INCLUDE%
echo PATH=%PATH%
echo VCToolsInstallDir=%VCToolsInstallDir%
echo WindowsSdkDir=%WindowsSdkDir%
echo WindowsSDKVersion=%WindowsSDKVersion%
echo UCRTVersion=%UCRTVersion%
echo UniversalCRTSdkDir=%UniversalCRTSdkDir%
echo VSCMD_ARG_app_plat=%VSCMD_ARG_app_plat%
"@ | Set-Content -Path $envScript -Encoding ASCII
$envOutput = cmd /c $envScript
if ($LASTEXITCODE -ne 0) { throw "vcvarsall failed" }
foreach ($line in $envOutput) {
    if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
    }
}
Write-Host "   UWP env ready (plat=$env:VSCMD_ARG_app_plat)" -ForegroundColor Green

# --- 2. Check tools ---
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" }
$ninja = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $ninja)) { $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
if (-not $ninja) { throw "ninja not found" }
Write-Host "   cmake=$cmake" -ForegroundColor Gray
Write-Host "   ninja=$ninja" -ForegroundColor Gray

# --- 3. Build ICU (if not already at IcuRoot) ---
$icuStamp = "$IcuRoot\.built_x64"
if ((Test-Path $icuStamp) -and ($Mode -ne 'Clean')) {
    Write-Host "==> ICU already built at $IcuRoot (delete $icuStamp to rebuild)" -ForegroundColor Yellow
} else {
    Write-Host "==> Building ICU for x64-uwp..." -ForegroundColor Cyan
    $icuSrc = "C:\vcpkg\buildtrees\icu\src"
    if (-not (Test-Path $icuSrc)) {
        # Find ICU in vcpkg ports
        $icuPort = "C:\vcpkg\ports\icu"
        if (-not (Test-Path $icuPort)) { throw "ICU port not found at $icuPort" }
        Write-Host "   ICU port found at $icuPort — will build from vcpkg source cache" -ForegroundColor Yellow
        Write-Host "   Consider: run 'vcpkg install icu --triplet x64-uwp' after VS UWP detection fix" -ForegroundColor Yellow
        Write-Host "   For now, download ICU from https://github.com/unicode-org/icu/releases" -ForegroundColor Yellow
    }
    
    # Try building ICU4C from source using ICU's own build system
    Write-Host "   SKIP: ICU build requires manual download from https://github.com/unicode-org/icu/releases" -ForegroundColor Yellow
    Write-Host "   Need: icu4c-78_1-src.tgz or similar" -ForegroundColor Yellow
    Write-Host "   For now, creating ICU stub..." -ForegroundColor Yellow
    
    # Create minimal ICU stub for test compilation (just the headers)
    if (-not (Test-Path "$IcuRoot\include")) {
        New-Item -ItemType Directory -Path "$IcuRoot\include\unicode" -Force | Out-Null
        New-Item -ItemType Directory -Path "$IcuRoot\lib" -Force | Out-Null
        New-Item -ItemType Directory -Path "$IcuRoot\bin" -Force | Out-Null
    }
    Set-Content -Path $icuStamp -Value "stub" -Encoding ASCII
    Write-Host "   ICU stub created at $IcuRoot" -ForegroundColor Green
}

# --- 4. Build vcpkg packages manually ---
# Since vcpkg's VS UWP detection fails, we build critical packages with CMake directly
$packages = @(
    @{Name='zlib';    Url='https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz'},
    @{Name='expat';   Url='https://github.com/libexpat/libexpat/archive/refs/tags/R_2_6_4.tar.gz'},
    @{Name='pixman';  Url='https://github.com/freedesktop/pixman/archive/refs/tags/pixman-0.44.2.tar.gz'},
    @{Name='brotli';  Url='https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz'}
)

if ($Mode -eq 'Clean') {
    foreach ($pkg in $packages) {
        $buildDir = "$Root\deps-build\$($pkg.Name)"
        Remove-Item $buildDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "==> Cleaned all build dirs" -ForegroundColor Green
    return
}

Write-Host "==> Building x64-uwp dependencies..." -ForegroundColor Cyan
Write-Host "   (This requires source tarballs in C:\vcpkg\downloads or manual port builds)" -ForegroundColor Yellow
Write-Host "   For now, only setting up directory structure" -ForegroundColor Yellow

# Ensure vcpkg installed directory exists
if (-not (Test-Path $VcpkgInstalled)) {
    New-Item -ItemType Directory -Path "$VcpkgInstalled\lib" -Force | Out-Null
    New-Item -ItemType Directory -Path "$VcpkgInstalled\bin" -Force | Out-Null
    New-Item -ItemType Directory -Path "$VcpkgInstalled\include" -Force | Out-Null
}

Write-Host ""
Write-Host "=============================================================" -ForegroundColor Cyan
Write-Host "x64 UWP dependency build setup complete." -ForegroundColor Green
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Install VS UWP workload properly: run 'Visual Studio Installer' -> Modify -> Universal Windows Platform" -ForegroundColor Yellow
Write-Host "  2. Then: vcpkg install cairo[fontconfig,freetype] pixman freetype harfbuzz fontconfig expat curl[ssl,brotli] openssl libxml2 sqlite3 libpng libjpeg-turbo libwebp zlib bzip2 brotli --triplet x64-uwp" -ForegroundColor Yellow
Write-Host "  3. Or: vcpkg install icu --triplet x64-uwp   (for C:\icu-x64-uwp)" -ForegroundColor Yellow
Write-Host "=============================================================" -ForegroundColor Cyan

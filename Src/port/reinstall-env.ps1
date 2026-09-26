# reinstall-env.ps1 — Apotheosis toolchain bootstrap after a Windows reinstall.
# Restores every build tool that the OS wipe removed. Idempotent (safe to re-run).
# Run from an elevated PowerShell:  pwsh -File Src\port\reinstall-env.ps1
#
# Root cause (documented in AGENTS.md): the 4 GB RAM Surface Pro 5 build machine had
# its OS reinstalled, wiping: git, LLVM/clang-cl, CMake, Ninja, vcpkg, ICU, and the
# vcpkg installed stree (x64-uwp + x64-windows). The repo itself (Src\, WebKit\, and
# the prebuilt build-x64-gpu DLLs) survived because it lives outside the OS partition.
#
# Vendored/prebuilt that we must NOT delete:
#   * Src\angle\x64\            (ANGLE libEGL/libGLESv2)          — survives
#   * build-x64-gpu\libs\*.lib   (cairo-complete, sqlite3)        — survives
#   * build-x64-gpu\bin\*.dll    (WebCore/JavaScriptCore/WTF/PAL) — survives
#   * Src\port\WebCoreDriver-gpu.lib                             — survives
#
# Only the OS-level tools + vcpkg + ICU need rebuilding.

$ErrorActionPreference = 'Stop'

Write-Host "==> Apotheosis environment reinstaller" -ForegroundColor Cyan

# ---- 0. Force RemoteSigned so scripts in the repo can run ----
try {
    Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned -Force -ErrorAction Stop
    Write-Host "[ok] ExecutionPolicy = RemoteSigned" -ForegroundColor Green
} catch {
    Write-Host "[warn] could not set ExecutionPolicy: $($_.Exception.Message)" -ForegroundColor Yellow
}

# ---- toolbox: Install via winget when missing ----
function Install-IfMissing([string]$Id, [string]$Probe, [string]$Name) {
    if (Test-Path $Probe) {
        Write-Host "[skip] $Name already present at $Probe" -ForegroundColor Green
        return
    }
    Write-Host "[..] installing $Name ($Id) ..." -ForegroundColor Cyan
    winget install --id $Id -e --accept-source-agreements --accept-package-agreements --silent
    if ($LASTEXITCODE -ne 0) { throw "winget failed for $Id" }
    Write-Host "[ok] $Name installed" -ForegroundColor Green
}

# ---- 2. Core toolchain ----
Install-IfMissing -Probe 'C:\Program Files\Git\cmd\git.exe'       -Id 'Git.Git'        -Name 'git'
Install-IfMissing -Probe 'C:\Program Files\LLVM\bin\clang-cl.exe' -Id 'LLVM.LLVM'      -Name 'LLVM / clang-cl'
Install-IfMissing -Probe 'C:\Program Files\CMake\bin\cmake.exe'   -Id 'Kitware.CMake'  -Name 'CMake'
Install-IfMissing -Probe "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja*" -Id 'Ninja-build.Ninja' -Name 'Ninja'
Install-IfMissing -Probe 'C:\Program Files\*\memreduct*.exe'      -Id 'Henry++.MemReduct' -Name 'MemReduct'

# ---- 3. Ninja short-links into the paths the build scripts expect ----
# configure-gpu-x64.ps1 hardcodes ninja at C:\ProgramData\chocolatey\bin\ninja.exe
$ninjaWinGet = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja-build.Ninja*\ninja.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($ninjaWinGet) {
    New-Item -ItemType Directory -Force -Path 'C:\ProgramData\chocolatey\bin' | Out-Null
    Copy-Item $ninjaWinGet.FullName 'C:\ProgramData\chocolatey\bin\ninja.exe' -Force
    Copy-Item $ninjaWinGet.FullName 'C:\Program Files\CMake\bin\ninja.exe' -Force
    Write-Host "[ok] ninja copied to chocolatey + CMake bin" -ForegroundColor Green
}

# ---- 4. vcpkg ----
if (-not (Test-Path 'C:\vcpkg\vcpkg.exe')) {
    Write-Host "[..] cloning vcpkg ..." -ForegroundColor Cyan
    if (-not (Test-Path 'C:\vcpkg\.git')) {
        git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
    }
    & 'C:\vcpkg\bootstrap-vcpkg.bat' -disableMetrics
    if (Test-Path 'C:\vcpkg\.git') { git -C C:\vcpkg checkout -f }   # finish a partial clone
    Write-Host "[ok] vcpkg ready" -ForegroundColor Green
} else {
    Write-Host "[skip] vcpkg present" -ForegroundColor Green
}

# Wait for any prior install to finish before starting a new one.
function Assert-VcpkgIdle {
    $p = Get-Process -Name vcpkg -ErrorAction SilentlyContinue
    while ($p) {
        Write-Host "[..] vcpkg busy (PID $($p.Id -join ',')), waiting ..." -ForegroundColor Yellow
        Start-Sleep -Seconds 30
        $p = Get-Process -Name vcpkg -ErrorAction SilentlyContinue
    }
}

# ---- 5. vcpkg x64-uwp dependencies (long-running; rebuild often slow on 4 GB) ----
# NOTE: harfbuzz and fontconfig are NOT installed here — they use meson which cannot
# target UWP via vcpkg (BUILD_FAILED). Build them manually afterward with
# build-harfbuzz-x64.ps1 and build-fontconfig-x64.ps1 (meson cross files).
Assert-VcpkgIdle
if (-not (Test-Path 'C:\vcpkg\installed\x64-uwp\lib\jpeg.lib')) {
    Write-Host "[..] installing vcpkg x64-uwp deps (long, do not interrupt) ..." -ForegroundColor Magenta
    & 'C:\vcpkg\vcpkg.exe' install `
        freetype libxml2 libpng zlib brotli pixman libjpeg-turbo libwebp bzip2 expat openssl curl `
        --triplet x64-uwp --allow-unsupported --disable-metrics
    Write-Host "[ok] vcpkg x64-uwp deps installed" -ForegroundColor Green
} else {
    Write-Host "[skip] vcpkg x64-uwp deps already installed" -ForegroundColor Green
}

# ---- 6. ICU (x64-uwp) ----
# ICU is a vcpkg CMake port and can be installed directly for the x64-uwp triplet.
# Note: harness vcxproj expects unversioned icuuc.lib/icuin.lib/icudt.lib at
# C:\icu-x64-uwp\lib (plus headers + DLLs). vcpkg produces versioned icuuc75.lib etc.
# If vcpkg succeeds, copy the produced .lib under C:\icu-x64-uwp\lib (rename to unversioned)
# and headers to C:\icu-x64-uwp\include unless you update the vcxproj lib directories.
if (-not (Test-Path 'C:\vcpkg\installed\x64-uwp\lib\icuuc.lib')) {
    Write-Host "[..] installing ICU via vcpkg (long, do not interrupt) ..." -ForegroundColor Magenta
    & 'C:\vcpkg\vcpkg.exe' install icu --triplet x64-uwp --allow-unsupported --disable-metrics
    Write-Host "[ok] ICU installed via vcpkg" -ForegroundColor Green
} else {
    Write-Host "[skip] ICU present" -ForegroundColor Green
}

Write-Host "" -ForegroundColor Cyan
Write-Host "==> Environment reinstall complete." -ForegroundColor Green
Write-Host "    Rebuild the engine:  ninja -C build-x64-gpu WebCore -j1" -ForegroundColor Yellow
Write-Host "    Then: pwsh Src\port\link-driver-gpu-arm32.ps1 then msbuild Src\harness\Harness.vcxproj ..." -ForegroundColor Yellow
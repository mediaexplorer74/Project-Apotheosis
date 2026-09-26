# configure-gpu-x64.ps1 — x64 UWP WebCore GPU build configuration (build-x64-gpu).
# Based on configure-gpu-arm32.ps1, adapted for x64-uwp.
# Expectations: vcpkg x64-uwp deps installed, including icu:x64-uwp.
param(
    [ValidateSet('Release','Debug')] [string]$Config = 'Release',
    # Apotheosis 2026-08-21: ICU comes from vcpkg now, not from the hand-built C:\icu-x64-uwp.
    # That tree held 75.1 against ARM's 78.3 and, having no lib\pkgconfig, made it impossible to
    # build harfbuzz with ICU on this line -- see Doc/HARFBUZZ-ICU-DIVERGENCE.md. It is also why this
    # parameter existed at all: with ICU inside the vcpkg prefix there is nothing separate to point
    # at. Kept as a parameter so an experiment can still aim elsewhere, but the default is now the
    # same prefix as every other dependency.
    #
    # Note for whoever reconfigures: the old cache has CMAKE_PREFIX_PATH beginning with
    # C:\icu-x64-uwp, and CMake searches the prefix path in order, so the stale tree wins until the
    # cache is regenerated. Installing the vcpkg package alone changes nothing.
    [string]$IcuRoot = 'C:\vcpkg\installed\x64-uwp'
)
$ErrorActionPreference = 'Continue'
# Apotheosis: a CMake cache configured from an ARM-primed shell would bake ARM include and library
# paths into build-x64-gpu, which is expensive to notice and expensive to undo. See assert-target.ps1.
& "$PSScriptRoot\assert-target.ps1" -Expect x64

$cmake  = "C:\Program Files\CMake\bin\cmake.exe"
$ninja  = "C:\ProgramData\chocolatey\bin\ninja.exe"
$WebKit = Join-Path $env:APOTHEOSIS_ROOT 'WebKit'
$Build  = Join-Path $env:APOTHEOSIS_ROOT 'build-x64-gpu'
$Toolchain = Join-Path $PSScriptRoot 'Toolchain-x64-UWP-clang.cmake'

$env:PKG_CONFIG_PATH = "C:/vcpkg/installed/x64-uwp/lib/pkgconfig"
$env:PATH = "C:\vcpkg\installed\x64-windows\tools\gperf;$env:PATH"

# Find perl (bundled by vcpkg for OpenSSL build)
$perl = (Get-ChildItem "C:\vcpkg\downloads\tools\perl" -Recurse -Filter "perl.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if ($perl) {
    $perlDir = Split-Path -Parent $perl
    $env:PATH = "$perlDir;$env:PATH"
    $perlParam = "-DPERL_EXECUTABLE=$perl"
} else {
    $perlParam = ""
}

Write-Host "==> configure x64 GPU build (build-x64-gpu, $Config, JIT+TextureMapper+ANGLE)" -ForegroundColor Cyan
& $cmake -S $WebKit -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_PREFIX_PATH=$IcuRoot;C:\vcpkg\installed\x64-uwp" `
    "-DPORT=WinUWP" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DENABLE_STATIC_JSC=ON" `
    "-DENABLE_C_LOOP=OFF" `
    "-DENABLE_JIT=ON" `
    "-DENABLE_DFG_JIT=OFF" `
    "-DENABLE_FTL_JIT=OFF" `
    "-DENABLE_SAMPLING_PROFILER=OFF" `
    "-DAPOTHEOSIS_GPU=ON" `
    $perlParam
Write-Host "==> configure 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Red'})

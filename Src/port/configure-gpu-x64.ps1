# configure-gpu-x64.ps1 — x64 UWP WebCore GPU build configuration (build-x64-gpu).
# Based on configure-gpu.ps1, adapted for x64-uwp.
# Expectations: vcpkg x64-uwp deps installed, ICU for x64-uwp available.
param(
    [ValidateSet('Release','Debug')] [string]$Config = 'Release',
    [string]$IcuRoot = 'C:\icu-x64-uwp'
)
$ErrorActionPreference = 'Continue'

$cmake  = "C:\Program Files\CMake\bin\cmake.exe"
$ninja  = "C:\Users\Admin\AppData\Local\Microsoft\WinGet\Links\ninja.exe"
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

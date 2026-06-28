# configure-gpu-x64.ps1 — x64 UWP WebCore GPU build configuration (build-x64-gpu).
# Based on configure-gpu.ps1, adapted for x64-uwp.
# Expectations: vcpkg x64-uwp deps installed, ICU for x64-uwp available.
param(
    [ValidateSet('Release','Debug')] [string]$Config = 'Release',
    [string]$IcuRoot = 'C:\icu-x64-uwp'
)
$ErrorActionPreference = 'Continue'

$Root   = Split-Path -Parent $PSScriptRoot
$cmake  = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$WebKit = Join-Path $Root 'WebKit'
$Build  = Join-Path $Root 'build-x64-gpu'
$Toolchain = Join-Path $PSScriptRoot 'Toolchain-x64-UWP-clang.cmake'

$pkgconfig = (Get-ChildItem "C:\vcpkg\downloads\tools\msys2" -Recurse -Filter "pkg-config.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\x64-uwp\lib\pkgconfig"
$env:PATH = "C:\vcpkg\installed\x64-windows\tools\gperf;$env:PATH"

# Optional: add ARM32 env if x64 path resolving needs it — not needed for x64 native build
Write-Host "==> configure x64 GPU build (build-x64-gpu, $Config, JIT+TextureMapper+ANGLE)" -ForegroundColor Cyan
& $cmake -S $WebKit -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_PREFIX_PATH=$IcuRoot;C:\vcpkg\installed\x64-uwp" `
    "-DPKG_CONFIG_EXECUTABLE=$pkgconfig" `
    "-DPORT=WinUWP" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DENABLE_STATIC_JSC=ON" `
    "-DENABLE_C_LOOP=OFF" `
    "-DENABLE_JIT=ON" `
    "-DENABLE_DFG_JIT=OFF" `
    "-DENABLE_FTL_JIT=OFF" `
    "-DENABLE_SAMPLING_PROFILER=OFF" `
    "-DUSE_SYSTEM_MALLOC=ON" `
    "-DAPOTHEOSIS_GPU=ON"
Write-Host "==> configure 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Red'})

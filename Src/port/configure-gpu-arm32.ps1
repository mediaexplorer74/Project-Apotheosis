# configure-gpu-arm32.ps1 — GPU 路径1 的 WebCore 构建配置(build-arm32-gpu)。
# = configure-jit(保留 JIT:gpu-path1 分支 = JIT + GPU)+ 触发 GPU cmake 分支(-DAPOTHEOSIS_GPU=ON)。
# GPU 分支(OptionsWinUWP.cmake,见 [[gpu-acceleration]] 记忆 B 路配方):
#   USE_TEXTURE_MAPPER/USE_GRAPHICS_LAYER_TEXTURE_MAPPER/USE_ANGLE ON、USE_GRAPHICS_LAYER_WC OFF、
#   COORDINATED_GRAPHICS OFF;ANGLE::GLES/ANGLE::EGL 做成 IMPORTED 指向预编译 NuGet ANGLE($env:APOTHEOSIS_ROOT\angle),
#   跳过 add_subdirectory(ThirdParty/ANGLE)(复用预编译,绕开为 thumbv7 编 in-tree ANGLE)。
# 前置:OptionsWinUWP.cmake 需加 APOTHEOSIS_GPU 分支;Source/CMakeLists.txt 的 ANGLE 子目录 add 需 AND NOT APOTHEOSIS_GPU。
# 真机探针(0.1.5.0)验证预编译 ANGLE 能在 App Container 跑通后再开本构建。
param(
    [ValidateSet('Release','Debug')] [string]$Config = 'Release',
    [string]$IcuRoot = 'C:\vcpkg\installed\arm-uwp'
)
$ErrorActionPreference = 'Continue'
# Apotheosis: assert before the dot-source, not after -- arm32-uwp-env.ps1 stamps the marker itself,
# so a guard placed below would always pass. See assert-target.ps1.
& "$PSScriptRoot\assert-target.ps1" -Expect arm32
. "$PSScriptRoot\arm32-uwp-env.ps1"

$cmake  = "C:\Program Files\CMake\bin\cmake.exe"
$ninja  = "C:\Program Files\CMake\bin\ninja.exe"
$WebKit = Join-Path $env:APOTHEOSIS_ROOT 'WebKit'
$Build  = Join-Path $env:APOTHEOSIS_ROOT 'build-arm32-gpu'
$Toolchain = Join-Path $PSScriptRoot 'Toolchain-ARM32-UWP-clang.cmake'

# Apotheosis: use pkgconf (mingw64 build), NOT the msys2 pkg-config: the msys binary mangles
# Windows paths in its search-path output (backslashes, dropped drive letter), and FindPkgConfig
# parses that output as a CMake string and dies on the "\v" escape.
$pkgconfig = "C:\vcpkg\downloads\tools\msys2\3e71d1f8e22ab23f\mingw64\bin\pkgconf.exe"
# Apotheosis: forward slashes only -- msys2 pkg-config echoes the path back with backslashes and
# FindPkgConfig re-parses it as a CMake string, where "\v" etc. is an invalid character escape.
$env:PKG_CONFIG_PATH = "C:/vcpkg/installed/arm-uwp/lib/pkgconfig"
$env:PATH = "C:\vcpkg\installed\x64-windows\tools\gperf;$env:PATH"

Write-Host "==> configure GPU build (build-arm32-gpu, $Config, JIT+TextureMapper+ANGLE)" -ForegroundColor Cyan
# Apotheosis: bmalloc is ON, like the x64 line. USE_SYSTEM_MALLOC=ON would skip
# add_subdirectory(bmalloc), and the JSC LLInt extractor targets then die on the
# missing bmalloc_CopyHeaders dependency (WebKitMacros.cmake:156).
& $cmake -S $WebKit -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_PREFIX_PATH=$IcuRoot;C:\vcpkg\installed\arm-uwp" `
    "-DPKG_CONFIG_EXECUTABLE=$pkgconfig" `
    "-DPORT=WinUWP" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DENABLE_STATIC_JSC=ON" `
    "-DENABLE_C_LOOP=OFF" `
    "-DENABLE_JIT=ON" `
    "-DENABLE_DFG_JIT=OFF" `
    "-DENABLE_FTL_JIT=OFF" `
    "-DENABLE_SAMPLING_PROFILER=OFF" `
    "-DAPOTHEOSIS_GPU=ON"
Write-Host "==> configure 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Red'})

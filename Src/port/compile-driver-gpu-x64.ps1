# compile-driver-gpu-x64.ps1 — compile a single port .cpp for x64 GPU build
# Expects clang-cl.exe in PATH (x64 Developer PowerShell) and WebKit configured
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build-x64-gpu'

# Compiler
$clang = "C:\Program Files\LLVM\bin\clang-cl.exe"
$target = "--target=x86_64-unknown-windows-msvc"

# Common defines (mirror WebKit CMake WinUWP x64 build)
$defines = @(
    "/nologo /TP /bigobj /O2"
    "-DWK_WINUWP=1 -DWINAPI_FAMILY=WINAPI_FAMILY_APP"
    "-D_HAS_EXCEPTIONS=0 -DNOMINMAX -DNOCRYPT"
    "-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN"
    "-D__WRL_NO_DEFAULT_LIB__"
    "-DBUILDING_WEBKIT=1 -DBUILDING_WINUWP__ -DBUILDING_WITH_CMAKE=1"
    "-DBUILDING_WebCore -DHAVE_CONFIG_H=1"
    "-DSTATICALLY_LINKED_WITH_JavaScriptCore"
    "-DSTATICALLY_LINKED_WITH_PAL"
    "-DSTATICALLY_LINKED_WITH_WTF"
    "-DENABLE_JIT=1 -DENABLE_C_LOOP=0"
)

# Include paths
$includes = @(
    "-I$Root\WebKit\Source"
    "-I$Root\WebKit\Source\WebCore"
    "-I$Root\WebKit\Source\WebCore\Modules"
    "-I$Root\WebKit\Source\WebCore\platform\graphics\texmap"
    "-I$Root\WebKit\Source\WebCore\platform\graphics\texmap\coordinated"
    "-I$Root\WebKit\Source\WebKitLegacy"
    "-I$Root\WebKit\Source\WebKitLegacy\WebCoreSupport"
    "-I$Root\WebKit\Source\WTF"
    "-I$Root\WebKit\Source\JavaScriptCore"
    "-I$Root\WebKit\Source\PAL"
    "-I$BuildDir"
    "-I$BuildDir\WebCore\PrivateHeaders"
    "-I$BuildDir\WebCore\DerivedSources"
    "-I$Root\Src\angle\include"
    "-IC:\vcpkg\installed\x64-uwp\include"
    "-IC:\icu-x64-uwp\include"
)

$flags = $defines + $includes -join ' '
$cmd = "`"$clang`" $target $flags /Fo`"$Obj`" -c -- `"$Src`""

$log = "$(Split-Path -Parent $Obj)\compile-x64-gpu.log"
$bat = "$(Split-Path -Parent $Obj)\_driver_x64_compile.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
cmd /c $bat 1> $log 2>&1
$exit = $LASTEXITCODE
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver-gpu-x64] EXIT=$exit  errors=$err  obj=$Obj  log=$log"
if ($exit -ne 0) { Get-Content $log -Tail 25 }

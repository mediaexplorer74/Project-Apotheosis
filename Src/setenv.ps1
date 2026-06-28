# setenv.ps1 — установить APOTHEOSIS_ROOT и под-переменные для проекта Apotheosis
# Source this script: . .\Src\setenv.ps1
# Все скрипты сборки используют $env:APOTHEOSIS_* вместо E:\Apotheosis\

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir "..")
$env:APOTHEOSIS_ROOT = $Root

# Под-переменные для поддиректорий
$env:APOTHEOSIS_PORT    = Join-Path $Root "Src\port"
$env:APOTHEOSIS_HARNESS = Join-Path $Root "Src\harness"
$env:APOTHEOSIS_TOOLS   = Join-Path $Root "Src\tools"
$env:APOTHEOSIS_ANGLE   = Join-Path $Root "Src\angle"
$env:APOTHEOSIS_CRASH   = Join-Path $Root "crash"

# Платформа по умолчанию (arm / x64)
if (-not $env:APOTHEOSIS_ARCH) { $env:APOTHEOSIS_ARCH = "x64" }

# VS + SDK пути
$env:APOTHEOSIS_VS = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$env:APOTHEOSIS_MSVC = "$env:APOTHEOSIS_VS\VC\Tools\MSVC\14.44.35207"
$env:APOTHEOSIS_SDK = 'C:\Program Files (x86)\Windows Kits\10'
$env:APOTHEOSIS_SDK_VER = '10.0.19041.0'

# Vcpkg + ICU (внешние зависимости)
$env:APOTHEOSIS_VCPKG = 'C:\vcpkg'
$env:APOTHEOSIS_ICU   = 'C:\icu-arm-uwp'

write-host "==> APOTHEOSIS_ROOT  = $Root" -ForegroundColor Cyan
write-host "==> APOTHEOSIS_PORT  = $env:APOTHEOSIS_PORT" -ForegroundColor Cyan
write-host "==> APOTHEOSIS_TOOLS = $env:APOTHEOSIS_TOOLS" -ForegroundColor Cyan
write-host "==> APOTHEOSIS_ARCH  = $env:APOTHEOSIS_ARCH" -ForegroundColor Cyan

# Валидация критических путей
$checks = @(
    "$env:APOTHEOSIS_PORT\WebCoreDriver.cpp",
    "$env:APOTHEOSIS_HARNESS\MainPage.xaml.cpp",
    "$env:APOTHEOSIS_MSVC\bin\Hostx64\x64\cl.exe"
)
foreach ($c in $checks) {
    if (-not (Test-Path $c)) { write-host "WARNING: not found: $c" -ForegroundColor Yellow }
}

param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'
. "$env:APOTHEOSIS_PORT\setenv.ps1" *> $null

$cmd = (Get-Content "$env:APOTHEOSIS_PORT\harness-cmd.bat" -Raw).Trim()
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '-c\s+--\s+\S+UnifiedSource\S+',''
# Replace ARM build dirs with x64
$cmd = $cmd -replace 'build-arm32-webcore','build-x64-gpu'
$cmd = $cmd -replace 'build-arm32-gpu','build-x64-gpu'
# ARM target -> x64
$cmd = $cmd -replace '--target=thumbv7-unknown-windows-msvc','--target=x86_64-unknown-windows-msvc'
# ARM paths -> x64
$cmd = $cmd -replace 'C:\\icu-arm-uwp','C:\icu-x64-uwp'
$cmd = $cmd -replace 'C:\\vcpkg\\installed\\arm-uwp','C:\vcpkg\installed\x64-uwp'
$cmd = $cmd -replace 'C:\\vcpkg\\packages\\[^\\]+\\arm-uwp','C:\vcpkg\installed\x64-uwp'
# Remove ARM-specific flags
$cmd = $cmd -replace '-mthumb',''
$cmd = $cmd -replace '/FI[^ ]*clang-cl-arm-shim\.h',''
$cmd = $cmd -replace '-DARM_THUMB2\b',''
$cmd = $cmd -replace '-D__ARM_[^ ]+',''
# GPU-specific: add ANGLE include
$cmd = "$cmd -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy\WebCoreSupport -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy -I$env:APOTHEOSIS_ANGLE\include /Fo`"$Obj`" -c -- `"$Src`""

$bat = "$env:APOTHEOSIS_PORT\_driver_compile_x64.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
$log = "$env:APOTHEOSIS_PORT\driver-compile-x64.log"
Set-Location "$env:APOTHEOSIS_ROOT\build-x64-gpu"
cmd /c $bat 1> $log 2>&1
$exit = $LASTEXITCODE
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver-x64] EXIT=$exit  errors=$err  obj=$Obj"
if ($exit -ne 0) { Get-Content $log -Tail 10 }

# compile-driver-jit.ps1 — 同 compile-driver.ps1,但把所有头/库路径从 build-clang-webcore 换成
# build-clang-jit(JIT 配置的 PrivateHeaders),保证驱动与 JIT 库 ABI 一致(ENABLE_JIT/ENABLE_C_LOOP
# 不同会改 JSC 结构布局)。用法: pwsh -File port\compile-driver-jit.ps1 <src.cpp> <out.obj>
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'
. "$env:APOTHEOSIS_PORT\arm32-uwp-env.ps1" *> $null

$cmd = (Get-Content "$env:APOTHEOSIS_PORT\harness-cmd.bat" -Raw).Trim()
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '-c\s+--\s+\S+UnifiedSource\S+',''
# ★ 关键:头/库路径换到 JIT 构建目录
$cmd = $cmd -replace 'build-clang-webcore','build-clang-jit'
$cmd = "$cmd -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy\WebCoreSupport -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy /Fo`"$Obj`" -c -- `"$Src`""

$bat = "$env:APOTHEOSIS_PORT\_driver_compile_jit.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
$log = "$env:APOTHEOSIS_PORT\driver-compile-jit.log"
Set-Location "$env:APOTHEOSIS_ROOT\build-clang-jit"
cmd /c $bat 1> $log 2>&1
$exit = $LASTEXITCODE
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver-jit] EXIT=$exit  errors=$err  obj=$Obj  log=$log"
if ($exit -ne 0) { Get-Content $log -Tail 25 }

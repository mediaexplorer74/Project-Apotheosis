# compile-driver-gpu.ps1 — 同 compile-driver-jit.ps1,但头/库路径换成 build-clang-gpu
# (GPU 配置:USE_TEXTURE_MAPPER/USE_ANGLE ON + ENABLE_JIT ON;TextureMapper/GraphicsLayer 头才有)。
# 用法: pwsh -File port\compile-driver-gpu.ps1 <src.cpp> <out.obj>
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'
. "$env:APOTHEOSIS_PORT\arm32-uwp-env.ps1" *> $null

$cmd = (Get-Content "$env:APOTHEOSIS_PORT\harness-cmd.bat" -Raw).Trim()
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '-c\s+--\s+\S+UnifiedSource\S+',''
$cmd = $cmd -replace 'build-clang-webcore','build-clang-gpu'
# M2 GPU 呈现:WebCoreDriver.cpp 要包 texmap(经 platform/graphics 已在 -I 上)+ ANGLE 的 GLES2/EGL 头(angle\include 不在 harness-cmd 的 -I 里,补上)。
$cmd = "$cmd -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy\WebCoreSupport -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy -I$env:APOTHEOSIS_ANGLE\include /Fo`"$Obj`" -c -- `"$Src`""

$bat = "$env:APOTHEOSIS_PORT\_driver_compile_gpu.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
$log = "$env:APOTHEOSIS_PORT\driver-compile-gpu.log"
Set-Location "$env:APOTHEOSIS_ROOT\build-clang-gpu"
cmd /c $bat 1> $log 2>&1
$exit = $LASTEXITCODE
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver-gpu] EXIT=$exit  errors=$err  obj=$Obj  log=$log"
if ($exit -ne 0) { Get-Content $log -Tail 25 }

# compile-driver-gpu-arm32.ps1 — 同 compile-driver-jit-arm32.ps1,但头/库路径换成 build-arm32-gpu
# (GPU 配置:USE_TEXTURE_MAPPER/USE_ANGLE ON + ENABLE_JIT ON;TextureMapper/GraphicsLayer 头才有)。
# 用法: pwsh -File port\compile-driver-gpu-arm32.ps1 <src.cpp> <out.obj>
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'
# Apotheosis: derive the tree from this script's own location when setenv.ps1 was not sourced,
# the same way link-driver-gpu-arm32.ps1 does -- a clean shell must not break the path below.
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
if (-not $env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT = $Root }
if (-not $env:APOTHEOSIS_PORT) { $env:APOTHEOSIS_PORT = Join-Path $Root 'Src\port' }
if (-not $env:APOTHEOSIS_ANGLE) { $env:APOTHEOSIS_ANGLE = Join-Path $Root 'Src\angle' }
# Apotheosis: assert before the dot-source, not after -- arm32-uwp-env.ps1 stamps the marker itself,
# so a guard placed below would always pass. See assert-target.ps1.
& "$PSScriptRoot\assert-target.ps1" -Expect arm32
. "$env:APOTHEOSIS_PORT\arm32-uwp-env.ps1" *> $null

$cmd = (Get-Content "$env:APOTHEOSIS_PORT\harness-cmd.bat" -Raw).Trim()
# Apotheosis: harness-cmd.bat is a stale dump that still carries the original dev box's tree root
# (E:/Apotheosis/...) AND the old layout (port/ instead of Src/port/) -- rewrite both, or the
# -include clang-cl-arm-shim.h fails.
$cmd = $cmd.Replace('E:/Apotheosis/port/', "$env:APOTHEOSIS_ROOT\Src\port\")
$cmd = $cmd.Replace('E:/Apotheosis', $env:APOTHEOSIS_ROOT)
# Apotheosis: the bat was written for cmd.exe, which expands %APOTHEOSIS_ROOT% itself; clang-cl
# invoked directly does not, so expand them here or every -I / the WebCore config.h fail to resolve.
$cmd = [Environment]::ExpandEnvironmentVariables($cmd)
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '-c\s+--\s+\S+UnifiedSource\S+',''
$cmd = $cmd -replace 'build-arm32-webcore','build-arm32-gpu'
# Apotheosis: the bat carries -DBUILDING_WebCore / -DWebCore_EXPORTS from a WebCore-internal
# build, which makes WEBCORE_EXPORT mean dllexport in port code. The port is a *consumer* of
# WebCore.dll -- the x64 script strips these for the same reason (compile-driver-gpu-x64.ps1).
# Without the strip the ARM port emits *direct* data references (HTMLNames::aTag ...) instead
# of __imp_ thunks, and the ARM32 linker cannot redirect those (x64 can), so the harness link
# fails with LNK2019 on every WebCore data symbol the driver touches.
$cmd = $cmd -replace '(^|\s)[-/]DBUILDING_WebCore(?=\s|$)', ' '
$cmd = $cmd -replace '(^|\s)[-/]DWebCore_EXPORTS(?=\s|$)', ' '
# M2 GPU 呈现:WebCoreDriver.cpp 要包 texmap(经 platform/graphics 已在 -I 上)+ ANGLE 的 GLES2/EGL 头(angle\include 不在 harness-cmd 的 -I 里,补上)。
# Apotheosis: the bat predates bmalloc, so its -I list has no bmalloc\Headers; the x64 scripts
# (link-driver-x64.ps1, recompile-stubs-x64.ps1) append it too.
$cmd = "$cmd -I$env:APOTHEOSIS_ROOT\build-arm32-gpu\bmalloc\Headers -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy\WebCoreSupport -I$env:APOTHEOSIS_ROOT\WebKit\Source\WebKitLegacy -I$env:APOTHEOSIS_ANGLE\include /Fo`"$Obj`" -c -- `"$Src`""

# Apotheosis: harness-cmd.bat is ~19 KB of flags, far beyond cmd.exe's 8191-char line limit, so
# `cmd /c $bat` always fails with "The input line is too long." Run clang-cl directly instead --
# PowerShell passes the argv through CreateProcess, whose limit (32767) is ample. Quoted and bare
# tokens are split with a quote-aware regex.
$parts = @()
foreach ($m in [regex]::Matches($cmd, '"([^"]*)"|([^\s]+)')) {
    if ($m.Groups[1].Success) { $parts += $m.Groups[1].Value }
    else { $parts += ($m.Groups[2].Value -replace '"', '') }
}
$log = "$env:APOTHEOSIS_PORT\driver-compile-gpu.log"
Set-Location "$env:APOTHEOSIS_ROOT\build-arm32-gpu"
& $parts[0] $parts[1..($parts.Count - 1)] 1> $log 2>&1
$exit = $LASTEXITCODE
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver-gpu] EXIT=$exit  errors=$err  obj=$Obj  log=$log"
if ($exit -ne 0) { Get-Content $log -Tail 25 }

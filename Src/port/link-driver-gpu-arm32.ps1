# link-driver-gpu-arm32.ps1 — like link-driver-gpu-x64.ps1: compile the 12 port sources against
# build-arm32-gpu (TextureMapper + ANGLE + JIT) and archive them into WebCoreDriver-gpu.lib for
# the ARM harness. The harness links this lib statically and LoadPackagedLibrary's
# WebCore.dll/JavaScriptCore.dll at runtime (MainPage.xaml.cpp) - it never loads a driver DLL.
# Apotheosis (Aug 18, 2026): this script USED to also lld-link a WebCoreDriver-gpu.dll. That DLL
# is dead weight today (nothing references it) AND the DLL link could not resolve the protected
# RenderTheme virtuals (minimumControlSize, adjustButtonStyle, ...) referenced by the
# StubRenderTheme vtable in stubs-other.cpp: lld links all 12 objs directly, while the harness
# archive link only pulls referenced members, so those symbols never surface there. The DLL step
# was removed.
$ErrorActionPreference = 'Stop'
# Derive the tree from this script's own location when the environment has not been set up, the
# same way link-driver-gpu-x64.ps1 does -- so the script works whether or not setenv.ps1 was
# sourced first. $P used to be "$env:APOTHEOSIS_ROOT\port", which stopped existing when the port
# layer moved under Src\; every run then silently skipped all 12 sources ("missing <x>.cpp") and
# archived nothing.
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$P = if ($env:APOTHEOSIS_PORT) { $env:APOTHEOSIS_PORT } else { Join-Path $Root 'Src\port' }
# Apotheosis: stamp the derived values back into the environment -- arm32-uwp-env.ps1 and
# compile-driver-gpu-arm32.ps1 read $env:APOTHEOSIS_* directly, and a clean shell must not break them.
if (-not $env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT = $Root }
if (-not $env:APOTHEOSIS_PORT) { $env:APOTHEOSIS_PORT = $P }
if (-not $env:APOTHEOSIS_ANGLE) { $env:APOTHEOSIS_ANGLE = Join-Path $Root 'Src\angle' }
# Apotheosis: assert before the dot-source, not after -- arm32-uwp-env.ps1 stamps the marker itself,
# so a guard placed below would always pass. See assert-target.ps1.
& "$PSScriptRoot\assert-target.ps1" -Expect arm32
. "$P\arm32-uwp-env.ps1" *> $null

$srcs = @(
  'WebCoreDriver','PortPlatformStrategies','LoadingFrameLoaderClient','PortNetworkStorageSession','PortChromeClient','PortStorage',
  'webcore-driver-stubs','stubs-crypto','stubs-pasteboard','stubs-network','stubs-ax','stubs-other','stubs-loader'
)
foreach ($s in $srcs) {
    if (-not (Test-Path "$P\$s.cpp")) { Write-Host "missing source $s.cpp" -ForegroundColor Yellow; continue }
    & pwsh -NoProfile -File "$P\compile-driver-gpu-arm32.ps1" "$P\$s.cpp" "$P\$s.arm32.obj" | Out-Null
    if (-not (Test-Path "$P\$s.arm32.obj") -or (Get-Item "$P\$s.arm32.obj").LastWriteTime -lt (Get-Item "$P\$s.cpp").LastWriteTime) {
        Write-Host "!! $s.cpp (GPU) failed to compile, see driver-compile-gpu.log" -ForegroundColor Red
        Get-Content "$P\driver-compile-gpu.log" -Tail 25; exit 1
    }
}
$objs = $srcs | ForEach-Object { "$P\$_.arm32.obj" }

# Static archive only -- the harness link resolves WebCore symbols against WebCore.lib (import)
# and pulls archive members on demand. No DLL step (see the header comment).
$libExe = 'C:\Program Files\LLVM\bin\llvm-lib.exe'
# Apotheosis: arch-suffixed output, see the comment in link-driver-gpu-x64.ps1. Both scripts used to
# write WebCoreDriver-gpu.lib, so an ARM relink would quietly break the next x64 build and the other way
# round.
$outLib = "$P\WebCoreDriver-gpu-arm32.lib"
Write-Host "==> creating $(Split-Path -Leaf $outLib) (static, $($objs.Count) objs)" -ForegroundColor Cyan
& $libExe /OUT:$outLib $objs *> "$P\driver-archive.log"
if ($LASTEXITCODE -eq 0) {
    Write-Host "🎉 GPU 驱动库链接成功! $(Split-Path -Leaf $outLib) ($([math]::Round((Get-Item $outLib).Length/1MB,2)) MB)" -ForegroundColor Green
}
else {
    Write-Host "==> archive FAILED (exit $LASTEXITCODE), see driver-archive.log" -ForegroundColor Red
    Get-Content "$P\driver-archive.log" -Tail 15
    exit 1
}
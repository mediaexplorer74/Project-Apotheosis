# link-driver-gpu-x64.ps1 — link WebCoreDriver-x64.dll from port objs + WebKit static libs.
# Based on link-driver-gpu-arm32.ps1, adapted for x64-uwp target.
param([string]$Config = 'Release')
# Apotheosis: lld-link reads %LIB%, so an ARM-primed shell would put Lib\10.0.19041.0\um\arm on the
# search path for an x64 link. Refuse instead. See assert-target.ps1.
& "$PSScriptRoot\assert-target.ps1" -Expect x64

$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$BuildDir = Join-Path $Root 'build-x64-gpu'

$LibDirs = @(
    "$BuildDir\lib"
    # Apotheosis 2026-08-21: ICU moved into the vcpkg prefix, so the separate C:\icu-x64-uwp\lib entry
    # that used to sit here is gone -- icuuc.lib and friends are in the line above now.
    "C:\vcpkg\installed\x64-uwp\lib"
    "$Root\Src\angle\x64"   # Pre-built x64 ANGLE binaries
)

# Port layer sources — compile then link
$PortDir = "$Root\Src\port"
$Srcs = @(
    'WebCoreDriver','PortChromeClient','LoadingFrameLoaderClient',
    'PortPlatformStrategies','PortNetworkStorageSession','PortStorage',
    'webcore-driver-stubs','stubs-crypto','stubs-pasteboard',
    'stubs-network','stubs-ax','stubs-other','stubs-loader'
)
$CompileScript = Join-Path $PSScriptRoot 'compile-driver-gpu-x64.ps1'
foreach ($s in $Srcs) {
    $srcFile = "$PortDir\$s.cpp"
    if (-not (Test-Path $srcFile)) { Write-Host "  SKIP (no source): $s" -ForegroundColor Yellow; continue }
    Write-Host "  compile $s.cpp ..." -NoNewline
    $compileStarted = [DateTime]::UtcNow
    & pwsh -NoProfile -File $CompileScript $srcFile "$PortDir\$s.x64.obj"
    $compileExit = $LASTEXITCODE
    # Apotheosis: fail closed on compilation failure or a stale output, not just missing objects.
    if ($compileExit -ne 0 -or -not (Test-Path "$PortDir\$s.x64.obj") -or
        (Get-Item "$PortDir\$s.x64.obj").LastWriteTimeUtc -lt $compileStarted) {
        Write-Host " FAILED" -ForegroundColor Red
        Get-Content "$PortDir\compile-x64-gpu.log" -Tail 15; exit 1
    }
    Write-Host " OK" -ForegroundColor Green
}
$Objs = $Srcs | ForEach-Object { "$PortDir\$_.x64.obj" }

# WebKit static libs (from build-x64-gpu)
# Note: PAL is OBJECT library (no .lib), WTF objs are in WebCore
$WebKitLibs = @(
    "$BuildDir\lib\WebCore.lib"
    "$BuildDir\lib\JavaScriptCore.lib"
)

$LinkLibs = @(
    "windowsapp.lib"
    "icuuc.lib"
    "icuin.lib"
    "icudt.lib"
    "libEGL.lib"
    "libGLESv2.lib"
    "cairo.lib"
    "pixman-1.lib"
    "freetype.lib"
    "harfbuzz.lib"
    "fontconfig.lib"
    "libexpat.lib"
    "libcurl.lib"
    "libssl.lib"
    "libcrypto.lib"
    "libxml2.lib"
    "sqlite3.lib"
    "libpng16.lib"
    "jpeg.lib"
    "libwebp.lib"
    "z.lib"
    "bz2.lib"
    "brotlicommon.lib"
    "brotlidec.lib"
)

# Create static .lib (not DLL) — harness links this + WebCoreFull.lib
$libExe = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\lib.exe"
# Apotheosis: the output carries the architecture in its name. It used to be WebCoreDriver-gpu.lib for
# both lines, and since link-driver-gpu-arm32.ps1 wrote the same path, whichever ran last silently
# replaced the other. That happened on 2026-08-21: an ARM relink at 11:03 turned the 47 MB x64 archive
# into the 1 MB ARM one, and the next x64 harness build failed with unresolved WebCoreSetCACertBlob,
# WebCoreGetDiag and WebCoreGetLastError -- symbols that were present, in a library for another
# architecture. Object files were already suffixed per arch (.x64.obj / .arm32.obj); the archive was the
# one place the convention was missing.
$outLib = "$PortDir\WebCoreDriver-gpu-x64.lib"

# Build list of .obj files
$objArgs = @()
foreach ($o in $Objs) { if (Test-Path $o) { $objArgs += $o } }

Write-Host "==> creating $(Split-Path -Leaf $outLib) (static)" -ForegroundColor Cyan
& $libExe /NOLOGO /OUT:$outLib $objArgs
if ($LASTEXITCODE -eq 0) {
    Write-Host "==> $(Split-Path -Leaf $outLib) created successfully ($([math]::Round((Get-Item $outLib).Length/1MB,2)) MB)" -ForegroundColor Green
} else {
    Write-Host "==> lib creation FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

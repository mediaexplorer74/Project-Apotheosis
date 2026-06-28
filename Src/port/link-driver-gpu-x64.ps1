# link-driver-gpu-x64.ps1 — link WebCoreDriver-x64.dll from port objs + WebKit static libs.
# Based on link-driver-gpu.ps1, adapted for x64-uwp target.
param([string]$Config = 'Release')

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build-x64-gpu'

$LibDirs = @(
    "$BuildDir\lib"
    "C:\vcpkg\installed\x64-uwp\lib"
    "C:\icu-x64-uwp\lib"
    "$Root\angle\x64"       # Pre-built x64 ANGLE binaries
)

# Port layer objects (parallel to ARM)
$PortDir = "$Root\port"
$Objs = @(
    "$PortDir\WebCoreDriver.gpu.obj"
    "$PortDir\PortChromeClient.obj"
    "$PortDir\LoadingFrameLoaderClient.obj"
    "$PortDir\PortPlatformStrategies.obj"
    "$PortDir\PortNetworkStorageSession.obj"
    # Add new objs as needed
)

# WebKit static libs (from build-x64-gpu)
$WebKitLibs = @(
    "$BuildDir\lib\WebCore.lib"
    "$BuildDir\lib\JavaScriptCore.lib"
    "$BuildDir\lib\PAL.lib"
    "$BuildDir\lib\WTF.lib"
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
    "expat.lib"
    "libcurl.lib"
    "libssl.lib"
    "libcrypto.lib"
    "libxml2.lib"
    "sqlite3.lib"
    "libpng16.lib"
    "libjpeg-turbo.lib"
    "libwebp.lib"
    "zlib.lib"
    "bz2.lib"
    "brotlicommon.lib"
    "brotlidec.lib"
)

$flags = @(
    "/DLL /MACHINE:X64 /NOLOGO"
    "/OUT:$PortDir\WebCoreDriver-x64.dll"
    "/APPCONTAINER"
    "/MANIFEST:NO"
)

foreach ($d in $LibDirs) { $flags += "/LIBPATH:$d" }
foreach ($o in $Objs) { if (Test-Path $o) { $flags += $o } }
foreach ($l in $WebKitLibs) { if (Test-Path $l) { $flags += $l } }
foreach ($l in $LinkLibs) { $flags += $l }

$lld = "lld-link.exe"
Write-Host "==> linking WebCoreDriver-x64.dll" -ForegroundColor Cyan
Write-Host "    flags: $($flags -join ' ')" -ForegroundColor Gray
& $lld $flags
if ($LASTEXITCODE -eq 0) {
    Write-Host "==> WebCoreDriver-x64.dll linked successfully" -ForegroundColor Green
} else {
    Write-Host "==> link FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
}

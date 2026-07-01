# link-driver-gpu-x64.ps1 — link WebCoreDriver-x64.dll from port objs + WebKit static libs.
# Based on link-driver-gpu.ps1, adapted for x64-uwp target.
param([string]$Config = 'Release')

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build-x64-gpu'

$LibDirs = @(
    "$BuildDir\lib"
    "C:\vcpkg\installed\x64-uwp\lib"
    "C:\icu-x64-uwp\lib"
    "$Root\Src\angle\x64"   # Pre-built x64 ANGLE binaries
)

# Port layer sources — compile then link
$PortDir = "$Root\port"
$Srcs = @(
    'WebCoreDriver','PortChromeClient','LoadingFrameLoaderClient',
    'PortPlatformStrategies','PortNetworkStorageSession',
    'webcore-driver-stubs','stubs-crypto','stubs-pasteboard',
    'stubs-network','stubs-ax','stubs-other','stubs-loader'
)
$CompileScript = Join-Path $PSScriptRoot 'compile-driver-gpu-x64.ps1'
foreach ($s in $Srcs) {
    $srcFile = "$PortDir\$s.cpp"
    if (-not (Test-Path $srcFile)) { Write-Host "  SKIP (no source): $s" -ForegroundColor Yellow; continue }
    Write-Host "  compile $s.cpp ..." -NoNewline
    & pwsh -NoProfile -File $CompileScript $srcFile "$PortDir\$s.x64.obj" *> $null
    if (-not (Test-Path "$PortDir\$s.x64.obj")) {
        Write-Host " FAILED" -ForegroundColor Red
        Get-Content "$PortDir\compile-x64-gpu.log" -Tail 15; exit 1
    }
    Write-Host " OK" -ForegroundColor Green
}
$Objs = $Srcs | ForEach-Object { "$PortDir\$_.x64.obj" }

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

$lld = "C:\Program Files\LLVM\bin\lld-link.exe"
Write-Host "==> linking WebCoreDriver-x64.dll" -ForegroundColor Cyan
Write-Host "    flags: $($flags -join ' ')" -ForegroundColor Gray
& $lld $flags
if ($LASTEXITCODE -eq 0) {
    Write-Host "==> WebCoreDriver-x64.dll linked successfully" -ForegroundColor Green
} else {
    Write-Host "==> link FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
}

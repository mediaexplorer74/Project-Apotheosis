# ============================================================================
# x64-uwp-env.ps1  —  manual x64-uwp build env (mirror of arm32-uwp-env.ps1)
# Sets PATH/INCLUDE/LIB for clang-cl and lld-link to target x64 (UWP/AppContainer).
# Used by build-harfbuzz-x64.ps1 / build-fontconfig-x64.ps1 meson cross-builds.
# Usage: . .\port\x64-uwp-env.ps1   # dot-source to inject env into current session
# ============================================================================

$VS  = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$T   = "$VS\VC\Tools\MSVC\14.44.35207"
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
# Apotheosis 2026-09-26: unified with the ARM32 line on Windows SDK 19041.
# 26100 is still installed (VS 2022 needs it) but the x64 build line no longer
# references it. Safe because: all INCLUDE/LIB paths below exist in 19041, the
# 19041 x64 engine already built clean (build-x64-19041-probe\bin\WebCore.dll,
# 44.1 MB, 2026-09-26), and the one historical blocker - 26100's WindowsApp.lib
# exporting symbols 19041's does not, which openssl's cert-store provider needed -
# was already removed on the merits: an AppContainer has no system cert store,
# which is why cacert.pem ships in the package and is injected via
# WebCoreSetCACertBlob. See Src\port\vcpkg-ports\openssl\windows\portfile.cmake:46-56.
$VER = '10.0.19041.0'

# Pin the SDK in the environment so nothing downstream (ninja, rc.exe, mt.exe,
# MSBuild, lld-link) can silently fall back to "newest installed SDK" = 26100.
$env:WindowsTargetPlatformVersion = $VER
$env:WindowsSDKVersion = "$VER\"

# --- completeness self-check ---
$must = @(
    "$T\bin\Hostx64\x64\cl.exe",
    "$T\lib\x64\store",
    "$SDK\Include\$VER\ucrt",
    "$SDK\Lib\$VER\um\x64\WindowsApp.lib",
    "$SDK\bin\$VER\x64\rc.exe"
)
$bad = $must | Where-Object { -not (Test-Path $_) }
if ($bad) { Write-Host "!! missing:" -ForegroundColor Red; $bad | ForEach-Object { Write-Host "   $_" }; throw "env incomplete" }

# --- INCLUDE ---
$env:INCLUDE = @(
    "$T\include"
    "$SDK\Include\$VER\ucrt"
    "$SDK\Include\$VER\shared"
    "$SDK\Include\$VER\um"
    "$SDK\Include\$VER\winrt"
    "$SDK\Include\$VER\cppwinrt"
) -join ';'

# --- LIB (UWP: store lib first) ---
$env:LIB = @(
    "$T\lib\x64\store"
    "$T\lib\x64"
    "$SDK\Lib\$VER\ucrt\x64"
    "$SDK\Lib\$VER\um\x64"
) -join ';'

# --- PATH ---
$front = @(
    "$T\bin\Hostx64\x64"
    "$SDK\bin\$VER\x64"
) -join ';'
$env:PATH = "$front;$env:PATH"

$env:VSCMD_ARG_TGT_ARCH = 'x64'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:MSYS2_ARG_CONV_EXCL = '/nologo;/EP;/TP'

# Apotheosis: stamp the session so an ARM32 step run in this same shell refuses instead of compiling
# thumbv7 objects against x64 headers. See assert-target.ps1 for the whole contract.
$env:APOTHEOSIS_TARGET = 'x64'

Write-Host "==> x64-uwp env injected (cl=$T\bin\Hostx64\x64, SDK=$VER)" -ForegroundColor Green
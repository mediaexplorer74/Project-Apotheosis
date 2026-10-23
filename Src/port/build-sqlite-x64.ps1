# build-sqlite-x64.ps1 - builds the SQLite amalgamation for x64-uwp and installs it
# into C:\vcpkg\installed\x64-uwp (lib + headers). Same recipe as
# build-sqlite-arm32.ps1: plain Win32 compile with SQLITE_OS_WINRT=1 -- no
# WINAPI_FAMILY define, so the 19041 SDK exposes the full API surface; the WinRT
# VFS only calls FromApp/CreateFile2 APIs, which resolve against WindowsApp.lib
# at link time.
# Usage:
#   pwsh -File build-sqlite-x64.ps1 -Amalgamation C:\src\sqlite-amalgamation-3490100
# Download the amalgamation from https://www.sqlite.org/download.html (3.49.1 used here)
# and extract the zip; the directory must contain sqlite3.c / sqlite3.h.
param(
    [string]$Amalgamation = $env:APOTHEOSIS_SQLITE_AMALGAMATION,
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' })
)
$ErrorActionPreference = 'Stop'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$P = if ($env:APOTHEOSIS_PORT) { $env:APOTHEOSIS_PORT } else { Join-Path $Root 'Src\port' }
& "$PSScriptRoot\assert-target.ps1" -Expect x64
. "$P\x64-uwp-env.ps1" *> $null

$amal = $Amalgamation
if (-not $amal) {
    throw "Pass -Amalgamation <dir containing sqlite3.c>, or set APOTHEOSIS_SQLITE_AMALGAMATION. " +
          "Download sqlite-amalgamation-*.zip from https://www.sqlite.org/download.html first."
}
$src = "$amal\sqlite3.c"
if (-not (Test-Path $src)) { throw "sqlite3.c not found in $amal" }
$ver = Select-String -Path "$amal\sqlite3.h" -Pattern 'define SQLITE_VERSION\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value }
Write-Host "==> building sqlite3 amalgamation $ver for x64 UWP (SQLITE_OS_WINRT=1, /MT)" -ForegroundColor Cyan

$obj = "$amal\sqlite3-x64.obj"
$lib = "$amal\sqlite3.lib"
cl /nologo /c /O2 /MT /DSQLITE_OS_WINRT=1 "/Fo$obj" $src
if ($LASTEXITCODE -ne 0) { throw "cl failed" }
lib /NOLOGO /MACHINE:X64 "/OUT:$lib" $obj
if ($LASTEXITCODE -ne 0) { throw "lib failed" }

$dstLib = Join-Path $VcpkgRoot 'installed\x64-uwp\lib\sqlite3.lib'
$dstHdr = Join-Path $VcpkgRoot 'installed\x64-uwp\include'
$buildLib = Join-Path $Root 'build-x64-gpu\libs'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dstLib), $dstHdr, $buildLib | Out-Null
Copy-Item $lib $dstLib -Force
# The x64 WebKit platform file links this copy explicitly; keep both in step.
Copy-Item $lib (Join-Path $buildLib 'sqlite3.lib') -Force
Copy-Item "$amal\sqlite3.h" "$dstHdr\sqlite3.h" -Force
Copy-Item "$amal\sqlite3ext.h" "$dstHdr\sqlite3ext.h" -Force
Write-Host "==> installed sqlite3.lib (vcpkg + build-x64-gpu\libs) and sqlite3.h" -ForegroundColor Green

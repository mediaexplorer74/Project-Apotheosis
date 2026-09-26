# build-sqlite-arm32.ps1 - builds the SQLite amalgamation for arm-uwp and installs it
# into C:\vcpkg\installed\arm-uwp (lib + headers). Follows the x64 line's recipe
# (see Doc/Summary.md "SQLite3 UWP: manually built amalgamation"): plain Win32 compile
# with SQLITE_OS_WINRT=1 -- no WINAPI_FAMILY define, so the 19041 SDK exposes the full
# API surface; the WinRT VFS only calls FromApp/CreateFile2 APIs, which resolve against
# WindowsApp.lib at link time. The vcpkg sqlite3 port does NOT work on arm-uwp: modern
# amalgamations have no SQLITE_OS_WINRT code path and fail to compile under
# WINAPI_FAMILY_APP (undeclared CreateFileA/LoadLibraryA/...).
# Usage: pwsh -File build-sqlite-arm32.ps1 [-Amalgamation <dir-with-sqlite3.c>]
$ErrorActionPreference = 'Stop'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$P = if ($env:APOTHEOSIS_PORT) { $env:APOTHEOSIS_PORT } else { Join-Path $Root 'Src\port' }
& "$PSScriptRoot\assert-target.ps1" -Expect arm32
. "$P\arm32-uwp-env.ps1" *> $null

$amal = $Amalgamation
if (-not $amal) {
    # sqlite-amalgamation-3490100.zip from https://www.sqlite.org/2025/, extracted
    $amal = "C:\Users\media\AppData\Local\Temp\opencode\sqlite-3.49.1\sqlite-amalgamation-3490100"
}
$src = "$amal\sqlite3.c"
if (-not (Test-Path $src)) { throw "sqlite3.c not found in $amal" }
$ver = Select-String -Path "$amal\sqlite3.h" -Pattern 'define SQLITE_VERSION\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value }
Write-Host "==> building sqlite3 amalgamation $ver for ARM32 UWP (SQLITE_OS_WINRT=1, /MT)" -ForegroundColor Cyan

$obj = "$amal\sqlite3-arm32.obj"
$lib = "$amal\sqlite3.lib"
cl /nologo /c /O2 /MT /DSQLITE_OS_WINRT=1 "/Fo$obj" $src
if ($LASTEXITCODE -ne 0) { throw "cl failed" }
lib /NOLOGO /MACHINE:ARM "/OUT:$lib" $obj
if ($LASTEXITCODE -ne 0) { throw "lib failed" }

$dstLib = "C:\vcpkg\installed\arm-uwp\lib\sqlite3.lib"
$dstHdr = "C:\vcpkg\installed\arm-uwp\include"
Copy-Item $lib $dstLib -Force
Copy-Item "$amal\sqlite3.h" "$dstHdr\sqlite3.h" -Force
Copy-Item "$amal\sqlite3ext.h" "$dstHdr\sqlite3ext.h" -Force
Write-Host "==> installed sqlite3.lib + sqlite3.h to C:\vcpkg\installed\arm-uwp" -ForegroundColor Green

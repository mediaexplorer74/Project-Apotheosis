# build-cairo-x64.ps1 - build the static cairo archive the x64 driver links.
#
# WebCore compiles against cairo headers, but the driver links a complete static
# cairo archive. vcpkg's x64-uwp cairo.lib is incomplete for this port (48 objects,
# missing internals), so the original tree carried a hand-built archive. This
# script reproduces it from the official Cairo 1.18.4 source.
#
# Usage:
#   pwsh -File Src\port\build-cairo-x64.ps1
#   pwsh -File Src\port\build-cairo-x64.ps1 -Archive C:\Downloads\cairo-1.18.4.tar.xz
#
# Output:
#   build-x64-gpu\libs\cairo-complete-x64-uwp.lib
#
# Requirements: x64-uwp vcpkg prefix with pixman, LLVM clang-cl, lib.exe.

param(
    [string]$Version = '1.18.4',
    [string]$Archive,
    [string]$Out,
    [string]$Root = $(if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'x64-uwp-env.ps1') | Out-Null
$env:PATH = "$env:ProgramFiles\LLVM\bin;$env:PATH"

$clang = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'
if (-not (Test-Path $clang)) { throw 'clang-cl.exe not found. Install LLVM.' }
$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }
$pixmanInc = Join-Path $vcpkg 'installed\x64-uwp\include\pixman-1'
if (-not (Test-Path (Join-Path $pixmanInc 'pixman.h'))) {
    throw "pixman headers not found: $pixmanInc`nInstall pixman:x64-uwp first."
}

$BuildDir = Join-Path $Root 'build-x64-gpu'
$LibsDir = Join-Path $BuildDir 'libs'
$CairoRoot = Join-Path $LibsDir "cairo-src\cairo-$Version"
$SrcDir = Join-Path $CairoRoot 'src'
$ObjDir = Join-Path $LibsDir 'cairo-obj'
$OutLib = if ($Out) { $Out } else { Join-Path $LibsDir 'cairo-complete-x64-uwp.lib' }
$Rsp = Join-Path $LibsDir 'cairo-objects.rsp'

if ($Clean) {
    Remove-Item $ObjDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $OutLib -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path (Join-Path $SrcDir 'cairo.h'))) {
    Write-Host "==> Cairo $Version source not found; obtaining it" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $CairoRoot) | Out-Null
    if (-not $Archive) {
        $Archive = Join-Path $LibsDir "cairo-src\cairo-$Version.tar.xz"
        if (-not (Test-Path $Archive)) {
            $url = "https://cairographics.org/releases/cairo-$Version.tar.xz"
            Write-Host "    downloading $url"
            Invoke-WebRequest -Uri $url -OutFile $Archive -UseBasicParsing
        }
    }
    if (-not (Test-Path $Archive)) { throw "Cairo archive not found: $Archive" }
    & tar -xf $Archive -C (Split-Path -Parent $CairoRoot)
    if ($LASTEXITCODE -ne 0) { throw "tar failed to extract $Archive" }
}
if (-not (Test-Path (Join-Path $SrcDir 'cairo.h'))) { throw "Cairo source is incomplete: $SrcDir" }

# The generated config.h is small but essential: without it Cairo picks the
# desktop Windows backend and the UWP link fails. It is reproduced verbatim.
$Config = Join-Path $SrcDir 'config.h'
if (-not (Test-Path $Config)) {
    @'
/* config.h for Cairo - x64-uwp (clang-cl UWP) */
#ifndef _WINDOWS_
#ifdef WINAPI_FAMILY
#undef WINAPI_FAMILY
#endif
#define WINAPI_FAMILY 100
#include <windows.h>
#undef WINAPI_FAMILY
#define WINAPI_FAMILY WINAPI_FAMILY_APP
#endif
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_UINT64_T 1
#define DISABLE_SOME_FLOATING_POINT 0
#define HAVE_FCNTL_H 1
#define CAIRO_HAS_WIN32_FONT 1
#define CAIRO_HAS_WIN32_SURFACE 1
#define CAIRO_HAS_PNG_SURFACE 1
'@ | Set-Content -Path $Config -Encoding ascii
}

# The source tree carries a generated cairo-features.h for the image backend.
# The x64 driver also needs the Win32 surface and font code, so turn those on.
$Features = Join-Path $SrcDir 'cairo-features.h'
if (Test-Path $Features) {
    $ft = Get-Content -LiteralPath $Features -Raw
    $ft = $ft -replace '#define CAIRO_HAS_WIN32_FONT 0', '#define CAIRO_HAS_WIN32_FONT 1'
    $ft = $ft -replace '#define CAIRO_HAS_WIN32_SURFACE 0', '#define CAIRO_HAS_WIN32_SURFACE 1'
    Set-Content -LiteralPath $Features -Value $ft -Encoding ascii
}

New-Item -ItemType Directory -Force -Path $ObjDir, (Split-Path -Parent $OutLib) | Out-Null
# Quartz/Xlib/XCB/GL sources are Unix/macOS backends; the PDF/PS/SVG/Script/Tee
# surfaces are disabled in cairo-features.h. cairo-image-mask-compositor.c says
# in its own header that it is not part of any Makefile. The Win32 sources live
# in src\win32, so the source scan must recurse. cairo-ft-stub.c is local
# scaffolding that duplicates the real cairo-ft-font.c symbols.
$exclude = '^cairo-(quartz|xlib|xcb|gl|egl|vg|cocoa|skia|drm|pdf|ps|script|svg|tee|image-mask)-'
$files = @(Get-ChildItem $SrcDir -Recurse -File -Filter '*.c' |
    Where-Object {
        $_.Name -notmatch $exclude -and
        $_.Name -ne 'cairo-ft-stub.c' -and
        $_.FullName -notmatch '\\src\\(quartz|xlib|xcb|gl|egl|vg|cocoa|skia|drm)\\'
    } |
    Sort-Object FullName)
if ($files.Count -lt 100) { throw "Only $($files.Count) Cairo sources found in $SrcDir" }

# Remove objects from a previous source list (for example a backend that is now
# excluded). Keep the rest so a normal rerun is incremental.
$expected = @($files | ForEach-Object { $_.BaseName + '.x64.obj' })
Get-ChildItem $ObjDir -File -Filter '*.obj' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notin $expected } |
    Remove-Item -Force

Write-Host "==> compiling $($files.Count) Cairo sources for x64-uwp" -ForegroundColor Cyan
$flags = @(
    '/nologo', '/c', '/MD', '/O2', '/DHAVE_CONFIG_H', '/DCAIRO_WIN32_STATIC_BUILD=1',
    "-I$SrcDir", "-I$LibsDir", "-I$pixmanInc",
    "-I$vcpkg\installed\x64-uwp\include", "-I$vcpkg\installed\x64-uwp\include\freetype2",
    '-D_WIN32_WINNT=0x0A00', '-DWIN32_LEAN_AND_MEAN',
    '-D_CRT_SECURE_NO_WARNINGS', '-D_USE_MATH_DEFINES', '-D_CRT_NONSTDC_NO_WARNINGS',
    '-Wno-everything'
)
$failed = @()
$n = 0
foreach ($f in $files) {
    $n++
    $obj = Join-Path $ObjDir ($f.BaseName + '.x64.obj')
    Write-Progress -Activity 'Cairo' -Status $f.Name -PercentComplete (($n / $files.Count) * 100)
    & $clang @flags "-Fo$obj" -- $f.FullName 2>$null
    if ($LASTEXITCODE -ne 0) { $failed += $f.Name }
}
Write-Progress -Activity 'Cairo' -Completed

if ($failed.Count) {
    Write-Host "FAILED ($($failed.Count)):" -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    throw 'Cairo compilation failed; no archive was written.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) {
    (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
} else { Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvc) { throw "No MSVC toolset found under $vs\VC\Tools\MSVC" }
$libExe = Join-Path $msvc.FullName 'bin\Hostx64\x64\lib.exe'
if (-not (Test-Path $libExe)) { throw "lib.exe not found: $libExe" }

$objs = @(Get-ChildItem $ObjDir -File -Filter '*.obj' | Sort-Object Name)
$objs | ForEach-Object { '"' + $_.FullName + '"' } | Set-Content -Path $Rsp -Encoding ascii
Write-Host "==> archiving $($objs.Count) objects" -ForegroundColor Cyan
& $libExe /nologo "/OUT:$OutLib" "@$Rsp"
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed with exit code $LASTEXITCODE" }
$size = (Get-Item $OutLib).Length
Write-Host ("==> {0} ({1:N0} bytes, {2:N2} MB)" -f $OutLib, $size, ($size / 1MB)) -ForegroundColor Green


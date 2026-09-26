# build-fontconfig-x64.ps1  —  cross-build fontconfig 2.17.1 for x64-uwp, clang-cl (C11).
# Depends on freetype2.pc + expat.pc + gperf host tool. Feed pkgconfig from vcpkg x64-uwp.
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\x64-uwp-env.ps1" | Out-Null

$py     = "C:\vcpkg\downloads\tools\python\python-3.14.2-x64-1\python.exe"
if (-not (Test-Path $py)) { $py = (Get-Command python).Source }
$meson  = "C:\vcpkg\downloads\tools\meson-1.9.0-633807\meson.py"
$src    = "C:\vcpkg\buildtrees\fontconfig\src\2.17.1-6edd6fe145.clean"
$build  = "$env:APOTHEOSIS_ROOT\deps-build\fontconfig-x64"
$prefix = "C:\vcpkg\installed\x64-uwp"
$cross  = "$PSScriptRoot\fontconfig-cross-clang-x64.txt"

$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\x64-uwp\lib\pkgconfig"
$env:PATH = "C:\Program Files\LLVM\bin;C:\vcpkg\installed\x64-windows\tools\gperf;C:\vcpkg\downloads\tools\ninja-1.13.2-windows;$env:PATH"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

if (-not (Test-Path $src)) { Write-Host "!! src missing: $src" -ForegroundColor Red; exit 1 }

Write-Host "==> meson setup fontconfig (clang-cl, x64-uwp)" -ForegroundColor Cyan
& $py $meson setup $build `
    -Dxml-backend=expat `
    -Dcache-build=disabled -Dtools=disabled -Dtests=disabled `
    -Ddoc=disabled -Ddoc-txt=disabled -Ddoc-man=disabled -Ddoc-pdf=disabled -Ddoc-html=disabled `
    -Dnls=disabled -Diconv=disabled `
    --backend ninja --wrap-mode nodownload -Ddebug=false --libdir lib `
    --native "C:\vcpkg\scripts\buildsystems\meson\none.txt" `
    --cross $cross --prefix $prefix $src
Write-Host "==> meson setup exit $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> meson compile fontconfig" -ForegroundColor Cyan
& $py $meson compile -C $build
Write-Host "==> meson compile exit $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> meson install fontconfig -> $prefix" -ForegroundColor Cyan
& $py $meson install -C $build
Write-Host "==> meson install exit $LASTEXITCODE (fontconfig.lib/dll)" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'}); exit $LASTEXITCODE
# build-harfbuzz-x64.ps1  —  cross-build harfbuzz 14.2.1 for x64-uwp, clang-cl, exceptions off.
# freetype on (hb-ft); icu off (no triplet ICU, use hb-ucd builtin). gdi/directwrite disabled.
# Depends only on freetype.lib + headers already in C:\vcpkg\installed\x64-uwp.
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\x64-uwp-env.ps1" | Out-Null

# Force UTF-8/ASCII subprocess output so meson's archiver detection (lib.exe /?) can
# decode it. Under the GBK/Chinese Windows locale lib.exe /? emitted non-ASCII text that
# Popen_safe_logged failed to decode -> 'NoneType' has no attribute 'upper' (meson detect.py).
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONLEGACYWINDOWSSTDIO = '0'
$env:LC_ALL = 'C.UTF-8'
$env:LANG = 'C.UTF-8'

$py     = "C:\vcpkg\downloads\tools\python\python-3.14.2-x64-1\python.exe"
if (-not (Test-Path $py)) { $py = (Get-Command python).Source }
$meson  = "C:\vcpkg\downloads\tools\meson-1.9.0-633807\meson.py"
$src    = (Get-ChildItem "C:\vcpkg\buildtrees\harfbuzz\src" -Directory | Select-Object -First 1).FullName
$build  = "$env:APOTHEOSIS_ROOT\deps-build\harfbuzz-x64"
$prefix = "C:\vcpkg\installed\x64-uwp"
$cross  = "$PSScriptRoot\harfbuzz-cross-clang-x64.txt"

$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\x64-uwp\lib\pkgconfig"
$env:PATH = "C:\Program Files\LLVM\bin;C:\vcpkg\downloads\tools\ninja-1.13.2-windows;$env:PATH"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "==> meson setup harfbuzz (clang-cl, x64-uwp, no-EH) src=$src" -ForegroundColor Cyan
& $py $meson setup $build `
    -Dfreetype=enabled -Dicu=enabled `
    -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled `
    -Dpng=disabled -Dzlib=disabled `
    -Dtests=disabled -Dutilities=disabled -Dintrospection=disabled -Ddocs=disabled `
    -Dbenchmark=disabled -Dgpu=disabled -Dsubset=disabled `
    --backend ninja --wrap-mode nodownload -Ddebug=false --libdir lib `
    --native "C:\vcpkg\scripts\buildsystems\meson\none.txt" `
    --cross $cross --prefix $prefix $src
Write-Host "==> meson setup exit $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> meson compile harfbuzz" -ForegroundColor Cyan
& $py $meson compile -C $build
Write-Host "==> meson compile exit $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> meson install harfbuzz -> $prefix" -ForegroundColor Cyan
& $py $meson install -C $build
Write-Host "==> meson install exit $LASTEXITCODE (harfbuzz.lib/dll in $prefix)" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'}); exit $LASTEXITCODE
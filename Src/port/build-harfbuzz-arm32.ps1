# build-harfbuzz-arm32.ps1  —  cross-build harfbuzz for arm-uwp,clang-cl,关异常(C++,clang thumbv7 无法 lower cleanupret)。
# freetype 开(hb-ft);icu 开(arm-uwp 有 vcpkg ICU .pc,WebKit 的 LocaleICU/ComplexTextControllerHarfBuzz
# 需要 hb-icu.h 与 harfbuzz-icu.lib)。
# Apotheosis: icu=enabled — WebCore USE(HARFBUZZ) 编译需要 hb-icu.h,否则 LocaleICU.cpp 报 file not found。
# gdi/directwrite/coretext 默认已 disabled(App Container 不要 Win shaper)。
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\arm32-uwp-env.ps1" | Out-Null
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"

$py     = "C:\vcpkg\downloads\tools\python\python-3.14.2-x64-1\python.exe"
if (-not (Test-Path $py)) { $py = (Get-Command python).Source }
$meson  = "C:\vcpkg\downloads\tools\meson-1.9.0-633807\meson.py"
$src    = (Get-ChildItem "C:\vcpkg\buildtrees\harfbuzz\src" -Directory | Select-Object -First 1).FullName
$build  = "$env:APOTHEOSIS_ROOT\deps-build\harfbuzz"
$prefix = "C:\vcpkg\installed\arm-uwp"
$cross  = "$PSScriptRoot\harfbuzz-cross-clang.txt"

$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\arm-uwp\lib\pkgconfig"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "==> meson setup harfbuzz (clang-cl, arm-uwp, no-EH) src=$src" -ForegroundColor Cyan
& $py $meson setup $build `
    -Dfreetype=enabled -Dicu=enabled `
    -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled `
    -Dpng=disabled -Dzlib=disabled `
    -Dtests=disabled -Dutilities=disabled -Dintrospection=disabled -Ddocs=disabled `
    -Dbenchmark=disabled -Dgpu=disabled -Dsubset=disabled `
    --backend ninja --wrap-mode nodownload -Ddebug=false --libdir lib `
    --native "C:\vcpkg\scripts\buildsystems\meson\none.txt" `
    --cross $cross --prefix $prefix $src
Write-Host "==> meson setup 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})
if ($LASTEXITCODE -eq 0) {
    Write-Host "==> meson compile" -ForegroundColor Cyan
    & $py $meson compile -C $build
}
if ($LASTEXITCODE -eq 0) {
    Write-Host "==> meson install" -ForegroundColor Cyan
    & $py $meson install -C $build
}
Write-Host "==> 总退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})

# build-pixman-arm32.ps1  —  cross-build pixman 0.46.4 for arm-uwp, clang-cl (C11), full cycle
# (meson setup + compile + install). Built manually instead of via vcpkg because the stock
# pixman port fetches from gitlab.freedesktop.org, which now answers scripted clients with an
# Anubis bot-check page. Source archive: www.cairographics.org/releases/pixman-0.46.4.tar.gz,
# extracted to C:\vcpkg\buildtrees\pixman\src\pixman-0.46.4. Produces pixman-1-0.dll +
# pixman-1.lib into the arm-uwp prefix (names expected by Harness.vcxproj).
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\arm32-uwp-env.ps1" | Out-Null  # INCLUDE/LIB = 14.44 arm + SDK(clang-cl 找 CRT/SDK 头)
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"  # meson 检测 clang-cl 时要找 lld-link

$py     = "C:\vcpkg\downloads\tools\python\python-3.14.2-x64-1\python.exe"
if (-not (Test-Path $py)) { $py = (Get-Command python).Source }
$meson  = "C:\vcpkg\downloads\tools\meson-1.9.0-633807\meson.py"
$src    = "C:\vcpkg\buildtrees\pixman\src\pixman-0.46.4"
$build  = "$env:APOTHEOSIS_ROOT\deps-build\pixman"
$prefix = "C:\vcpkg\installed\arm-uwp"
$cross  = "$PSScriptRoot\cairo-cross-clang.txt"   # 通用 clang-cl arm-uwp 交叉文件

$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\arm-uwp\lib\pkgconfig"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "==> meson setup pixman (clang-cl, arm-uwp)" -ForegroundColor Cyan
& $py $meson setup $build `
    -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled `
    --backend ninja --wrap-mode nodownload -Ddebug=false --libdir lib `
    --native "C:\vcpkg\scripts\buildsystems\meson\none.txt" `
    --cross $cross --prefix $prefix $src
if ($LASTEXITCODE -eq 0) {
    Write-Host "==> meson compile" -ForegroundColor Cyan
    & $py $meson compile -C $build
}
if ($LASTEXITCODE -eq 0) {
    Write-Host "==> meson install" -ForegroundColor Cyan
    & $py $meson install -C $build
}
Write-Host "==> pixman 总退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})

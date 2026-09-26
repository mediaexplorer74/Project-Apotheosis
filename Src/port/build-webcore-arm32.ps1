# 全量重编 WebCore,keep-going 收集所有残留。先删 WebCore PCH 防 mtime 陈旧。
. "$env:APOTHEOSIS_PORT\arm32-uwp-env.ps1" *> $null
$log = "$env:APOTHEOSIS_ROOT\webcore-pch4.log"
$ninja = (Get-Command ninja).Source
# 删除 WebCore 陈旧 PCH —— 强制本次运行内重建,与 TU 同批 mtime 自洽
Remove-Item "$env:APOTHEOSIS_ROOT\build-clang-webcore\Source\WebCore\CMakeFiles\WebCore.dir\cmake_pch.cxx.pch","$env:APOTHEOSIS_ROOT\build-clang-webcore\Source\WebCore\CMakeFiles\WebCore.dir\cmake_pch.cxx.obj" -Force -ErrorAction SilentlyContinue
& $ninja -k 0 -C $env:APOTHEOSIS_ROOT\build-clang-webcore WebCore 2>&1 | Tee-Object $log
Write-Host "=== ninja 退出码: $LASTEXITCODE ==="

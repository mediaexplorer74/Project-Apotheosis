# 精简增量构建:不删 PCH(本轮未改 PCH 头),只重生成被删的 binding + 重编受影响 TU。
. "$env:APOTHEOSIS_PORT\arm32-uwp-env.ps1" *> $null
$log = "$env:APOTHEOSIS_ROOT\webcore-pch7.log"
$ninja = (Get-Command ninja).Source
& $ninja -k 0 -C $env:APOTHEOSIS_ROOT\build-clang-webcore WebCore 2>&1 | Tee-Object $log
Write-Host "=== ninja 退出码: $LASTEXITCODE ==="

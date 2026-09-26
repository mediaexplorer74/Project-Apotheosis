# link-driver-arm32.ps1 — 编全部 port 驱动/桥/stub .cpp,链成测试 DLL,暴露 LNK2005/undefined。
# Phase 1b:加入 PortPlatformStrategies + LoadingFrameLoaderClient + curl/openssl 库。
$ErrorActionPreference = 'Stop'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$P = if ($env:APOTHEOSIS_PORT) { $env:APOTHEOSIS_PORT } else { Join-Path $Root 'Src\port' }
. "$P\arm32-uwp-env.ps1" *> $null

# --- 1. 编全部 port .cpp(串行,compile-driver 用固定临时文件)---
$srcs = @(
  'WebCoreDriver','PortPlatformStrategies','LoadingFrameLoaderClient','PortStorage',
  'webcore-driver-stubs','stubs-crypto','stubs-pasteboard','stubs-network','stubs-ax','stubs-other','stubs-loader'
)
foreach ($s in $srcs) {
    if (-not (Test-Path "$P\$s.cpp")) { Write-Host "缺源 $s.cpp" -ForegroundColor Yellow; continue }
    & pwsh -NoProfile -File "$P\compile-driver-arm32.ps1" "$P\$s.cpp" "$P\$s.arm32-soft.obj" | Out-Null
    if (-not (Test-Path "$P\$s.arm32-soft.obj") -or (Get-Item "$P\$s.arm32-soft.obj").LastWriteTime -lt (Get-Item "$P\$s.cpp").LastWriteTime) {
        Write-Host "!! $s.cpp 编译失败,看 driver-compile.log" -ForegroundColor Red
        Get-Content "$P\driver-compile.log" -Tail 20; exit 1
    }
}
$objs = $srcs | ForEach-Object { "$P\$_.arm32-soft.obj" }

# --- 2. 链接 DLL ---
$lld = 'C:\Program Files\LLVM\bin\lld-link.exe'
$libs = @(
  'WebCore.lib','JavaScriptCore.lib','PAL.lib','WTF.lib','JavaScriptCore.lib','WTF.lib'  # 循环兜底
  'libcurl.lib','libssl.lib','libcrypto.lib'                                              # 网络栈(TLS 1.3)
  'cairo.lib','pixman-1.lib','freetype.lib','fontconfig.lib','libexpat.lib','harfbuzz.lib'
  'jpeg.lib','libpng16.lib','libwebp.lib','libwebpdemux.lib','libsharpyuv.lib'
  'libxml2.lib','sqlite3.lib','z.lib','bz2.lib','brotlidec.lib','brotlicommon.lib'
  'icuuc.lib','icuin.lib','icudt.lib','WindowsApp.lib'
)
$log = "$P\link-driver.log"
& $lld /DLL /MACHINE:ARM /OUT:"$P\WebCoreDriver.dll" `
    $objs `
    /LIBPATH:"$env:APOTHEOSIS_ROOT\build-arm32-webcore\lib" `
    /LIBPATH:"C:\vcpkg\installed\arm-uwp\lib" `
    /LIBPATH:"C:\icu-arm-uwp\lib" `
    $libs `
    /INCLUDE:WebCoreRenderHtml /EXPORT:WebCoreRenderHtml /EXPORT:WebCoreLoadUrl `
    /OPT:REF /OPT:NOICF /INCREMENTAL:NO /errorlimit:0 `
    *> $log
$code = $LASTEXITCODE

$unres = @(Select-String -Path $log -Pattern 'error: undefined symbol:')
$dup   = @(Select-String -Path $log -Pattern 'duplicate symbol|already defined')
Write-Host "[link-driver] EXIT=$code  undefined=$($unres.Count)  duplicate=$($dup.Count)  log=$log"
if ($code -eq 0) {
  Write-Host "🎉 链接成功! WebCoreDriver.dll(含网络栈)产出" -ForegroundColor Green
  # 链接通过 → 归档 WebCoreDriver.lib(含 PortPlatformStrategies/LoadingFrameLoaderClient/trimmed stubs),供 appx 链接。
  & 'C:\Program Files\LLVM\bin\llvm-lib.exe' /OUT:"$P\WebCoreDriver.lib" $objs | Out-Null
  Write-Host "  WebCoreDriver.lib 重新归档($($objs.Count) obj)"
}
else {
  if ($dup.Count) { Write-Host "--- 重复符号(LNK2005,需删 stub)---"; $dup | ForEach-Object { ($_.Line -replace '.*(duplicate symbol|already defined):? ?','').Trim() } | Sort-Object -Unique | Select-Object -First 30 }
  if ($unres.Count) { Write-Host "--- 未定义符号 ---"; $unres | ForEach-Object { ($_.Line -replace '.*error: undefined symbol: ','').Trim() } | Sort-Object -Unique | Select-Object -First 40 }
}

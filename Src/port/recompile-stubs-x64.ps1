. "$env:APOTHEOSIS_ROOT\Src\setenv.ps1" *>$null
$P = $env:APOTHEOSIS_PORT
$R = $env:APOTHEOSIS_ROOT
$cmd = (Get-Content "$P\harness-cmd.bat" -Raw).Trim()
$cmd = $cmd -replace '^\S+\s+',''
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '\s*-c\s+--\s+\S+',''
$cmd = $cmd -replace '%APOTHEOSIS_ROOT%',$R
$cmd = $cmd -replace '--target=thumbv7-unknown-windows-msvc','--target=x86_64-unknown-windows-msvc'
$cmd = $cmd -replace 'build-arm32-webcore','build-x64-gpu'
$cmd = $cmd -replace 'build-arm32-gpu','build-x64-gpu'
$cmd = $cmd -replace 'C:\\icu-arm-uwp','C:\icu-x64-uwp'
$cmd = $cmd -replace 'C:\\vcpkg\\installed\\arm-uwp','C:\vcpkg\installed\x64-uwp'
$cmd = $cmd -replace '-mthumb',''
$cmd = $cmd -replace '/FI\S*clang-cl-arm-shim\.h',''
$cmd = $cmd -replace '\s-DARM_THUMB2\b',''
$cmd = $cmd -replace '-D_HAS_EXCEPTIONS=0',''
$cmd = "$cmd /EHsc"
$cmd = "$cmd -DCAIRO_WIN32_STATIC_BUILD=1 -I$R\WebKit\Source\WebKitLegacy\WebCoreSupport -I$R\WebKit\Source\WebKitLegacy -I$env:APOTHEOSIS_ANGLE\include -I$R\build-x64-gpu\bmalloc\Headers"
$cmd = ($cmd -replace '\s+',' ').Trim()

$srcFile = "$P\webcore-internal-stubs.cpp"
$objFile = "$P\webcore-internal-stubs.x64.obj"
$rsp = @("/Fo$objFile") + ($cmd -split '\s+') + @("-c", "--", $srcFile)
$rspContent = $rsp -join "`n"
$rspFile = "$P\_webcore-internal-stubs.rsp"
Set-Content -Path $rspFile -Value $rspContent -Encoding ASCII
Write-Output "Compiling webcore-internal-stubs.cpp..."
$result = & "C:\Program Files\LLVM\bin\clang-cl.exe" "@$rspFile" 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    Write-Output "OK - recompiled successfully"
    $objs = @("$P\WebCoreDriver.x64.obj","$P\PortPlatformStrategies.x64.obj","$P\LoadingFrameLoaderClient.x64.obj","$P\PortNetworkStorageSession.x64.obj","$P\PortChromeClient.x64.obj","$P\chromeclient-stubs.x64.obj","$P\webcore-internal-stubs.x64.obj","$P\webcore-driver-stubs.x64.obj","$P\stubs-crypto.x64.obj","$P\stubs-pasteboard.x64.obj","$P\stubs-network.x64.obj","$P\stubs-ax.x64.obj","$P\stubs-other.x64.obj","$P\stubs-loader.x64.obj") | Where-Object { Test-Path $_ }
    $libResult = & 'C:\Program Files\LLVM\bin\llvm-lib.exe' /OUT:"$P\WebCoreDriver-gpu.lib" $objs 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
        Write-Output "WebCoreDriver-gpu.lib created"
    } else {
        Write-Output "LIB creation FAILED: $libResult"
    }
} else {
    Write-Output "FAILED:"
    $result -split "`n" | Where-Object { $_ -match 'error:|fatal:' } | Select-Object -First 5 | ForEach-Object { Write-Output "  $_" }
}

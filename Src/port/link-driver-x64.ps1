param([switch]$CompileOnly)
$ErrorActionPreference = 'Continue'
. "$env:APOTHEOSIS_ROOT\Src\setenv.ps1" *> $null

$P = "$env:APOTHEOSIS_PORT"
$R = $env:APOTHEOSIS_ROOT

# Read base command from harness-cmd.bat and adapt for x64
$cmd = (Get-Content "$P\harness-cmd.bat" -Raw).Trim()
# Strip executable path
$cmd = $cmd -replace '^\S+\s+',''
# Strip per-file flags
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '\s*-c\s+--\s+\S+',''
# Expand %APOTHEOSIS_ROOT% (batch env var → literal path)
$cmd = $cmd -replace '%APOTHEOSIS_ROOT%',$R
# Adapt ARM→x64
$cmd = $cmd -replace '--target=thumbv7-unknown-windows-msvc','--target=x86_64-unknown-windows-msvc'
$cmd = $cmd -replace 'build-arm32-webcore','build-x64-gpu'
$cmd = $cmd -replace 'build-arm32-gpu','build-x64-gpu'
$cmd = $cmd -replace 'C:\\icu-arm-uwp','C:\icu-x64-uwp'
$cmd = $cmd -replace 'C:\\vcpkg\\installed\\arm-uwp','C:\vcpkg\installed\x64-uwp'
$cmd = $cmd -replace '-mthumb',''
$cmd = $cmd -replace '/FI\S*clang-cl-arm-shim\.h',''
$cmd = $cmd -replace '\s-DARM_THUMB2\b',''
# Strip _HAS_EXCEPTIONS=0 — ARM clang workaround; x64 clang-cl handles exceptions fine
$cmd = $cmd -replace '-D_HAS_EXCEPTIONS=0',''
$cmd = "$cmd /EHsc"
# Add GPU-specific includes & vcpkg include (original misses this path)
# Add CAIRO_WIN32_STATIC_BUILD for static Cairo linking (avoids __imp_cairo_* dllimport references)
$cmd = "$cmd -DCAIRO_WIN32_STATIC_BUILD=1 -I$R\WebKit\Source\WebKitLegacy\WebCoreSupport -I$R\WebKit\Source\WebKitLegacy -I$env:APOTHEOSIS_ANGLE\include -I$R\build-x64-gpu\bmalloc\Headers"
$cmd = $cmd -replace '\s+',' '

# Clean up: remove trailing whitespace
$cmd = $cmd.Trim()

$srcs = @(
    'WebCoreDriver','PortPlatformStrategies','LoadingFrameLoaderClient',
    'PortNetworkStorageSession','PortChromeClient','PortStorage',
    'chromeclient-stubs','webcore-internal-stubs',
    'webcore-driver-stubs','stubs-crypto','stubs-pasteboard',
    'stubs-network','stubs-ax','stubs-other','stubs-loader'
)

Write-Host "=== Compiling port layer for x64 ===" -ForegroundColor Cyan
$ok = 0; $fail = 0
foreach ($s in $srcs) {
    $srcFile = "$P\$s.cpp"
    if (-not (Test-Path $srcFile)) { Write-Host "  SKIP: $s.cpp"; continue }
    $objFile = "$P\$s.x64.obj"

    # Write response file: one argument per line
    $rsp = @("/Fo$objFile") + ($cmd -split '\s+') + @("-c", "--", $srcFile)
    $rspContent = $rsp -join "`n"
    $rspFile = "$P\_$s.rsp"
    Set-Content -Path $rspFile -Value $rspContent -Encoding ASCII

    Write-Host "  $s..." -NoNewline
    $result = & "C:\Program Files\LLVM\bin\clang-cl.exe" "@$rspFile" 2>&1 | Out-String
    $exit = $LASTEXITCODE
    if ($exit -eq 0 -and (Test-Path $objFile)) {
        Write-Host " OK" -ForegroundColor Green
        $ok++
    } else {
        Write-Host " FAIL (exit=$exit)" -ForegroundColor Red
        # Show first error/warning lines
        $result -split "`n" | Where-Object { $_ -match 'error:|warning:|fatal:' } | Select-Object -First 3 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkRed }
        $fail++
    }
}

# Build the static lib
$objs = $srcs | ForEach-Object { "$P\$_.x64.obj" } | Where-Object { Test-Path $_ }
if ($objs.Count -gt 0) {
    Write-Host "=== Creating WebCoreDriver-gpu.lib ($ok ok, $fail failed) ===" -ForegroundColor Cyan
    $libResult = & 'C:\Program Files\LLVM\bin\llvm-lib.exe' /OUT:"$P\WebCoreDriver-gpu.lib" $objs 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
        Write-Host "=== WebCoreDriver-gpu.lib created ===" -ForegroundColor Green
    } else {
        Write-Host "=== LIB creation FAILED ===" -ForegroundColor Red
    }
} else {
    Write-Host "=== No objects to link ===" -ForegroundColor Yellow
}

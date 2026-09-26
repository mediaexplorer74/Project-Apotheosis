# bulk-compile-x64.ps1 — compile all port sources for x64
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$PortDir = if ($env:APOTHEOSIS_PORT) { $env:APOTHEOSIS_PORT } else { Split-Path $PSScriptRoot }
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent $PSScriptRoot }

$sources = @(
    "WebCoreDriver.cpp"
    "PortChromeClient.cpp"
    "LoadingFrameLoaderClient.cpp"
    "PortPlatformStrategies.cpp"
    "PortNetworkStorageSession.cpp"
    "PortStorage.cpp"
    "webcore-driver-stubs.cpp"
    "stubs-crypto.cpp"
    "stubs-pasteboard.cpp"
    "stubs-network.cpp"
    "stubs-ax.cpp"
    "stubs-loader.cpp"
)

if ($Force -or -not (Test-Path "$PortDir\stubs-other.x64.obj")) {
    Write-Host "[bulk] stubs-other.cpp (first, to prime PCH)"
    & powershell -File "$PortDir\compile-driver-gpu-x64.ps1" "$PortDir\stubs-other.cpp" "$PortDir\stubs-other.x64.obj"
}

foreach ($s in $sources) {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($s)
    $obj = "$base.x64.obj"
    $objPath = "$PortDir\$obj"
    if (-not $Force -and (Test-Path $objPath)) {
        Write-Host "[bulk] SKIP $s → $obj exists"
        continue
    }
    Write-Host "[bulk] === COMPILING $s → $obj ==="
    $start = Get-Date
    & powershell -File "$PortDir\compile-driver-gpu-x64.ps1" "$PortDir\$s" "$PortDir\$obj"
    $elapsed = (Get-Date) - $start
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[bulk] OK $s ($($elapsed.TotalSeconds.ToString('F1'))s)" -ForegroundColor Green
    } else {
        Write-Host "[bulk] FAIL $s exit=$LASTEXITCODE" -ForegroundColor Red
        throw "$s compilation failed"
    }
}
Write-Host "[bulk] ALL DONE" -ForegroundColor Green

# ============================================================================
# fetch-webkit-source.ps1 — Sparse checkout of WebKit source (space-efficient)
#
# Downloads ONLY the source directories needed to compile WebCoreDriver.dll.
# Skips: tests, benchmarks, tools, documentation, LayoutTests (~4 GB saved).
#
# Usage: pwsh -File fetch-webkit-source.ps1
# Requires: git 2.25+ with sparse-checkout support
# ============================================================================

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$WebKitDir = Join-Path $Root 'WebKit'
$REMOTE = "https://github.com/WebKit/WebKit.git"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Fetching WebKit source (sparse, shallow, blobless)" -ForegroundColor Cyan
Write-Host " Target: $WebKitDir" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# Check disk space
$drive = Get-WmiObject Win32_LogicalDisk -Filter "DeviceID='C:'"
$freeGB = [math]::Round($drive.FreeSpace / 1GB, 1)
Write-Host "Free disk: ${freeGB} GB" -ForegroundColor Yellow
if ($freeGB -lt 3) {
    Write-Host "!! Less than 3 GB free. Need at least ~2 GB for sparse checkout." -ForegroundColor Red
    exit 1
}

# Step 1: Initialize repo if needed
if (-not (Test-Path "$WebKitDir\.git")) {
    Write-Host "`n[1/4] Initializing sparse repo..." -ForegroundColor Green
    if (-not (Test-Path $WebKitDir)) { mkdir $WebKitDir | Out-Null }
    Push-Location $WebKitDir
    & git init
    & git remote add origin $REMOTE 2>$null
    & git remote set-url origin $REMOTE
    Pop-Location
}

Push-Location $WebKitDir

# Step 2: Configure sparse-checkout (only needed source dirs)
Write-Host "`n[2/4] Configuring sparse-checkout paths..." -ForegroundColor Green

$sparsePaths = @(
    'Source/WebCore/'
    'Source/JavaScriptCore/'
    'Source/WTF/'
    'Source/PAL/'
    'Source/WebKitLegacy/'
    'Source/cmake/'
    'Tools/Scripts/'
    'CMakeLists.txt'
)

# Write sparse-checkout patterns
$scDir = Join-Path $WebKitDir ".git\info"
if (-not (Test-Path $scDir)) { mkdir $scDir | Out-Null }
$scFile = Join-Path $scDir "sparse-checkout"
$sparseContent = ($sparsePaths -join "`n")
Set-Content -Path $scFile -Value $sparseContent -Encoding UTF8

# Enable sparse-checkout
& git config core.sparseCheckout true

# Also try modern sparse-checkout command
foreach ($p in $sparsePaths) {
    & git sparse-checkout add $p 2>$null
}

Write-Host "  Sparse paths configured: $($sparsePaths.Count) entries" -ForegroundColor Gray

# Step 3: Fetch (shallow + blobless)
Write-Host "`n[3/4] Fetching from GitHub (shallow + blobless)..." -ForegroundColor Green
Write-Host "  Downloads ~500MB-1GB of git metadata (no file contents yet)." -ForegroundColor Gray
Write-Host "  Files materialized in step 4." -ForegroundColor Gray
Write-Host ""

$tag = "webkitgtk-2.52.4"

& git fetch --filter=blob:none --depth=1 --no-tags --no-checkout origin "refs/tags/$tag`:refs/tags/$tag" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "  Tag fetch failed, trying 'main'..." -ForegroundColor Yellow
    & git fetch --filter=blob:none --depth=1 --no-tags --no-checkout origin main
    if ($LASTEXITCODE -ne 0) {
        Write-Host "!! Fetch failed. Check network and try again." -ForegroundColor Red
        Pop-Location
        exit 1
    }
    $tag = "main"
}

Write-Host "  Fetch complete." -ForegroundColor Green

# Step 4: Checkout (materializes only sparse-checked-out files)
Write-Host "`n[4/4] Checking out files (only sparse paths)..." -ForegroundColor Green
& git checkout $tag 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "  Trying 'main'..." -ForegroundColor Yellow
    & git checkout main 2>$null
}

Pop-Location

# Report
$size = 0
if (Test-Path $WebKitDir) {
    $size = (Get-ChildItem $WebKitDir -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
}
$sizeMB = [math]::Round($size / 1MB, 1)

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host " Done! WebKit source: ${sizeMB} MB" -ForegroundColor Green
Write-Host " Location: $WebKitDir" -ForegroundColor Green
Write-Host " Tag: $tag" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "Downloaded:" -ForegroundColor Cyan
Write-Host "  Source/WebCore/       engine + private headers" -ForegroundColor Gray
Write-Host "  Source/JavaScriptCore/ JS engine" -ForegroundColor Gray
Write-Host "  Source/WTF/           foundation library" -ForegroundColor Gray
Write-Host "  Source/PAL/           platform abstraction" -ForegroundColor Gray
Write-Host "  Source/WebKitLegacy/  WebCoreSupport headers" -ForegroundColor Gray
Write-Host "  Source/cmake/         CMake modules" -ForegroundColor Gray
Write-Host "  Tools/Scripts/        build scripts" -ForegroundColor Gray
Write-Host ""
Write-Host "Skipped (~4 GB saved):" -ForegroundColor Yellow
Write-Host "  Source/WebKit/        WebKit2 multi-process (not used)" -ForegroundColor Gray
Write-Host "  LayoutTests/          ~3 GB test suite" -ForegroundColor Gray
Write-Host "  Tools/TestWebKitAPI/  tests" -ForegroundColor Gray

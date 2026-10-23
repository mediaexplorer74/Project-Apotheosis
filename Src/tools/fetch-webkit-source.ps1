# ============================================================================
# fetch-webkit-source.ps1 - fetch WebKit 2.52.4 and apply the Apotheosis patch
#
# Downloads only the source directories the UWP port needs, checks out the exact
# upstream tag the patch was generated against (webkitgtk-2.52.4 = 7acdf5e1),
# and applies WebKitPatches/apotheosis-webkit-2.52.4.patch.
#
# Usage:
#   pwsh -File Src\tools\fetch-webkit-source.ps1
#   pwsh -File Src\tools\fetch-webkit-source.ps1 -SkipPatch
#
# Requires: git 2.25+ with sparse-checkout and partial-clone support.
# The WebKit/ tree is ignored by git; only the small patch file is committed.
# ============================================================================

param(
    [switch]$SkipPatch
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$WebKitDir = Join-Path $Root 'WebKit'
$Patch = Join-Path $Root 'WebKitPatches\apotheosis-webkit-2.52.4.patch'
$REMOTE = 'https://github.com/WebKit/WebKit.git'
$Tag = 'webkitgtk-2.52.4'

Write-Host '============================================================' -ForegroundColor Cyan
Write-Host ' Fetching WebKit 2.52.4 (sparse, shallow, blobless)' -ForegroundColor Cyan
Write-Host " Target: $WebKitDir" -ForegroundColor Cyan
Write-Host " Tag:    $Tag" -ForegroundColor Cyan
Write-Host '============================================================' -ForegroundColor Cyan

# Check disk space on the volume that holds the repo.
$driveLetter = (Get-Item $Root).PSDrive.Name
$freeGB = [math]::Round((Get-PSDrive $driveLetter).Free / 1GB, 1)
Write-Host "Free disk on ${driveLetter}: ${freeGB} GB" -ForegroundColor Yellow
if ($freeGB -lt 6) {
    Write-Host '!! Less than 6 GB free. Sparse WebKit needs roughly 3-6 GB including the checkout.' -ForegroundColor Red
    exit 1
}

# Step 1: initialize a repository if this is the first run.
if (-not (Test-Path (Join-Path $WebKitDir '.git'))) {
    Write-Host "`n[1/5] Initializing sparse repo..." -ForegroundColor Green
    New-Item -ItemType Directory -Force -Path $WebKitDir | Out-Null
    & git -C $WebKitDir init
    if ($LASTEXITCODE -ne 0) { throw 'git init failed' }
    & git -C $WebKitDir remote add origin $REMOTE 2>$null
    & git -C $WebKitDir remote set-url origin $REMOTE
    if ($LASTEXITCODE -ne 0) { throw 'could not configure the WebKit remote' }
}

Push-Location $WebKitDir
try {
    # Step 2: sparse paths. These are exactly the directories touched by the
    # Apotheosis patch; Tools/ is intentionally not modified by it.
    Write-Host "`n[2/5] Configuring sparse-checkout paths..." -ForegroundColor Green
    $sparsePaths = @(
        'Source/WebCore/'
        'Source/JavaScriptCore/'
        'Source/WTF/'
        'Source/WebKitLegacy/'
        'Source/cmake/'
        'Source/bmalloc/'
        'Source/ThirdParty/'
        'Source/Tools/'
        'Source/CMakeLists.txt'
        'Source/PlatformWinUWP.cmake'
        'CMakeLists.txt'
    )

    $scDir = Join-Path $WebKitDir '.git\info'
    New-Item -ItemType Directory -Force -Path $scDir | Out-Null
    Set-Content -Path (Join-Path $scDir 'sparse-checkout') -Value ($sparsePaths -join "`n") -Encoding ascii
    & git config core.sparseCheckout true
    Write-Host "  Sparse paths configured: $($sparsePaths.Count) entries" -ForegroundColor Gray

    # Step 3: fetch only the exact release tag. Do NOT fall back to main:
    # the patch was generated against 7acdf5e1 and will not apply to a newer tree.
    Write-Host "`n[3/5] Fetching $Tag from GitHub (shallow + blobless)..." -ForegroundColor Green
    Write-Host '  Metadata first; file contents are fetched during checkout.' -ForegroundColor Gray
    & git fetch --filter=blob:none --depth=1 --no-tags --no-checkout origin "refs/tags/$Tag`:refs/tags/$Tag"
    if ($LASTEXITCODE -ne 0) {
        throw "Could not fetch tag $Tag. Check the network and retry; no fallback to main is used because the patch is tag-specific."
    }

    # Step 4: materialize only the sparse paths.
    Write-Host "`n[4/5] Checking out files (only sparse paths)..." -ForegroundColor Green
    & git checkout $Tag
    if ($LASTEXITCODE -ne 0) { throw "git checkout $Tag failed" }
    $head = (& git rev-parse HEAD).Trim()
    Write-Host "  HEAD: $head" -ForegroundColor Gray
    if ($head -notlike '7acdf5e1*') {
        Write-Host "  !! HEAD is not the expected 7acdf5e1; patch application may fail." -ForegroundColor Yellow
    }

    # Step 5: apply the port patch. Re-running is safe: a reverse check detects
    # an already-patched tree and skips the apply.
    Write-Host "`n[5/5] Applying the Apotheosis WebKit patch..." -ForegroundColor Green
    if ($SkipPatch) {
        Write-Host '  -SkipPatch: tree left at upstream 2.52.4.' -ForegroundColor Yellow
    } else {
        if (-not (Test-Path $Patch)) { throw "Patch not found: $Patch" }

        & git apply --check --reverse $Patch 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host '  Patch is already applied; leaving the tree alone.' -ForegroundColor Yellow
        } else {
            & git apply --check $Patch
            if ($LASTEXITCODE -ne 0) {
                throw "The patch does not apply cleanly. Expected upstream $Tag at 7acdf5e1. Check 'git status' in $WebKitDir."
            }
            & git apply --3way $Patch
            if ($LASTEXITCODE -ne 0) {
                Write-Host '  --3way failed; retrying a plain apply...' -ForegroundColor Yellow
                & git apply $Patch
                if ($LASTEXITCODE -ne 0) { throw 'git apply failed' }
            }
            Write-Host '  Patch applied.' -ForegroundColor Green
        }
        $changed = @(git status --short --untracked-files=all)
        Write-Host "  Changed/untracked paths in WebKit: $($changed.Count)" -ForegroundColor Gray
    }
}
finally {
    Pop-Location
}

# Report working-tree size (excluding .git).
$size = (Get-ChildItem $WebKitDir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\\.git\\' } |
    Measure-Object Length -Sum).Sum
$sizeMB = [math]::Round($size / 1MB, 1)

Write-Host ''
Write-Host '============================================================' -ForegroundColor Green
Write-Host " Done. WebKit source: ${sizeMB} MB" -ForegroundColor Green
Write-Host " Location: $WebKitDir" -ForegroundColor Green
Write-Host " Tag:      $Tag" -ForegroundColor Green
Write-Host '============================================================' -ForegroundColor Green
Write-Host ''
Write-Host 'Sparse paths downloaded:' -ForegroundColor Cyan
Write-Host '  Source/WebCore, JavaScriptCore, WTF, WebKitLegacy,' -ForegroundColor Gray
Write-Host '  Source/cmake, bmalloc, ThirdParty, Tools, root CMake files' -ForegroundColor Gray
Write-Host ''
Write-Host 'Next: Src\tools\Get-Angle.ps1, then the SQLite and WebKit builds' -ForegroundColor Cyan
Write-Host 'in Doc\DEVELOPER-SETUP.md.' -ForegroundColor Cyan

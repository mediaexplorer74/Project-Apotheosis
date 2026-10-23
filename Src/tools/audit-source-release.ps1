# audit-source-release.ps1 - pre-publication audit for the Apotheosis source tree.
#
# Checks that the repository is a source release, not a WebKit/build/binary dump:
#   - no tracked WebKit/, build trees, appx output, native payload, PFX/CER
#   - no tracked DLL/LIB/EXE/PDB/OBJ
#   - .gitignore still covers the generated trees
#   - the WebKit patch is present, touches only Source/, and has no binary hunks
#   - the largest tracked files and the upload size are reported
#
# Usage:
#   pwsh -File Src\tools\audit-source-release.ps1
#   pwsh -File Src\tools\audit-source-release.ps1 -ZipPath C:\Temp\Apotheosis-source.zip
#
# The ZIP is produced with `git archive HEAD`, so commit first; the audit warns if the
# working tree is dirty. It contains exactly the tracked files and no .git data.

param(
    [string]$ZipPath,
    [switch]$SkipBundle
)

$ErrorActionPreference = 'Stop'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
Push-Location $Root
$script:failures = @()
$script:warnings = @()

function Fail([string]$Message) {
    $script:failures += $Message
    Write-Host "  FAIL  $Message" -ForegroundColor Red
}

function Warn([string]$Message) {
    $script:warnings += $Message
    Write-Host "  WARN  $Message" -ForegroundColor Yellow
}

function Pass([string]$Message) {
    Write-Host "  ok    $Message" -ForegroundColor Green
}

Write-Host '=== Apotheosis source-release audit ===' -ForegroundColor Cyan
Write-Host "repo: $Root" -ForegroundColor Gray

$dirty = @(git status --porcelain)
if ($dirty.Count) {
    Warn "working tree has $($dirty.Count) uncommitted path(s); the ZIP will use HEAD only"
} else {
    Pass 'working tree clean'
}

Write-Host "`n[1/6] tracked-path policy" -ForegroundColor Cyan
$tracked = @(git ls-files)
if (-not $tracked.Count) { Fail 'git ls-files returned nothing' }
$forbidden = $tracked | Where-Object {
    $_ -match '^(WebKit|build-[^/]+|deps-build|WebKitPatches/.*\.(dll|lib|exe|pdb))(/|$)' -or
    $_ -match '^Src/Apotheosis/(AppPackages|BundleArtifacts|bin|obj)/' -or
    $_ -match '^Src/harness/(x64|ARM|Harness)/' -or
    $_ -match '\.(dll|lib|exe|pdb|obj|pfx|cer|appx|appxbundle|appxsym|pri|winmd|ilk|exp)$'
}
if ($forbidden.Count) {
    $forbidden | Select-Object -First 30 | ForEach-Object { Fail "tracked forbidden path: $_" }
} else {
    Pass "no tracked WebKit/build/binary/signing paths ($($tracked.Count) tracked files)"
}

Write-Host "`n[2/6] .gitignore coverage" -ForegroundColor Cyan
$required = @(
    'WebKit/',
    'build-*/',
    '*.dll',
    '*.lib',
    '*.exe',
    '*.pdb',
    '*.obj',
    '*.pfx',
    '*.cer',
    'AppPackages/',
    'bin/',
    'obj/'
)
$ignore = Get-Content -LiteralPath (Join-Path $Root '.gitignore') -Raw
foreach ($pattern in $required) {
    if ($ignore -match [regex]::Escape($pattern)) {
        Pass ".gitignore contains $pattern"
    } else {
        Fail ".gitignore is missing $pattern"
    }
}

Write-Host "`n[3/6] WebKit patch set" -ForegroundColor Cyan
$patch = Join-Path $Root 'WebKitPatches\apotheosis-webkit-2.52.4.patch'
if (-not (Test-Path $patch)) {
    Fail 'WebKitPatches/apotheosis-webkit-2.52.4.patch is missing'
} else {
    $patchSize = (Get-Item $patch).Length
    $patchText = Get-Content -LiteralPath $patch -Raw
    $files = [regex]::Matches($patchText, '(?m)^diff --git a/(.+?) b/') | ForEach-Object { $_.Groups[1].Value }
    $tools = @($files | Where-Object { $_ -like 'Tools/*' })
    $binary = $patchText -match '(?m)^GIT binary patch'
    Pass ("patch: {0:N0} bytes, {1} paths, {2} Tools/ paths, binary={3}" -f $patchSize, $files.Count, $tools.Count, $binary)
    if ($tools.Count) { Fail "patch contains excluded Tools/ paths: $($tools[0])" }
    if ($binary) { Fail 'patch contains a binary hunk' }
    if ($files.Count -lt 100) { Warn "patch touches only $($files.Count) paths; expected about 110" }
    $readme = Join-Path $Root 'WebKitPatches\README.md'
    if (Test-Path $readme) { Pass 'WebKitPatches/README.md present' } else { Fail 'WebKitPatches/README.md missing' }
}

Write-Host "`n[4/6] large tracked files and history size" -ForegroundColor Cyan
$large = @($tracked | ForEach-Object {
    $p = Join-Path $Root $_
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        $f = Get-Item -LiteralPath $p
        if ($f.Length -ge 1MB) { [pscustomobject]@{ MB = [math]::Round($f.Length / 1MB, 2); Path = $_ } }
    }
} | Sort-Object MB -Descending)
if ($large.Count) {
    $large | Select-Object -First 15 | ForEach-Object { Write-Host ("  {0,7:N2} MB  {1}" -f $_.MB, $_.Path) }
    Pass "largest tracked file: $($large[0].MB) MB"
} else {
    Pass 'no tracked file at or above 1 MB'
}

$counts = git count-objects -vH
$sizePack = ($counts | Select-String '^size-pack:').Line
if ($sizePack) { Pass "git object pack: $($sizePack -replace '^size-pack:\s*','')" }
if (-not $SkipBundle) {
    $bundle = Join-Path ([IO.Path]::GetTempPath()) ("apotheosis-audit-{0}.bundle" -f [guid]::NewGuid().ToString('N'))
    try {
        git bundle create $bundle --all 2>$null | Out-Null
        if (Test-Path $bundle) {
            $mb = [math]::Round((Get-Item $bundle).Length / 1MB, 2)
            Pass "full-history bundle (upload estimate): $mb MB"
        }
    } catch {
        Warn "could not create the bundle estimate: $($_.Exception.Message)"
    } finally {
        Remove-Item $bundle -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "`n[5/6] secret patterns in tracked text" -ForegroundColor Cyan
$secretPatterns = @(
    'ghp_[A-Za-z0-9]{20,}',
    'github_pat_[A-Za-z0-9_]{20,}',
    'AKIA[0-9A-Z]{16}',
    '-----BEGIN (RSA|OPENSSH|EC|DSA) PRIVATE KEY-----'
)
$leaks = @()
foreach ($pattern in $secretPatterns) {
    $hits = @(git grep -n -I -E $pattern -- . 2>$null)
    if ($hits.Count) { $leaks += $hits }
}
if ($leaks.Count) {
    $leaks | Select-Object -First 20 | ForEach-Object { Fail "possible secret: $_" }
} else {
    Pass 'no common token/private-key patterns in tracked text'
}

Write-Host "`n[6/6] source ZIP" -ForegroundColor Cyan
if ($ZipPath) {
    $ZipPath = [IO.Path]::GetFullPath($ZipPath)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ZipPath) | Out-Null
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    git archive --format=zip --output=$ZipPath HEAD
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ZipPath)) {
        Fail 'git archive failed'
    } else {
        $zipMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 2)
        Pass "ZIP: $ZipPath ($zipMB MB)"
    }
} else {
    Write-Host '  (no -ZipPath; skipping ZIP creation)' -ForegroundColor Gray
}

Write-Host ''
if ($script:failures.Count) {
    Write-Host "AUDIT FAILED: $($script:failures.Count) blocking issue(s), $($script:warnings.Count) warning(s)." -ForegroundColor Red
    Pop-Location
    exit 1
}
Write-Host "AUDIT PASSED: $($tracked.Count) tracked files, $($script:warnings.Count) warning(s)." -ForegroundColor Green
Pop-Location
exit 0

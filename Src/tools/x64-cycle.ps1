# x64-cycle.ps1 -- one command for the whole local x64 test loop:
#   bump version -> (relink driver) -> build appx -> reinstall -> seed LocalState -> launch -> collect logs
#
# Replaces install-local-x64.ps1, which hardcoded both an absolute repo path and a single
# appx version. Everything here is relative to $env:APOTHEOSIS_ROOT (Src\setenv.ps1).
#
# Why the version bump is mandatory: Add-AppxPackage fails 0x80073CFB when reinstalling the
# same version, and the Remove-AppxPackage that would otherwise be needed WIPES LocalState.
# Why LocalState is seeded AFTER install: same reason -- anything written before the install
# is deleted by it. settings.ini/home= is how the startup URL is chosen with no rebuild.
#
# Examples:
#   pwsh -File Src\tools\x64-cycle.ps1                          # local test.html
#   pwsh -File Src\tools\x64-cycle.ps1 -Url http://example.com
#   pwsh -File Src\tools\x64-cycle.ps1 -Url https://news.ycombinator.com -WaitSec 90
#   pwsh -File Src\tools\x64-cycle.ps1 -SkipDriver -SkipBuild    # re-run last appx, new URL

param(
    # Startup URL. '' or 'about:home' = built-in home page. 'test' = the seeded local test.html.
    [string]$Url = 'test',
    [switch]$SkipDriver,
    [switch]$SkipBuild,
    [switch]$NoLaunch,
    [int]$WaitSec = 45,
    # UI language index: 0=zh, 1=en, 2=ru (matches SetLangCombo order in MainPage.xaml).
    [int]$Lang = 1,
    # Apotheosis: seed gpudefault=1 instead of 0.
    #
    # This script has always written gpudefault=0, which means every "the bench survives it" result
    # was obtained with GPU compositing switched off -- while the phone runs with it on, calls
    # EnableGpu after the first successful load, and dies shortly after WebCoreGpuInit returns. The
    # bench was therefore not running the code being blamed. Measured 2026-08-21; the crash in
    # Doc/PUMPLOOP-SILENT-DEATH.md is the case in point.
    #
    # Left off by default because the software path is the baseline the project rests on, and most
    # cycles want it. Pass -Gpu when the point is to mirror the device.
    [switch]$Gpu
)

$ErrorActionPreference = 'Stop'

$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$Harness  = Join-Path $Root 'Src\harness'
$Manifest = Join-Path $Harness 'Package.appxmanifest'

function Say([string]$m) { Write-Host "[cycle] $m" -ForegroundColor Cyan }

# ---------------------------------------------------------------- 1. version bump
[xml]$mx = Get-Content $Manifest
$idNode = $mx.Package.Identity
$parts  = $idNode.Version.Split('.')
$parts[3] = [string]([int]$parts[3] + 1)
$newVer = $parts -join '.'
$idNode.Version = $newVer
$mx.Save($Manifest)
Say "version -> $newVer"

# ---------------------------------------------------------------- 2. driver relink
if (-not $SkipDriver) {
    Say 'relinking WebCoreDriver-gpu.lib'
    & (Join-Path $Root 'Src\port\link-driver-gpu-x64.ps1')
    if ($LASTEXITCODE) { throw "driver relink failed ($LASTEXITCODE)" }
}

# ---------------------------------------------------------------- 3. appx build
if (-not $SkipBuild) {
    # Apotheosis: go through _build-appx-x64.bat rather than calling MSBuild here.
    #
    # This used to invoke MSBuild.exe directly, found via vswhere, with no Visual Studio environment
    # around it. MSBuild then resolves the C++ toolset itself and picks the 32-bit host targeting
    # x86, so the objects come out x86 while the project targets x64 and the link dies with
    #     pch.obj : fatal error LNK1112: module machine type 'x86' conflicts with target machine
    #               type 'x64'
    # followed by MSB4181 from CompileXaml, which is only the cascade and hides the real line.
    # Measured 2026-08-21. The batch file exists precisely because msbuild needs a full VS
    # environment and the nested quoting for vcvars64.bat does not survive being passed through a
    # shell -- see CLAUDE.md. Having two ways to build the same appx, one of them without vcvars,
    # was the defect.
    Say 'building appx (this is the long step)'
    $bat = Join-Path $Root 'Src\tools\_build-appx-x64.bat'
    if (-not (Test-Path $bat)) { throw "missing $bat" }
    & cmd.exe /c "`"$bat`""
    if ($LASTEXITCODE) { throw "msbuild failed ($LASTEXITCODE)" }
}

# ---------------------------------------------------------------- 4. locate appx
$appx = Get-ChildItem -Path (Join-Path $Harness 'AppPackages') -Recurse -Filter "Harness_${newVer}_x64.appx" -ErrorAction SilentlyContinue |
        Select-Object -First 1
if (-not $appx) {
    # Fall back to the newest appx on disk (covers -SkipBuild, where $newVer was not built).
    $appx = Get-ChildItem -Path (Join-Path $Harness 'AppPackages') -Recurse -Filter 'Harness_*_x64.appx' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $appx) { throw 'no Harness_*_x64.appx found under Src\harness\AppPackages' }
    Say "using newest appx on disk: $($appx.Name)"
}
Say ("appx {0} ({1:N1} MB)" -f $appx.Name, ($appx.Length / 1MB))   # .Length, never ls -l

# ---------------------------------------------------------------- 5. reinstall
$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if ($pkg) { Say "removing $($pkg.PackageFullName)"; Remove-AppxPackage $pkg.PackageFullName }
Add-AppxPackage -Path $appx.FullName
$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness
Say "installed $($pkg.PackageFullName)"

# ---------------------------------------------------------------- 6. seed LocalState
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
New-Item -ItemType Directory -Force -Path $local | Out-Null

$testHtml = Join-Path $local 'test.html'
@'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Apotheosis local probe</title>
<style>
  body { font-family: sans-serif; margin: 16px; color: #202124; background: #fff; }
  h1 { font-size: 22px; margin: 0 0 10px; }
  .box { border: 1px solid #c0c0c0; padding: 10px; margin: 12px 0; background: #f6f6f6; }
  a { color: #1a0dab; }
</style></head>
<body>
  <h1>Local probe page</h1>
  <p>Plain paragraph text, ASCII only, so a tofu box here means the primary font failed.</p>
  <div class="box">A bordered block: exercises borders, padding, and a background fill.</div>
  <p>Anchor for the hit-test path: <a href="http://example.com/">example.com</a></p>
  <p style="color:#c5221f">Inline style, red.</p>
  <p>CJK fallback check (SimHei): &#x4F60;&#x597D;&#x4E16;&#x754C;</p>
  <p>Cyrillic check: &#x41F;&#x440;&#x438;&#x432;&#x435;&#x442;</p>
</body></html>
'@ | Set-Content -Path $testHtml -Encoding UTF8

# Apotheosis 2026-09-04: seed the compositing-layer probe too. navseq.txt entry 1b points at
# LocalState\layertest.html, but only test.html above was ever written here -- and the install in
# step 5 wipes LocalState, so the file a previous session had seeded by hand was gone. The load
# then failed with curl code 37 ("Could not open file") and the run skipped the ONLY entry in the
# sequence that reaches gpuCompositeReadback. It skipped it visibly, in log.txt, but the run still
# read as a pass at a glance. will-change + translateZ is what forces WebCore to give the page its
# own GraphicsLayer; without it rootLayer() stays null and the whole GPU branch is dead code.
$layerHtml = Join-Path $local 'layertest.html'
@'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Apotheosis layer probe</title>
<style>
  body { font-family: sans-serif; margin: 16px; color: #202124; background: #fff; }
  .layer { will-change: transform; transform: translateZ(0);
           width: 320px; height: 200px; background: linear-gradient(#4285f4, #0f9d58);
           color: #fff; padding: 12px; box-sizing: border-box; }
</style></head>
<body>
  <h1>Compositing layer probe</h1>
  <div class="layer">This block is promoted to its own compositing layer.</div>
  <p>If this page renders, gpuCompositeReadback ran and returned pixels.</p>
</body></html>
'@ | Set-Content -Path $layerHtml -Encoding UTF8

$homeValue = switch ($Url) {
    'test'       { "file:///$($testHtml -replace '\\','/')" }
    'about:home' { '' }
    ''           { '' }
    default      { $Url }
}
@(
    'search=0'
    "home=$homeValue"
    'ua=0'
    'zoom=100'
    'tabmode=0'
    "gpudefault=$(if ($Gpu) { 1 } else { 0 })"
    'ua_custom='
    "lang=$Lang"
) -join "`n" | Set-Content -Path (Join-Path $local 'settings.ini') -Encoding UTF8
Say "home=$(if ($homeValue) { $homeValue } else { 'about:home' })"

# ---------------------------------------------------------------- 7. launch + collect
if ($NoLaunch) { Say "not launching; localstate=$local"; return }

$aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!$aid"
Say "launched $aid; waiting ${WaitSec}s"
Start-Sleep -Seconds $WaitSec

foreach ($f in 'log.txt', 'port-trace.txt', 'stage.txt', 'glyph.log') {
    $p = Join-Path $local $f
    Write-Host "`n===== $f =====" -ForegroundColor Yellow
    if (Test-Path $p) { Get-Content $p -Tail 40 } else { Write-Host '(absent)' }
}
Write-Host "`n[cycle] localstate=$local" -ForegroundColor Cyan

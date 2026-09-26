# make-layer-test-pages.ps1 -- is a composited layer's backing store the per-navigation accumulator?
#
# Apotheosis 2026-09-24. The evidence this page set exists to test, and the reason the hypothesis is the
# surviving one rather than a guess:
#   * `file://` control pages (d0/g0/gtiny, and https://example.com/) are FLAT across navigations -- 0 to
#     +1 MB/load, measured repeatedly.
#   * `https://dzen.ru/` grows ~50-90 MB/load, monotonically, in every run, and a fresh process reaches
#     ~1 GB in ten loads. Its `[STAGE] after-load ... compositing=1`; the flat pages are `compositing=0`.
#   * The two MemoryCache levers do not change it, and neither do they touch GraphicsLayers or their
#     backing stores -- they reach CachedResource payload.
#   * The retained Documents are now priced and are far too small: ~1 document per load with
#     `nodes=1518..1853` and no live render tree, plus two ~12-20-node SVG-image documents. Fractions of
#     a MB together, against 50-90 MB/load.
# So the difference between the flat pages and the growing one that no instrument has yet touched is
# COMPOSITING, and the cheapest way to test that is to change nothing but layer promotion.
#
# The one respect that differs between the arms below is layer promotion, and nothing else: same element
# count, same layout, same styles, same painted area, same viewport fit. `will-change: transform` is what
# promotes; the control has the identical box without it.
#   L0.html -- 20 plain boxes                     (control: no promoted layers)
#   L1.html -- the same 20 boxes, will-change: transform on each (20 composited layers)
#
# Read: `SL: live pages` / RSS over N loads of each, in ONE process, alternating. If L1 grows and L0 does
# not, the accumulator is per-layer and lives outside every cache the port has tried to release.
#
# There is a second reading worth taking from the same run and it needs no extra page: `WebCoreGpuLayerInfo`
# (the GPU toggle's dump) reports the layer tree as text, so a manual toggle after L1 gives the layer count
# and the backing-store sizes directly. It is on-demand only, so it does not run in a scripted loop.
param(
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'

if (-not $OutDir) {
    $pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
    if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
    $OutDir = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState\po"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Write-Grid {
    param([string]$Name, [string]$ExtraStyle, [string]$Note)
    $cells = ''
    for ($i = 0; $i -lt 20; $i++) {
        $cells += "<div class=""c"" style=""$ExtraStyle"">$i</div>`n"
    }
    $html = @"
<!doctype html>
<html><head><meta charset="utf-8"><title>$Name</title>
<style>
  body { margin: 0; }
  .c { width: 200px; height: 100px; float: left; background: #4a90d9; color: #fff;
       font: 14px sans-serif; text-align: center; }
</style>
</head>
<body>
<!-- $Note -->
$cells
</body></html>
"@
    # ASCII only: a test page's encoding accident would be misread as an engine defect.
    $path = Join-Path $OutDir "$Name.html"
    [System.IO.File]::WriteAllText($path, $html, [System.Text.Encoding]::ASCII)
    Write-Host "wrote $path"
}

Write-Grid -Name 'L0' -ExtraStyle '' `
    -Note 'control arm: 20 boxes, no layer promotion'
Write-Grid -Name 'L1' -ExtraStyle 'will-change: transform;' `
    -Note 'test arm: the same 20 boxes, each promoted by will-change'

Write-Host ''
Write-Host 'Drive in ONE process, alternating arms, e.g.:'
Write-Host ('  pwsh -File Src\tools\rss-loop.ps1 -Url "file:///<po>\L0.html" -Count 5')
Write-Host ('  pwsh -File Src\tools\rss-loop.ps1 -Url "file:///<po>\L1.html" -Count 5')
Write-Host 'and read the deltas. A growth that survives the alternation is the finding.'

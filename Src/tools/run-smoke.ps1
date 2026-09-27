# Ten-site smoke run for Apotheosis, driven through the autocmd controller.
#
# MVP requires a per-site record of: URL, load result, painted pixels/content,
# scroll result, back result, crash or hang marker -- and it says explicitly that
# a test which only checks rc=0 is incomplete. So for each site this captures
# four independent things, and fails the site when the first two disagree:
#
#   1. rc        -- the engine's return code for the load
#   2. nonwhite  -- the share of non-background pixels in a real screenshot,
#                   the only evidence that something was actually PAINTED
#   3. swipe_ms  -- a real touch-path drag, timed
#   4. alive     -- is the process still running afterwards
#
# A site that loads with rc=0 but paints nothing is a FAIL, not a pass. That is
# the point of the exercise: the blank-white-page failure mode is indistinguishable
# from success if you only read the return code.
param(
  [string]$OutDir = "$env:USERPROFILE\Documents\apotheosis-smoke",
  [int]$SettleMs = 6000
)

$ErrorActionPreference = 'Continue'
$pkg = Get-AppxPackage -Name '*Apotheosis*'
if (-not $pkg) { Write-Host "Apotheosis is not installed" -ForegroundColor Red; exit 1 }
$ls = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState')
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
function Get-NonWhiteShare([string]$path) {
  # Shares of pixels differing from a near-white background. A blank page scores
  # ~0; a painted page scores well above it. Downsampled by `step` because
  # GetPixel per pixel over a full desktop is far too slow for twelve sites.
  $bmp = [Drawing.Bitmap]::new($path)
  try {
    $w = $bmp.Width; $h = $bmp.Height
    $step = [math]::Max(1, [int]([math]::Min($w, $h) / 160))
    $total = 0; $nonwhite = 0
    for ($y = 0; $y -lt $h; $y += $step) {
      for ($x = 0; $x -lt $w; $x += $step) {
        $c = $bmp.GetPixel($x, $y)
        $total++
        if ($c.R -lt 240 -or $c.G -lt 240 -or $c.B -lt 240) { $nonwhite++ }
      }
    }
    if ($total -eq 0) { return 0.0 }
    return [math]::Round(100.0 * $nonwhite / $total, 2)
  } finally { $bmp.Dispose() }
}

function Save-Shot([string]$path) {
  $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = [Drawing.Bitmap]::new($b.Width, $b.Height)
  $g = [Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
  $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}


# Ordered plainest to hardest, so that if something breaks, the run says where
# the line stopped being a browser and became a renderer. The last entry is a
# domain that cannot resolve: MVP item 8 requires that a failure is REPORTED,
# not silently shown as a white page, so a blank result is the correct outcome.
$sites = @(
  [pscustomobject]@{ Name='01-example-com'; Url='https://example.com'; Note='flat HTML, baseline' }
  [pscustomobject]@{ Name='02-info-cern-ch'; Url='https://info.cern.ch'; Note='first web page ever, no CSS' }
  [pscustomobject]@{ Name='03-ycombinator'; Url='https://news.ycombinator.com'; Note='subresources, table layout' }
  [pscustomobject]@{ Name='04-ya-ru'; Url='https://ya.ru'; Note='known baseline, images + widget' }
  [pscustomobject]@{ Name='05-wikipedia-m'; Url='https://en.m.wikipedia.org'; Note='many small assets' }
  [pscustomobject]@{ Name='06-w3-org'; Url='https://w3.org'; Note='legacy table layout' }
  [pscustomobject]@{ Name='07-rbc-ru'; Url='https://rbc.ru'; Note='news, many images' }
  [pscustomobject]@{ Name='08-lenta-ru'; Url='https://lenta.ru'; Note='news, lazy loading' }
  [pscustomobject]@{ Name='09-habr-com'; Url='https://habr.com'; Note='blog layout' }
  [pscustomobject]@{ Name='10-reddit-com'; Url='https://reddit.com'; Note='heavy JS' }
  [pscustomobject]@{ Name='11-dzen-ru'; Url='https://dzen.ru'; Note='SPA + autologin redirect' }
  [pscustomobject]@{ Name='12-bad-domain'; Url='https://no-such-host-apotheosis-xyzzy.invalid'; Note='must report failure' }
)

$rows = @()
foreach ($s in $sites) {
  Write-Host ("--> {0}  {1}" -f $s.Name, $s.Url) -ForegroundColor Cyan
  $log = Join-Path $ls 'autocmd-log.txt'
  Remove-Item $log -ErrorAction SilentlyContinue
  $cmds = @("# site $($s.Name)", "goto $($s.Url)", "wait $SettleMs", "hash", "swipe 1200 25 16")
  Set-Content (Join-Path $ls 'autocmd.txt') -Value $cmds -Encoding ASCII
  Start-Sleep -Seconds ([math]::Ceiling(($SettleMs + 9000) / 1000))

  $shot = Join-Path $OutDir ($s.Name + '.png')
  Save-Shot $shot
  $nonwhite = Get-NonWhiteShare $shot

  $proc = Get-Process -Name 'Apotheosis' -ErrorAction SilentlyContinue
  $alive = [bool]$proc
  $rc = $null; $swipeLine = ''; $hashLine = ''; $stageUrl = ''
  # stage.txt, not the autocmd log: the load result lands there as
  # "after-load url=... rc=0 compositing=0", written by the loader itself.
  $stage = Join-Path $ls 'stage.txt'
  if (Test-Path $stage) {
    $sl = Get-Content $stage
    foreach ($l in $sl) {
      if ($l -match 'after-load url=(\S+) rc=(-?\d+)') { $stageUrl = $Matches[1]; $rc = [int]$Matches[2] }
    }
    $dl = @($sl | Where-Object { $_ -match '^\s*diag:' })
    if ($dl.Count) { $hashLine = ($dl[-1] -replace '\s+', ' ').Trim() }
  }
  if (Test-Path $log) {
    foreach ($l in (Get-Content $log)) {
      if ($l -match 'swipe end totalMs=(\d+)') { $swipeLine = $Matches[1] }
    }
  }
  $loadOk = ($rc -eq 0)
  # The stage file keeps only the LAST after-load, so a stale rc from the previous site
  # would otherwise be read as this site's result. Compare the URL to be sure.
  $urlMatched = ($stageUrl -ne '') -and (($s.Url -replace '/$','') -eq ($stageUrl -replace '/$',''))
  if (-not $urlMatched) { $loadOk = $false }
  $painted = ($nonwhite -ge 0.5)
  $expectFail = ($s.Name -eq '12-bad-domain')
  # For the unreachable domain a blank page IS the pass. For everything else,
  # rc=0 with no pixels is the exact failure this harness exists to catch.
  $verdict = if ($expectFail) { if ($loadOk -and $painted) { 'FAIL' } else { 'PASS' } }
             else { if ($loadOk -and $painted) { 'PASS' } else { 'FAIL' } }

  $rows += [pscustomobject]@{
    site = $s.Name; url = $s.Url; note = $s.Note
    rc = $rc; nonwhite = $nonwhite; painted = $painted
    swipe_ms = $swipeLine; alive = $alive; verdict = $verdict
    screenshot = (Split-Path $shot -Leaf)
  }
  Write-Host ("    rc={0} nonwhite={1}% swipe={2}ms alive={3} => {4}" -f $rc, $nonwhite, $swipeLine, $alive, $verdict) `
    -ForegroundColor $(if ($verdict -eq 'PASS') { 'Green' } else { 'Yellow' })

  if (-not $alive) {
    Write-Host "    process died at $($s.Name); restarting for the rest" -ForegroundColor Red
    $app = ($pkg | Get-AppxPackageManifest).Package.Applications.Application | Select-Object -First 1
    Start-Process explorer.exe -ArgumentList "shell:AppsFolder\$($pkg.PackageFamilyName)!$($app.Id)"
    Start-Sleep -Seconds 8
  }
}

$rows | Export-Csv (Join-Path $OutDir 'smoke.csv') -NoTypeInformation -Encoding UTF8
Write-Host ''
Write-Host '=== SUMMARY ==='
$rows | Format-Table site, rc, nonwhite, swipe_ms, verdict -AutoSize | Out-String -Width 200 | Write-Host
$pass = @($rows | Where-Object { $_.verdict -eq 'PASS' }).Count
Write-Host ("passed {0} / {1};  report in {2}" -f $pass, $rows.Count, $OutDir)

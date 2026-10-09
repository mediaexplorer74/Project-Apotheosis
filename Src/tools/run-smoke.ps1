# Thirteen-site smoke run for Apotheosis, driven through the autocmd controller.
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
  [ValidateSet('mobile','desktop')][string]$Mode = 'mobile',
  [int]$SettleMs = 6000,          # retained for callers; the loop now waits on the load itself
  [int]$LoadTimeoutSec = 45       # per site, for the after-load line to name the site's URL
)

# Mode is a first-class axis, not a footnote.
#
# Before this, desktop vs mobile could only be selected by hand-editing `ua=` in settings.ini, so
# "this site renders wrong in desktop mode" was a claim nobody could test. Each site now carries a
# `ua <mode>` command ahead of its `goto` -- ahead, because the user agent affects the layout of the
# NEXT request and the engine keeps the document it already has -- and `mode` is a column in the
# record. Run the script once per mode, into separate directories:
#
#     run-smoke.ps1                      # mobile, the historical path
#     run-smoke.ps1 -Mode desktop        # writes to apotheosis-smoke-desktop
#
# The two runs are deliberately not merged into one pass: a site's verdict is a statement about one
# mode, and a CSV where the column changes halfway down is a record nobody can compare.
if ($Mode -eq 'desktop' -and $OutDir -eq "$env:USERPROFILE\Documents\apotheosis-smoke") {
  $OutDir = "$OutDir-desktop"
}

$ErrorActionPreference = 'Continue'
$pkg = Get-AppxPackage -Name '*Apotheosis*'
if (-not $pkg) { Write-Host "Apotheosis is not installed" -ForegroundColor Red; exit 1 }
$ls = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState')
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type -AssemblyName System.Windows.Forms, System.Drawing

# DPI awareness, and it has to come before anything asks for a window size.
#
# This bench is a 2736x1728 surface at 200% scale. A DPI-unaware PowerShell is
# virtualized: PrimaryScreen.Bounds answers 1368x864, CopyFromScreen captures that
# virtual rectangle, and both the picture and the nonwhite share then describe a
# crop of the window instead of the window -- which is a measurement that lies,
# exactly the class of error this harness exists to avoid. SetProcessDPIAware()
# first, then the bounds mean physical pixels.
if (-not ('ApoDpi' -as [type])) {
  Add-Type -Namespace '' -Name ApoDpi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
  [void][ApoDpi]::SetProcessDPIAware()
}

# The app's window opens larger than the screen and must be fitted before the
# capture, for the same reason drive-csharp.ps1 does it: without this the bottom
# chrome (and on a fresh process the right edge) is simply outside the bitmap.
#
# Get-Process().MainWindowHandle is 0 for a UWP app, and the top-level window is
# owned by ApplicationFrameHost.exe, not by Apotheosis.exe -- so the window is
# found by class and title. The callback writes the SCRIPT scope on purpose: a
# local declared here would be shadowed by the callback's own scope and the value
# would be lost (that bug cost a round of "no window found").
function Fit-AppWindow {
  if (-not ('Apo.WinPlace' -as [type])) {
    Add-Type -Namespace Apo -Name WinPlace -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
'@
  }
  $script:apoHwnd = $null
  $cb = [Apo.WinPlace+EnumProc] {
    param($h, $l)
    if ([Apo.WinPlace]::IsWindowVisible($h)) {
      $t = New-Object System.Text.StringBuilder 512
      [void][Apo.WinPlace]::GetWindowText($h, $t, 512)
      if ($t.ToString() -eq 'Apotheosis') {
        $c = New-Object System.Text.StringBuilder 512
        [void][Apo.WinPlace]::GetClassName($h, $c, 512)
        if ($c.ToString() -eq 'ApplicationFrameWindow') { $script:apoHwnd = $h }
      }
    }
    return $true
  }
  [void][Apo.WinPlace]::EnumWindows($cb, [IntPtr]::Zero)
  if (-not $script:apoHwnd) { return [Windows.Forms.Screen]::PrimaryScreen.Bounds }
  $wa = [Windows.Forms.Screen]::PrimaryScreen.WorkingArea
  [void][Apo.WinPlace]::SetWindowPos($script:apoHwnd, [IntPtr]::Zero, $wa.X, $wa.Y, $wa.Width, $wa.Height, 0x0004 -bor 0x0010)
  Start-Sleep -Milliseconds 900
  return $wa
}

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
  # Fit first, then capture exactly the rectangle the window was moved into; the
  # nonwhite share is meant to describe the app's pixels, not the desktop's.
  $b = Fit-AppWindow
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
  # The fork's own MVP target, so the acceptance run covers the page the maintainer
  # actually asked for and not only a generic list.
  [pscustomobject]@{ Name='13-nokia-archive'; Url='https://nokiadesignarchive.aalto.fi/'; Note='MVP target: SPA archive, tall page' }
)

$rows = @()
foreach ($s in $sites) {
  Write-Host ("--> {0}  {1}" -f $s.Name, $s.Url) -ForegroundColor Cyan
  $log = Join-Path $ls 'autocmd-log.txt'
  $stage = Join-Path $ls 'stage.txt'
  Remove-Item $log -ErrorAction SilentlyContinue
  # stage.txt is cleared first, so anything found in it below belongs to THIS site.
  Remove-Item $stage -ErrorAction SilentlyContinue

  # Batch 1 is the navigation alone, and the wait that follows is for the LOAD, not for a
  # guess at how long a load takes.
  #
  # The previous version slept a fixed (SettleMs + 9000) ms and then read whatever stage.txt
  # held. On a heavy site the load outlives the sleep, so the script read the PREVIOUS site's
  # after-load line and the previous site's diag, and the record shifted by one: reddit came
  # back with an empty rc and dzen and bad-domain both reported the identical 35.65%. Two
  # sites cannot paint the same page; the numbers were not the sites'.
  #
  # The condition is the site's own URL appearing in an after-load line, which is the
  # loader's own statement that it finished.
  Set-Content (Join-Path $ls 'autocmd.txt') -Value @("# site $($s.Name)", "ua $Mode", "goto $($s.Url)") -Encoding UTF8

  $want = $s.Url.TrimEnd('/')
  $deadline = (Get-Date).AddSeconds($LoadTimeoutSec)
  $stageUrl = ''; $rc = $null; $sl = @()
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    if (-not (Test-Path $stage)) { continue }
    try { $sl = Get-Content $stage -ErrorAction Stop } catch { continue }
    $got = $false
    foreach ($l in $sl) {
      if ($l -match 'after-load url=(\S+) rc=(-?\d+)') {
        $stageUrl = $Matches[1]; $rc = [int]$Matches[2]; $got = $true
      }
    }
    if ($got -and ($stageUrl.TrimEnd('/') -eq $want)) { break }
  }

  # Batch 2, the touch-path drag, and again wait for its own end marker rather than sleeping.
  # It is a separate batch because a swipe issued before the session is live is refused, not
  # queued ("swipe ignored: no session"), and it would then never appear in the log at all.
  $swipeLine = ''
  Set-Content (Join-Path $ls 'autocmd.txt') -Value @("swipe 1200 25 16") -Encoding UTF8
  $swDeadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $swDeadline) {
    Start-Sleep -Milliseconds 300
    if (-not (Test-Path $log)) { continue }
    foreach ($l in (Get-Content $log -ErrorAction SilentlyContinue)) {
      if ($l -match 'swipe end totalMs=(\d+)') { $swipeLine = $Matches[1] }
    }
    if ($swipeLine -ne '') { break }
  }

  $shot = Join-Path $OutDir ($s.Name + '.png')
  Save-Shot $shot
  $nonwhite = Get-NonWhiteShare $shot

  $proc = Get-Process -Name 'Apotheosis' -ErrorAction SilentlyContinue
  $alive = [bool]$proc
  $hashLine = ''; $engNonwhite = $null
  $dl = @($sl | Where-Object { $_ -match '^\s*diag:' })
  if ($dl.Count) { $hashLine = ($dl[-1] -replace '\s+', ' ').Trim() }
  # "painted" comes from the ENGINE's own framebuffer accounting, not from the screen.
  #
  # The screen cannot answer this question here. The app's chrome is dark and fills the
  # window, so a whole-window pixel count returned 99.68% non-white for all twelve sites
  # -- the same number for a painted page and for a blank one. It was not a wrong answer,
  # it was a question about the window asked of a number about the page.
  #
  # The diag line is the engine describing its own page: nonwhite=A/B counts framebuffer
  # pixels differing from white. A blank or unpainted page scores ~0; a painted one scores
  # above it. The threshold is deliberately low (1%) because a text-only page on white --
  # info.cern.ch, say -- is legitimately almost all white.
  #
  # Calibrated against the 2026-10-03 run, and the spread is what makes the number usable:
  # example.com 100% (its body background is #f0f0f2, so almost no pixel IS white),
  # ycombinator 99.99%, nokia-archive 99.66%, wikipedia-m 81.68%, reddit 70.55%, w3.org 43.23%,
  # lenta 33.97%, ya.ru 8.40%, habr 4.04%, info.cern.ch 1.86%, dzen.ru 0.00%.
  # The last one is the point: dzen returned rc=0 and painted nothing, which is exactly
  # the failure rc-only checking cannot see.
  if ($dl.Count -and $dl[-1] -match 'nonwhite=(\d+)/(\d+)') {
    $nw = [double]$Matches[1]; $tot = [double]$Matches[2]
    if ($tot -gt 0) { $engNonwhite = [math]::Round(100.0 * $nw / $tot, 2) }
  }
  $loadOk = ($rc -eq 0)
  # A failed load publishes no page number, and the number it would publish is not its own.
  #
  # On the failure path the loader writes its stage block -- rc, then diag -- BEFORE it renders
  # the error HTML, and a failed load has not replaced the engine's document, so that diag
  # describes the PREVIOUS page's framebuffer. The run showed it plainly: bad-domain reported
  # eng_nonwhite=31.59, byte for byte dzen's reading one row above. Reporting it would be a
  # measurement that lies, so it is reported as n/a; the verdict for this row is the reported
  # failure itself, which is what MVP item 8 asks for.
  if (-not $loadOk) { $engNonwhite = $null }
  # The stage file keeps only the LAST after-load, so a stale rc from the previous site
  # would otherwise be read as this site's result. Compare the URL to be sure.
  $urlMatched = ($stageUrl -ne '') -and (($s.Url -replace '/$','') -eq ($stageUrl -replace '/$',''))
  if (-not $urlMatched) { $loadOk = $false }
  $painted = ($engNonwhite -ne $null) -and ($engNonwhite -ge 1.0)
  $expectFail = ($s.Name -eq '12-bad-domain')
  # For the unreachable domain a blank page IS the pass. For everything else,
  # rc=0 with no pixels is the exact failure this harness exists to catch.
  $verdict = if ($expectFail) { if ($loadOk -and $painted) { 'FAIL' } else { 'PASS' } }
             else { if ($loadOk -and $painted) { 'PASS' } else { 'FAIL' } }

  $rows += [pscustomobject]@{
    site = $s.Name; url = $s.Url; note = $s.Note; mode = $Mode
    rc = $rc; eng_nonwhite = $engNonwhite; screen_nonwhite = $nonwhite; painted = $painted
    swipe_ms = $swipeLine; alive = $alive; verdict = $verdict
    screenshot = (Split-Path $shot -Leaf)
  }
  Write-Host ("    rc={0} eng_nonwhite={1}% swipe={2}ms alive={3} => {4}" -f $rc, $engNonwhite, $swipeLine, $alive, $verdict) `
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
$rows | Format-Table site, mode, rc, eng_nonwhite, screen_nonwhite, swipe_ms, verdict -AutoSize | Out-String -Width 200 | Write-Host
$pass = @($rows | Where-Object { $_.verdict -eq 'PASS' }).Count
Write-Host ("passed {0} / {1} in {2} mode;  report in {3}" -f $pass, $rows.Count, $Mode, $OutDir)

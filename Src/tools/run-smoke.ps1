# Thirteen-site smoke run for Apotheosis, driven through the autocmd controller.
#
# MVP requires a per-site record of: URL, load result, painted pixels/content,
# scroll result, back result, crash or hang marker -- and it says explicitly that
# a test which only checks rc=0 is incomplete. So for each site this captures
# five independent things, and fails the site when the first two disagree:
#
#   1. rc        -- the engine's return code for the load
#   2. nonwhite  -- the share of non-background pixels in a real screenshot,
#                   the only evidence that something was actually PAINTED
#   3. swipe_ms  -- a real touch-path drag, timed
#   4. back_ok   -- MVP requirement 5: a Back afterwards must land on the row above
#                   (from the first row that is the app's own Home), and a Forward
#                   issued straight after must come back to this site. `back_case` says
#                   which of three outcomes it was: `prev` (the row above), `same-url`
#                   (the page pushed an entry of its own URL, so an engine Back moves
#                   between two identical rows), or `other` (a real mismatch)
#   5. alive     -- is the process still running afterwards
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

# --- Reading the app's answers ------------------------------------------------------------------
#
# Everything below talks to the running app through two files: `autocmd.txt` (one command per
# line, a batch per write) and `autocmd-log.txt` (the app's own account of what it did). The
# rules for reading the log were paid for one wrong measurement at a time.
#
# A hist line ALREADY in the log is not this verb's answer, and the first version of this reader
# treated any hist line as the completion marker -- so the `forward` poll returned the `back`
# line it found in the same file, one millisecond identical, and a whole run reported "forward
# did not return" thirteen times. Measured in the log it was plainly false: the Forward ran after
# every Back and moved the cursor back to the site.
#
# The marker must be the COMPLETE line, not its opening fields. The app appends to this file
# while it runs, so a read can catch a line flushed only as far as `fwd=0` -- enough to match a
# prefix pattern, and its `url=` field then reads as empty. That is what a run against the bench
# reported: every row `moved_to=''` while the app was moving correctly and the field had simply
# not been written yet.
$histRe = 'hist engine back=\d+ fwd=\d+ cached=-?\d+/-?\d+ host idx=-?\d+/\d+ url=\S*$'
$script:ls = $ls
$script:logFile = Join-Path $ls 'autocmd-log.txt'

function Invoke-NavVerb([string]$verb, [int]$waitMs = 10000, [switch]$SampleBefore) {
  # The `@(...)` around every read here is load-bearing, not decoration. A function's return
  # value travels the pipeline, which UNROLLS a one-element array, so a helper that returned
  # `@(...)` handed back a bare STRING whenever the log held exactly one hist line -- and a
  # string's [-1] is its LAST CHARACTER. Measured on the bench: the first sample of every row
  # came back as a single letter ('m' for ...url=about:home, '/' for the Nokia URL), because
  # that sample is taken when the log holds one hist line and every later sample is taken when
  # it holds two. `@()` forces the array back at every step.
  #
  # The answer is read from the lines APPENDED after this call, not from a match anywhere in
  # the file: the log is append-only across batches, so an older line of the same shape is a
  # different verb's answer.
  #
  # `-SampleBefore` puts the pre-verb reading in the SAME batch as the verb, as `hist`, settle,
  # verb, settle, `hist`. It was a batch of its own first, and that batch went unanswered in
  # some rows -- the reader then waited its full 45 s and filed an empty `hist_before` for a
  # site whose Back was in fact correct. Two readings in one batch cannot be separated that way:
  # a batch is one unit of work, and if it runs at all, both of its samples are written.
  $linesBefore = @(Get-Content $script:logFile -ErrorAction SilentlyContinue).Count
  $script:navHist = ''; $script:navFirst = ''; $script:navRc = $null; $script:navSkip = ''
  $batch = @()
  if ($SampleBefore) { $batch += @("hist", "wait 3000") }
  if ($waitMs -gt 0) { $batch += @($verb, "wait $waitMs", "hist") } else { $batch += @("hist") }
  $wantSamples = 2 - ($SampleBefore ? 0 : 1)
  Set-Content (Join-Path $script:ls 'autocmd.txt') -Value $batch -Encoding UTF8
  $deadline = (Get-Date).AddSeconds(45)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $new = @(Get-Content $script:logFile -ErrorAction SilentlyContinue | Select-Object -Skip $linesBefore)
    $hl = @($new | Where-Object { $_ -match $histRe })
    if ($hl.Count -lt $wantSamples) { continue }
    # The verb's own `history <verb> rc=` line belongs to this batch too. It is usually already
    # there; give it two bounded seconds more before accepting a sample without one, so a
    # missing number means the app never logged it rather than that the poll outran the write.
    #
    # A Back can also legitimately have no `rc=` at all: the app skips the engine and answers
    # from the host stack when there is no live session, or while a load or a gesture owns the
    # engine. Since 0.2.3.37 it says so -- `history back skipped reason=no-session` -- and that
    # line is kept as the row's explanation. Before that build the same state was silent, and a
    # Back that left no record was indistinguishable from a Back that never arrived.
    if ($verb -ne 'hist') {
      for ($i = 0; $i -lt 10; $i++) {
        $rc = @($new | Where-Object { $_ -match 'history (?:back|fwd) rc=(-?\d+)' })
        $sk = @($new | Where-Object { $_ -match 'history (?:back|fwd) skipped ' })
        if ($rc.Count -or $sk.Count) { break }
        Start-Sleep -Milliseconds 200
        $new = @(Get-Content $script:logFile -ErrorAction SilentlyContinue | Select-Object -Skip $linesBefore)
      }
      $rc = @($new | Where-Object { $_ -match 'history (?:back|fwd) rc=(-?\d+)' })
      if ($rc.Count) { $script:navRc = [int]([regex]::Match($rc[-1], 'rc=(-?\d+)').Groups[1].Value) }
      $sk = @($new | Where-Object { $_ -match 'history (?:back|fwd) skipped ' })
      if ($sk.Count) { $script:navSkip = ($sk[-1] -replace '\s+', ' ').Trim() }
    }
    if ($SampleBefore) { $script:navFirst = ($hl[0] -replace '\s+', ' ').Trim() }
    $script:navHist = ($hl[-1] -replace '\s+', ' ').Trim()
    break
  }
}

# `host idx=<cursor>/<lastIndex>` -- the app prints the cursor and the LAST index, so a stack
# of three reads "2". Used only to tell whether forward moved the cursor at all.
function Get-HistCursor([string]$line) {
  if ($line -match 'host idx=(\d+)/') { return [int]$Matches[1] }
  return -1
}

# Wait for the shell to answer before the first site is driven.
#
# A fresh process does not answer at once, and the first `goto` of a run is the one that pays:
# measured on the bench, the first row of a run came back rc=-16 (`kErrCurlDownload`, curl_easy_perform
# failed) with no page painted, while the same navigation issued by hand against an app started
# the same way returned rc=0, and every later row of that run loaded normally. The handshake
# below does not retry the load -- a run that retried its failures would be a run that hides
# them -- it only waits until the app is demonstrably listening before the list starts.
Invoke-NavVerb 'hist' 0
Start-Sleep -Seconds 4

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

  # Batch 3, Back -- and then Forward, to put the stack back. It has to come after the
  # screenshot: Back repaints the PREVIOUS page, so capturing after it would file one
  # site's picture under another site's name.
  #
  # This is MVP requirement 5, "back returns to the previous page", measured rather than
  # asserted. The expectation comes from the list itself: the previous row's URL.
  #
  # Forward is not decoration, and the first version of this check omitted it -- which cost
  # a full run of thirteen MISMATCHes that were the CHECK's fault, not the app's. A Back
  # onto Home leaves the cursor at Home, and the NEXT site's `goto` then truncates the
  # forward tail, which by then contains every site already visited. So the stack never grew
  # past [Home, current], and every later Back correctly answered with Home. Restoring with
  # Forward keeps the entry the site occupied, so the next `goto` appends instead of trimming
  # and the stack accumulates the whole list, which is what makes "the previous page" a real
  # expectation for every row after the first.
  #
  # `back` is read from the engine's own line and `back_to` from the `hist` verb's, because
  # they answer different questions: the engine logs `rc` and whether IT moved (rc=-99 is the
  # host-stack fallback, which is what carries Back across sessions at all), while `hist`
  # states where the app actually is. A Back can be served by the fallback and still land
  # correctly; only `hist` can say where it landed.
  # The `hist` reads and the two helpers they need are defined once above the loop -- see the
  # notes there. They were inside it first, which redefined them thirteen times and, worse, put
  # them out of reach of the startup handshake that waits for the shell to answer.

  Invoke-NavVerb 'back' 10000 -SampleBefore
  $histBefore = $script:navFirst
  $backRc = $script:navRc; $histAfterBack = $script:navHist
  $backSkip = $script:navSkip

  # The Forward is issued up to three times and the retry count is recorded, because a Forward
  # dropped by the app is indistinguishable from a Forward the app refused unless the attempt
  # count is kept. NavigateHistory returns at its first line while a load is in flight, and the
  # Back above starts a full reload (the back/forward cache is disabled process-wide), so on a
  # heavy site the first Forward can arrive before the Back's own load has finished. Retrying
  # measures "can the app come back", which is the question; one blind attempt would measure
  # "was ten seconds enough for that site".
  $backCursor = Get-HistCursor $histAfterBack
  $fwdAttempts = 0
  $histAfterForward = ''
  while ($fwdAttempts -lt 3) {
    $fwdAttempts++
    Invoke-NavVerb 'forward'
    $histAfterForward = $script:navHist
    if ((Get-HistCursor $histAfterForward) -ne $backCursor) { break }
  }

  $backTo = ''
  if ($histAfterBack -match 'url=(\S*)\s*$') { $backTo = $Matches[1] }
  # Which of the lists answered the Back, read off the engine's own return code: 0 is the
  # engine's list, anything else (measured: -99) is the host stack's fallback. The column is
  # here because the two are different claims about requirement 5 and a bare "ok" hides which
  # one was tested. In this list every row answers `host` -- a scripted `goto` builds a new
  # engine session, so the engine's list holds one item and rc is never 0. See the note in
  # Doc/Archive/Summary.md; the engine's list is exercised by the tap path, not by this list.
  #
  # `host-skipped` is a third state, not a rounding of `host`: the app answered from the host
  # stack without asking the engine at all, and said so on its own line (`history back skipped
  # reason=no-session` / `busy loading=1`). The rows whose load had failed are the ones that hit
  # it -- there is no live session to consult. `unknown` is kept separate on purpose: it means
  # no rc line AND no skip line, i.e. the app left no record, which is a defect in the app or
  # in this reader and must not be silently filed as a Back the host served.
  $backPath = if ($backRc -eq 0) { 'engine' }
              elseif ($backRc -ne $null) { 'host' }
              elseif ($backSkip -ne '') { 'host-skipped' }
              else { 'unknown' }
  # Site 01 is the only row with Home behind it; every other row must land on the row above.
  # This works only because the Forward above restored the cursor -- see the note at the top.
  $prevWant = if ($rows.Count -gt 0) { $rows[-1].url.TrimEnd('/') } else { 'about:home' }
  # Three outcomes, not two, because a Back that lands on this very URL is a legitimate one when
  # the PAGE put an entry of its own URL on the list.
  #
  # Measured on `0.2.3.37`, site 13: the engine served the Back (`rc=0`), and its list held
  # `nokia` twice -- `addItem ... n=1 cursor=0` for the load, then `addItem ... n=2 cursor=1`
  # from the document's own same-URL history push (the archive is an SPA). The engine then moved
  # from entry 2 to entry 1, which are the same URL, so `WebCoreGetUrl` returned the URL the app
  # was already showing and the app correctly left alone. Filed as a MISMATCH, that reads as a
  # broken Back; it is the engine behaving exactly like a browser, and the port's own list is
  # right. A page that pushes its own URL is the one case where "the previous page" has the same
  # address, so it gets its own name rather than a pass/fail vote.
  $backCase = if (($backTo.TrimEnd('/')) -eq $prevWant) { 'prev' }
              elseif ($backPath -eq 'engine' -and ($backTo.TrimEnd('/')) -eq ($s.Url.TrimEnd('/'))) { 'same-url' }
              else { 'other' }
  $backOk = ($backCase -ne 'other')
  $restored = ''
  if ($histAfterForward -match 'url=(\S*)\s*$') { $restored = $Matches[1] }
  $backRestored = (($restored.TrimEnd('/')) -eq ($s.Url.TrimEnd('/')))

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
    swipe_ms = $swipeLine
    # requirement 5, kept as its own column rather than folded into `verdict`. The verdict is
    # the load/paint question the list was built around; `back_ok` is a second question with a
    # different expectation per row (site 01 must NOT move, every other row must land on the
    # row above it). Folding a new expectation into the pass rule on its first run would make
    # one bad expectation look like thirteen broken sites.
    back_rc = $backRc; back_url = $backTo; back_case = $backCase; back_ok = $backOk
    back_restored = $backRestored
    back_path = $backPath; back_note = $backSkip; fwd_attempts = $fwdAttempts
    hist_before = $histBefore; hist = $histAfterBack; hist_after_fwd = $histAfterForward
    alive = $alive; verdict = $verdict
    screenshot = (Split-Path $shot -Leaf)
  }
  Write-Host ("    rc={0} eng_nonwhite={1}% swipe={2}ms alive={3} => {4}" -f $rc, $engNonwhite, $swipeLine, $alive, $verdict) `
    -ForegroundColor $(if ($verdict -eq 'PASS') { 'Green' } else { 'Yellow' })
  $backVerdict = if ($backCase -eq 'prev') { 'ok' } elseif ($backCase -eq 'same-url') { 'ok (the page pushed its own URL)' } else { 'MISMATCH' }
  if (-not $backRestored) { $backVerdict += ' (forward did not return)' }
  Write-Host ("    back via {0} rc={1} from idx={2} moved_to='{3}' expect='{4}' fwd_attempts={5} => {6}" -f $backPath, $backRc, (Get-HistCursor $histBefore), $backTo, $prevWant, $fwdAttempts, $backVerdict) `
    -ForegroundColor $(if ($backOk -and $backRestored) { 'Green' } else { 'Yellow' })
  if ($backSkip -ne '') { Write-Host ("      ($backSkip)") -ForegroundColor DarkGray }

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
$rows | Format-Table site, mode, rc, eng_nonwhite, screen_nonwhite, swipe_ms, verdict, back_path, back_case, back_ok, back_restored -AutoSize | Out-String -Width 200 | Write-Host
$backPass = @($rows | Where-Object { $_.back_ok }).Count
$restorePass = @($rows | Where-Object { $_.back_restored }).Count
$pass = @($rows | Where-Object { $_.verdict -eq 'PASS' }).Count
# `back_case` is broken out because `back_ok` folds two different successes into one number:
# a Back that landed on the row above (`prev`) and a Back that the engine served onto this very
# URL (`same-url`, a page that pushed its own URL onto the list). A bare `back 13 / 13` would
# hide which of the two each row was, and the second kind is exactly the one that used to be
# filed as a MISMATCH -- so the count that must stay 13 is `back_ok`, and the breakdown is there
# to say why it is 13.
$prevCase = @($rows | Where-Object { $_.back_case -eq 'prev' }).Count
$sameCase = @($rows | Where-Object { $_.back_case -eq 'same-url' }).Count
$otherCase = @($rows | Where-Object { $_.back_case -eq 'other' }).Count
Write-Host ("passed {0} / {1} in {2} mode;  back {3} / {4};  forward returned {5} / {4};  report in {6}" -f $pass, $rows.Count, $Mode, $backPass, $rows.Count, $restorePass, $OutDir)
Write-Host ("back breakdown: prev {0}, same-url {1}, other {2}" -f $prevCase, $sameCase, $otherCase)

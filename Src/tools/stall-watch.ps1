# stall-watch.ps1 -- load one URL and sample its diag line every few seconds, so a page that is stuck
# for a while and then recovers is visible as a time series instead of a verdict.
#
# Apotheosis 2026-09-24. Written because `stall-arms.ps1` called habr.com/ru/feed/ STALLED after a 35 s
# observation window -- and the timeout that governs the suspected mechanism is 60 s. The port's own
# loader queues per host: `serve started=208 inflight=6 pending=65 hosts=1` is six transfers in flight
# and sixty-five waiting behind them on one host, and the port advances that queue only in
# `ResourceLoader::remove()` (`PortPlatformStrategies.cpp`). Upstream's curl backend sets
# `CURLOPT_LOW_SPEED_LIMIT 1` / `CURLOPT_LOW_SPEED_TIME <defaultTimeoutInterval>` with a default of
# 60 s (`CurlContext.h:155`), so six stalled transfers die at 60 s and the queue should move.
#
# So the honest question is not "is it stalled" but "for how long, and does it end". A boolean verdict
# sampled inside the timeout window cannot tell a hard stall from a 60-second white page, and the two
# are different defects with different fixes.
#
# Usage:
#   pwsh -File Src\tools\stall-watch.ps1                                  # habr feed, 180 s of samples
#   pwsh -File Src\tools\stall-watch.ps1 -Url https://dzen.ru/ -Seconds 90
#   pwsh -File Src\tools\stall-watch.ps1 -NoRestart                       # watch the running process
param(
    [string]$Url = 'https://habr.com/ru/feed/',
    [int]$Seconds = 180,
    [int]$StepSec = 5,
    [int]$MaxSec = 90,
    [int]$SettleSec = 15,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$nav = Join-Path $local 'nav.txt'
$log = Join-Path $local 'log.txt'

function Get-RssMb {
    $p = Get-Process Harness -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { return -1 }
    return [int]($p.WorkingSet64 / 1MB)
}
function Get-PairCount {
    if (-not (Test-Path $log)) { return 0 }
    return @(Select-String -Path $log -Pattern '\[STAGE\] after-load' -ErrorAction SilentlyContinue).Count
}
function Get-LastDiag {
    param([string]$U)
    if (-not (Test-Path $log)) { return '' }
    $hit = Select-String -Path $log -Pattern 'diag: url=' -ErrorAction SilentlyContinue |
        Where-Object { $_.Line -like "*$U*" } | Select-Object -Last 1
    if (-not $hit) { return '' }
    return $hit.Line
}

if (-not $NoRestart) {
    $old = Get-Process Harness -ErrorAction SilentlyContinue
    if ($old) {
        Stop-Process -Id $old.Id -Force
        Wait-Process -Id $old.Id -Timeout 20 -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 1500
    }
    $aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
    Start-Process ("shell:AppsFolder\{0}!{1}" -f $pkg.PackageFamilyName, $aid)
    $deadline = (Get-Date).AddSeconds(45)
    $launchedAt = Get-Date
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-Process Harness -ErrorAction SilentlyContinue)) { continue }
        if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $launchedAt) { break }
    }
    if (-not (Get-Process Harness -ErrorAction SilentlyContinue)) { throw 'the app did not start within 45s' }
}
if ($SettleSec -gt 0) { Start-Sleep -Seconds $SettleSec }

$pairBase = Get-PairCount
Set-Content -Path $nav -Value $Url -Encoding ASCII
$deadline = (Get-Date).AddSeconds($MaxSec)
$loaded = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    if ((Get-PairCount) -gt $pairBase) { $loaded = $true; break }
    if ((Get-RssMb) -lt 0) { break }
}
Write-Host ("url:    {0}" -f $Url)
Write-Host ("load pair: {0}" -f $(if ($loaded) { 'arrived' } else { 'NEVER ARRIVED' }))
Write-Host ''
Write-Host '   t   rssMB  body  scripts sheets  pending  readyState  contents'
Write-Host '  ---  -----  ----  ------- ------  -------  ----------  --------'

$start = Get-Date
$rows = @()
$lastKey = ''
while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    $rss = Get-RssMb
    if ($rss -lt 0) { Write-Host '  the process died'; break }
    $diag = Get-LastDiag $Url
    $t = [int]((Get-Date) - $start).TotalSeconds
    $f = @{}
    foreach ($k in @('body', 'scripts', 'sheets', 'pending', 'contents')) {
        $f[$k] = if ($diag -match "$k=(\S+)") { $Matches[1] } else { '?' }
    }
    $f['rs'] = if ($diag -match 'rs=(\S+?)/') { $Matches[1] } else { '?' }
    Write-Host ("  {0,3}  {1,5}  {2,4}  {3,7} {4,6}  {5,7}  {6,10}  {7}" -f `
        $t, $rss, $f['body'], $f['scripts'], $f['sheets'], $f['pending'], $f['rs'], $f['contents'])
    $lastKey = "$($f['body'])/$($f['rs'])/$($f['scripts'])"
    $rows += [pscustomobject]@{ t = $t; body = $f['body']; rs = $f['rs']; pending = $f['pending'] }
    Start-Sleep -Seconds $StepSec
}

Write-Host ''
# The age of the reading this verdict rests on. `diag:` is written when a LOAD or a RESIZE completes --
# it is not a live view -- so "the diag still says body=0" is only meaningful together with WHEN that
# diag was written. Without this, a verdict of "still stalled" can rest on a line from before the wait
# began.
$lastDiag = Get-LastDiag $Url
$diagAge = ''
if ($lastDiag -match '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)') {
    $ageSec = [int]((Get-Date) - [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', $null)).TotalSeconds
    $diagAge = "last diag written ${ageSec}s ago"
} else { $diagAge = 'no diag line for this url' }
Write-Host ("  {0}" -f $diagAge)

# The port's own queue accounting, which prints on every change (`serve`), so the LAST line is a
# sound statement of what the loader believes right now -- unlike a diag, which is a past snapshot.
# This is the direct answer to "do the in-flight transfers ever retire": if `started` never advances
# and `inflight` never drops, they do not.
$trace = Join-Path $local 'port-trace.txt'
if (Test-Path $trace) {
    # 25000 lines, not 4000: on a page that loads, the bulk `+`/`-` pairs fill a short tail completely
    # and the last `serve` line -- the only statement of where the queue actually stands -- falls off
    # the end. Measured on attempt 1 of the 2026-09-24 series: a 4000-line tail found no serve line at
    # all on a successful load. -Tail reads from the end, so this costs nothing on a 34 MB file.
    $tail = Get-Content $trace -Tail 25000 -ErrorAction SilentlyContinue
    $serve = @($tail | Where-Object { $_ -match '^loader: serve' }) | Select-Object -Last 1
    $retire = @($tail | Where-Object { $_ -match '^loader: -' }) | Select-Object -Last 1
    Write-Host ("  loader now: {0}" -f $(if ($serve) { $serve } else { '(no serve line in the last 25000)' }))
    Write-Host ("  last retire in that tail: {0}" -f $(if ($retire) { $retire } else { '(none)' }))
}

Write-Host ''
$body = @($rows | Where-Object { $_.body -eq '1' })
# Refuse the verdict when there is nothing to have a verdict about.
#
# Measured 2026-09-24: with habr.com unreachable from the bench the engine reported `nav: FAIL rc=-16`
# and never created a document for the URL, so every sample read `body=?` -- no diag line existed at all
# -- and the tool still announced "no body within 45s of sampling -- the stall outlives the observation
# window". Every figure in that sentence was a question mark read from an absence, and the defect it
# invites you to hunt (a stalled resource queue) is a different one from a failed connection.
#
# The discriminator is the DIAG LINE FOR THIS URL, and finding that out cost a correction: the first
# attempt guarded on `$loaded`, the load pair -- but `[STAGE] after-load` is written on a FAILED
# navigation too (`rc=-16`), so the pair arrives and the guard passed. What distinguishes the two is
# that a failed navigation leaves the engine on the PREVIOUS document, so its diag names a different url
# and there is no diag for the one asked for.
if ([string]::IsNullOrEmpty($lastDiag)) {
    Write-Host 'VERDICT: cannot conclude -- no document was ever created for this url.'
    Write-Host '         Nothing here is evidence about the resource loader. Read the navigation outcome:'
    $verdictLine = Select-String -Path $log -Pattern '\[STAGE\] nav: (OK|FAIL)' -ErrorAction SilentlyContinue |
        Select-Object -Last 1
    if ($verdictLine) { Write-Host ("           {0}" -f $verdictLine.Line.Trim()) }
    $errLine = Select-String -Path $log -Pattern '^ERR: ' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($errLine) { Write-Host ("           {0}" -f $errLine.Line.Trim()) }
    Write-Host '         A failed navigation (a site that cannot be reached, a connect timeout) is not a'
    Write-Host '         stall: the loader counters below describe the page that was already on screen.'
} elseif ($rows.Count -eq 0) {
    Write-Host 'VERDICT: cannot conclude -- no samples were taken (did the process die at launch?).'
} elseif ($body.Count -eq 0) {
    Write-Host ("VERDICT: no body within {0}s of sampling -- the stall outlives the observation window." -f $Seconds)
    Write-Host '  Read the two lines above before believing it: they say whether the reading is fresh and'
    Write-Host '  whether the loader has moved at all. The curl low-speed timeout is 60 s by default'
    Write-Host '  (CurlContext.h m_defaultTimeoutInterval) and the port advances its per-host queue only'
    Write-Host '  when a transfer retires, so a recovery after ~60 s is the expected shape -- if the loader'
    Write-Host '  counter has not moved in that time, curl is not the thing that is stuck.'
    # Preserve the evidence: log.txt is truncated per run, so a stall that is not copied out now is
    # gone by the time the next attempt starts.
    $dir = Join-Path $local ("stall-watch\{0}" -f (Get-Date -Format 'MMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($f in @($log, (Join-Path $local 'gpuinit-steps.txt'))) {
        if (Test-Path $f) { Copy-Item $f (Join-Path $dir (Split-Path $f -Leaf)) -Force }
    }
    if (Test-Path $trace) {
        Get-Content $trace -Tail 40000 | Set-Content (Join-Path $dir 'port-trace-tail.txt') -Encoding ASCII
    }
    Write-Host ("  evidence: {0}" -f $dir)
} else {
    $first = $body[0]
    Write-Host ("VERDICT: body appeared at t={0}s (sampled every {1}s)." -f $first.t, $StepSec)
    Write-Host '  If that t is near 60 s, this is a 60-second white page, not a permanent stall -- the'
    Write-Host '  parser waits on a render-blocking resource queued behind transfers that only die when'
    Write-Host '  curl gives up on them.'
}
Write-Host ("  samples: {0}   rows with body=1: {1}" -f $rows.Count, $body.Count)

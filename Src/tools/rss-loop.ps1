# rss-loop.ps1 -- drive N navigations through LocalState\nav.txt and sample the harness's RSS after each.
#
# Apotheosis 2026-09-24. Exists because the memory question is a *slope*, not a single number: on
# 0.1.10.35 the same page repeated grew ~+100-145 MB per load while a trivial page plateaued. Measuring
# a fix needs the identical sequence before and after, with the process's own RSS read at the same point
# of each cycle.
#
# Uses the nav.txt watcher rather than a fresh launch per URL, so the process is one session and the
# growth (or its absence) is attributable to navigation rather than to startup. The watcher seeds its
# timestamp at startup, so only a rewrite fires it -- hence Set-Content every iteration, same URL
# included.
#
# Synchronisation is the `diag:` line the harness writes to log.txt after every load and resize, which
# is also the line that carries body/nonwhite/res. Waiting on it is what keeps the sample point the same
# on every iteration; a fixed Start-Sleep would drift with page weight.
param(
    [string]$Url   = 'https://dzen.ru/',
    [int]$Count    = 5,
    [int]$MaxSec   = 40,      # per load: how long to wait for the next `diag:` line
    [int]$SettleSec = 15,     # before the loop: let whatever the harness is doing finish first
    [switch]$NoWait
)
$ErrorActionPreference = 'Stop'

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$nav   = Join-Path $local 'nav.txt'
$log   = Join-Path $local 'log.txt'
# Wait rather than throw: right after an install-and-launch (the normal way this is used) log.txt does
# not exist for the first few seconds, and a hard failure there reads as "the app never ran".
$waitLog = (Get-Date).AddSeconds(45)
while (-not (Test-Path $log) -and (Get-Date) -lt $waitLog) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path $log)) {
    $running = Get-Process Harness -ErrorAction SilentlyContinue
    if ($running) { throw "Harness is running (pid $($running.Id)) but wrote no log.txt in 45s" }
    throw "no log.txt at $log and no Harness process -- did the app fail to start?"
}

function Get-DiagCount {
    if (-not (Test-Path $log)) { return 0 }
    return @(Select-String -Path $log -SimpleMatch 'diag:' -ErrorAction SilentlyContinue).Count
}
function Get-RssMb {
    $p = Get-Process Harness -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { return -1 }
    return [int]($p.WorkingSet64 / 1MB)
}

Write-Host ("url={0}  count={1}  localstate={2}" -f $Url, $Count, $local)
Write-Host ("version: {0}" -f $pkg.PackageFullName)

if (-not $NoWait -and $SettleSec -gt 0) {
    Write-Host ("settling {0}s before the loop ..." -f $SettleSec)
    Start-Sleep -Seconds $SettleSec
}

$rows  = @()
$before = Get-DiagCount
$startRss = Get-RssMb
Write-Host ("start: RSS={0} MB  diag={1}" -f $startRss, $before)

for ($i = 1; $i -le $Count; $i++) {
    # Rewrite, not append-as-a-no-op: the watcher keys on the file's timestamp.
    Set-Content -Path $nav -Value $Url -Encoding ASCII
    $deadline = (Get-Date).AddSeconds($MaxSec)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 400
        if ((Get-DiagCount) -gt $before) { $ok = $true; break }
    }
    $diag = Get-DiagCount
    $rss  = Get-RssMb
    if ($rss -lt 0) { Write-Host 'harness is gone'; break }
    if ($rss -gt 2500) { Write-Host ("ABORT: RSS={0} MB -- refusing to drive this laptop further" -f $rss); break }
    $slice = if ($ok) { '' } else { '  <-- TIMEOUT (no new diag: line)' }
    Write-Host ("  [{0}/{1}] RSS={2} MB  diag={3}{4}" -f $i, $Count, $rss, $diag, $slice)
    $rows += [pscustomobject]@{ iter = $i; rss = $rss; diag = $diag; settled = $ok }
    $before = $diag
    Start-Sleep -Milliseconds 700    # let the settle pump and any live tick finish before the next one
}

if ($rows.Count -ge 2) {
    $deltas = for ($j = 1; $j -lt $rows.Count; $j++) { $rows[$j].rss - $rows[$j-1].rss }
    $avg = [int](($deltas | Measure-Object -Average).Average)
    Write-Host ''
    Write-Host ("total: {0} -> {1} MB over {2} loads ({3} MB)" -f `
        $startRss, $rows[-1].rss, $rows.Count, ($rows[-1].rss - $startRss))
    Write-Host ("per-load deltas: {0}" -f ($deltas -join ', '))
    Write-Host ("average: {0} MB/load  (baseline 0.1.10.35 on dzen.ru: ~+100-145 MB/load)" -f $avg)
    if ($rows | Where-Object { -not $_.settled }) { Write-Host 'WARNING: at least one iteration timed out; deltas are not comparable' }
}

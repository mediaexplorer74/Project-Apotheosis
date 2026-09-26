# fresh-load.ps1 -- restart the harness, then price ONE load of each page from a FRESH process.
#
# Apotheosis 2026-09-24. `rss-loop.ps1` answers "what is the slope of one page repeated in one warm
# session"; this answers a different and, for the MVP, more urgent question: "what does a single load of
# this page cost on a process that has just started". The two are not the same number, and the difference
# is the whole finding of 2026-09-24 -- the slope only exists below a high watermark, so a warm-process
# reading says nothing about what one page does to a phone that has just launched the browser.
#
# Why it matters on the target: a Lumia 950 has 3 GB of RAM in total and an App Container budget far
# under the 12.5 GB the bench reports. A page that costs 1.2 GB from a fresh process is a different kind
# of problem there than it is here -- and the device's failure mode for it is a silent reap with no dump
# (memory `device-diagnostics-that-survive`).
#
# Synchronisation is the harness's own `[STAGE] before-load` / `[STAGE] after-load` pair, not a `diag:`
# line: the harness writes `diag:` after resizes as well as loads, so "the next diag" is not "the load
# finished" (load-time.ps1 measured 1941, 8373, 214, 11732 ms for the same page because of exactly that).
#
# Usage:
#   pwsh -File Src\tools\fresh-load.ps1                                   # the default set, fresh process
#   pwsh -File Src\tools\fresh-load.ps1 -Url https://habr.com/ru/feed/
#   pwsh -File Src\tools\fresh-load.ps1 -NoRestart                        # measure the running process
#
# WITHDRAWN READING, same day: the first version sampled on the load pair alone and reported
# `habr.com/ru/feed/ +37 MB` with the GPU probe disarmed. The diag at that moment read
# `body=0 nonwhite=0/710656 pending=58 rs=L` -- the page had not been rendered, the process was still
# climbing (168 MB sampled, 850 MB a minute later, then a slow fall to 822), and the number was ~20x
# smaller than the same page's GPU-on reading. The sync point, not the page, produced it. Hence the
# `[rendered]` / `[NOT RENDERED]` state printed beside every sample: an arm whose sample point is not
# the same is not an arm.
param(
    [string[]]$Url = @('https://habr.com/ru/feed/', 'https://news.ycombinator.com/', 'https://dzen.ru/', 'https://example.com/'),
    [int]$MaxSec = 60,
    [int]$SettleSec = 20,       # after launch: startup jobs must finish before the baseline is read
    [int]$RenderSec = 30,       # after the load pair: wait for body=1 pending=0 before sampling
    [int]$AfterSec = 4,         # after the render point: let the settle loop quiesce before sampling
    [int]$LimitMb = 2500,       # abort rather than drive this laptop into swap
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

# The last `diag:` line that mentions this url, so the sample point can be stated instead of assumed.
# This exists because the first version of this script sampled on the load pair alone and produced a
# `+37 MB` reading for habr.com/ru/feed/ whose diag said `body=0 nonwhite=0/710656 pending=58 rs=L` --
# the page had not been rendered at all, the process was still climbing (168 MB sampled, 850 MB a
# minute later), and the number was 20x smaller than the same page's GPU-on reading. A sample point
# that differs between arms is not a comparison; printing the diag makes the difference visible.
function Get-LastDiag {
    param([string]$Url)
    if (-not (Test-Path $log)) { return '' }
    $hit = Select-String -Path $log -Pattern 'diag: url=' -ErrorAction SilentlyContinue |
        Where-Object { $_.Line -like "*$Url*" } | Select-Object -Last 1
    if (-not $hit) { return '' }
    return $hit.Line
}
function Get-Rendered {
    param([string]$Line)
    # `body=1` and a readyState past Loading, NOT `pending=0`: every heavy SPA on this bench sits at
    # `pending=117` forever (subresources that never retire are the normal state of these pages), so a
    # `pending=0` test never fires and reports "NOT RENDERED" about a page that is fully Interactive.
    # The state that matters is "was a body built at all": `body=0 rs=L` is the stall, `body=1 rs=I` is
    # a page the user can see.
    return ($Line -match 'body=1') -and ($Line -notmatch 'rs=L/')
}

Write-Host ("version: {0}" -f $pkg.PackageFullName)

if (-not $NoRestart) {
    $old = Get-Process Harness -ErrorAction SilentlyContinue
    if ($old) {
        Write-Host ("restart: stopping pid {0} (rss {1} MB)" -f $old.Id, (Get-RssMb))
        Stop-Process -Id $old.Id -Force
        # Wait for the OLD process to be gone, not just signalled: measured 2026-09-24, relaunching in
        # the same instant left the new activation dropped and the run died with "Harness is not
        # running" 20 s later, which reads as a startup crash and is not one.
        Wait-Process -Id $old.Id -Timeout 20 -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 1500
    }
    $aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
    Start-Process ("shell:AppsFolder\{0}!{1}" -f $pkg.PackageFamilyName, $aid)
    Write-Host 'restart: launched, waiting for the process and a fresh log.txt ...'
    $deadline = (Get-Date).AddSeconds(45)
    $launchedAt = Get-Date
    # Both conditions, in this order. Waiting on log.txt alone is wrong on every run after the first:
    # the file is left over from the previous process, so the wait returns instantly and the baseline
    # sample happens before the app exists.
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-Process Harness -ErrorAction SilentlyContinue)) { continue }
        if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $launchedAt) { break }
    }
    if (-not (Get-Process Harness -ErrorAction SilentlyContinue)) { throw 'the app did not start within 45s' }
    if (-not ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $launchedAt)) {
        throw 'the app is running but has not written a fresh log.txt'
    }
}
if ($SettleSec -gt 0) { Write-Host ("settling {0}s ..." -f $SettleSec); Start-Sleep -Seconds $SettleSec }

$baseline = Get-RssMb
if ($baseline -lt 0) { throw 'Harness is not running' }
Write-Host ''
Write-Host ("fresh baseline: {0} MB" -f $baseline)
Write-Host ''

$prev = $baseline
foreach ($u in $Url) {
    $pairBase = Get-PairCount
    Set-Content -Path $nav -Value $u -Encoding ASCII
    $deadline = (Get-Date).AddSeconds($MaxSec)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 400
        if ((Get-PairCount) -gt $pairBase) { $ok = $true; break }
    }
    if (-not $ok) { Write-Host ("  {0,-40} TIMEOUT -- no load pair in {1}s" -f $u, $MaxSec); break }
    # The load pair is the fetch, not the render: wait for `body=1 pending=0` before sampling, and say so
    # when it never arrives rather than timing the sample off a page that is still parsing.
    $renderDeadline = (Get-Date).AddSeconds($RenderSec)
    $diag = ''
    while ((Get-Date) -lt $renderDeadline) {
        Start-Sleep -Milliseconds 500
        $diag = Get-LastDiag $u
        if (Get-Rendered $diag) { break }
    }
    if ($AfterSec -gt 0) { Start-Sleep -Seconds $AfterSec }
    $rss = Get-RssMb
    if ($rss -lt 0) { Write-Host ("  {0,-40} THE PROCESS DIED" -f $u); break }
    $state = if (-not $diag) { 'no diag line' }
             elseif ($diag -match 'body=0') { 'NO BODY -- the parse never completed' }
             elseif (Get-Rendered $diag) { 'rendered' }
             else { 'body built, still Loading' }
    Write-Host ("  {0,-40} RSS={1,5} MB   delta={2,5} MB   [{3}]" -f $u, $rss, ($rss - $prev), $state)
    # The three fields that decide whether the two arms are comparable at all.
    if ($diag -match 'contents=(\S+)')    { Write-Host ("      contents={0}" -f $Matches[1]) }
    if ($diag -match 'body=(\d+)')        { Write-Host ("      body={0}" -f $Matches[1]) }
    if ($diag -match 'pending=(\d+)')     { Write-Host ("      pending={0}" -f $Matches[1]) }
    if ($diag -match 'nonwhite=(\S+)')    { Write-Host ("      nonwhite={0}" -f $Matches[1]) }
    if ($diag -match 'rs=(\S+?)/')        { Write-Host ("      readyState={0}" -f $Matches[1]) }
    $prev = $rss
    if ($rss -gt $LimitMb) { Write-Host ("ABORT: RSS={0} MB > limit {1} -- stopping" -f $rss, $LimitMb); break }
}

Write-Host ''
Write-Host 'Read the deltas as one load each on a process that had just started. This measures RSS only,'
Write-Host 'so it does not separate the engine from the harness; for that, use the diag line in log.txt.'

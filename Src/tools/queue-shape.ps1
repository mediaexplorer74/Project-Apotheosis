# queue-shape.ps1 -- rank candidate pages by how closely their loader queue matches the shape that
# item 0n/0p needs: ONE host at its in-flight cap with work still waiting behind it.
#
# Apotheosis 2026-09-24. Exists because the reproduer for 0n/0p -- `habr.com/ru/feed/`, whose six stuck
# transfers starve a render-blocking stylesheet -- became unreachable from the bench, and a claim
# without a page to make it on is not a measurement. Rather than guess at a substitute by reputation,
# this reads the port's OWN counters.
#
# The port mirrors every `loader: serve` line into `gpuinit-steps.txt` through `portLoaderTraceBoth`
# (the channel that has survived every crash here), and that line carries the whole picture:
#
#   loader: serve started=N inflight=N pending=N hosts=N
#
# `inflight >= 6` is `maxRequestsInFlightPerHost` (PortPlatformStrategies.cpp), i.e. some host is at its
# cap. `pending > 0` beside it means work is queued behind that cap -- and if `hosts=1`, the queue and
# the cap are on the SAME host, which is the 0n shape exactly. A page can reach `pending` far higher
# with many hosts and never reproduce a stall, so `minHosts` is reported beside the peaks and the two
# must be read together.
#
# Usage:
#   pwsh -File Src\tools\queue-shape.ps1
#   pwsh -File Src\tools\queue-shape.ps1 -Urls 'https://lenta.ru/','https://www.rbc.ru/'
#   pwsh -File Src\tools\queue-shape.ps1 -Seconds 30
param(
    [string[]]$Urls = @('https://lenta.ru/', 'https://www.rbc.ru/', 'https://dzen.ru/',
                        'https://habr.com/ru/feed/'),
    [int]$Seconds = 18
)
$ErrorActionPreference = 'Stop'

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$nav = Join-Path $local 'nav.txt'
$mark = Join-Path $local 'gpuinit-steps.txt'
if (-not (Test-Path $mark)) { throw "no $mark -- the marker channel is the one this tool reads" }
if (-not (Get-Process Harness -ErrorAction SilentlyContinue)) {
    throw 'Harness is not running -- start it, this tool drives a live session'
}

Write-Host ("{0,-30} {1,6} {2,13} {3,12} {4,9} {5,15} {6,17} {7,18}" -f `
    'url', 'lines', 'peakInflight', 'peakPending', 'minHosts', 'atCap+queued', 'singleHostStuck', 'its peak pending')
Write-Host ('-' * 96)

foreach ($u in $Urls) {
    # Scope to THIS navigation: the file is append-only, so an offset is the honest way to separate
    # one page's traffic from the last one's residue -- the trap that once made 35 stale `+1/-0` pairs
    # look like a live leak from a page that was not even loading.
    $off = (Get-Item $mark).Length
    Set-Content -Path $nav -Value $u -Encoding ASCII
    Start-Sleep -Seconds $Seconds

    $fs = [System.IO.File]::Open($mark, 'Open', 'Read', 'ReadWrite')
    $fs.Seek($off, 'Begin') | Out-Null
    $sr = New-Object System.IO.StreamReader($fs)
    $txt = $sr.ReadToEnd()
    $sr.Close(); $fs.Close()

    $serves = @($txt -split "`n" | Where-Object { $_ -match '^loader: serve' })
    if ($serves.Count -eq 0) {
        # Never report a zero read from an empty input as a finding: the port's own tracer has been
        # silent before (.99/.100), and the navigation may simply have failed.
        Write-Host ("{0,-30} {1,6}  no serve traffic -- read the navigation outcome, not this row" -f $u, 0)        continue
    }

    $inflight = 0; $pending = 0; $minHosts = [int]::MaxValue; $atCap = 0
    $singleHostStuck = 0; $singleHostPending = 0
    foreach ($line in $serves) {
        $i = if ($line -match 'inflight=(\d+)') { [int]$Matches[1] } else { -1 }
        $p = if ($line -match 'pending=(\d+)')  { [int]$Matches[1] } else { -1 }
        $h = if ($line -match 'hosts=(\d+)')    { [int]$Matches[1] } else { -1 }
        if ($i -gt $inflight) { $inflight = $i }
        if ($p -gt $pending)  { $pending = $p }
        if ($h -gt 0 -and $h -lt $minHosts) { $minHosts = $h }
        if ($i -ge 6 -and $p -gt 0) { $atCap++ }
        # The 0n shape needs all three on ONE line. Taking `peakInflight >= 6` from one line and
        # `minHosts == 1` from another is the "two numbers from two observations" mistake that this
        # whole project has been bitten by -- a page that starts one host at a time satisfies both
        # and starves nothing.
        if ($h -eq 1 -and $i -ge 6 -and $p -gt 0) {
            $singleHostStuck++
            if ($p -gt $singleHostPending) { $singleHostPending = $p }
        }
    }
    if ($minHosts -eq [int]::MaxValue) { $minHosts = -1 }

    Write-Host ("{0,-30} {1,6} {2,13} {3,12} {4,9} {5,15} {6,17} {7,18}" -f `
        $u, $serves.Count, $inflight, $pending, $minHosts, $atCap, $singleHostStuck, $singleHostPending)
}

Write-Host ''
Write-Host 'A reproduer for 0n/0p is a row with singleHostStuck > 0 -- ONE host, at its cap, with work'
Write-Host 'queued behind it, all read off the same `loader: serve` line. `atCap+queued` counts the same'
Write-Host 'condition across any number of hosts and is kept only to show how much the loose reading'
Write-Host 'overstates it; `minHosts` is a minimum over the whole window and proves nothing on its own.'

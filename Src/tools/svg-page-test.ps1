# svg-page-test.ps1 -- load a list of file:/// urls in order and report the live-Page map after each.
#
# Apotheosis 2026-09-24. Companion to make-svg-test-pages.ps1. The hypothesis under test: each external
# SVG image creates a hidden whole Page (SVGImage::m_page) that survives navigation. If it holds, the
# anonymous-Page count read from `SL: live pages` tracks the SVG count of the page just loaded, and a
# return to the zero-SVG page does not bring it back down. If it does not hold, the count is flat across
# all of them and the hypothesis is dead.
#
# Synchronisation is the `diag:` line in log.txt, same as rss-loop.ps1 -- a fixed sleep would drift.
param(
    [string[]]$Urls = @('t0.html', 't1.html', 't10.html', 't0.html'),
    [int]$MaxSec = 30,
    [int]$SettleSec = 5
)
$ErrorActionPreference = 'Stop'
# Called from bash, a `-Urls a,b,c` arrives as one string, not an array -- split here so both forms work.
$UrlList = @($Urls | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$nav   = Join-Path $local 'nav.txt'
$log   = Join-Path $local 'log.txt'
$steps = Join-Path $local 'gpuinit-steps.txt'
$po    = (Join-Path $local 'po') -replace '\\', '/'

function Get-DiagCount {
    if (-not (Test-Path $log)) { return 0 }
    @(Select-String -Path $log -SimpleMatch 'diag:' -ErrorAction SilentlyContinue).Count
}
function Get-PageMaps {
    if (-not (Test-Path $steps)) { return @() }
    @(Select-String -Path $steps -SimpleMatch 'SL: live pages' -ErrorAction SilentlyContinue |
        Select-Object -Last 3 | ForEach-Object { $_.Line })
}
# 2026-09-24: the Document probe. `SL: docs <when> total=N` is the count; the `SL: doc` url lines that
# follow it are what say whether a retained document is a whole frame with a real url (a subframe) or
# blank/empty (a parser-made or SVG-image document). Printed as a pair on purpose: a count without the
# urls cannot distinguish those, and they lead to different defects.
function Get-DocTotals {
    if (-not (Test-Path $steps)) { return @() }
    @(Select-String -Path $steps -SimpleMatch 'SL: docs' -ErrorAction SilentlyContinue |
        Select-Object -Last 2 | ForEach-Object { $_.Line })
}
function Get-DocUrls {
    if (-not (Test-Path $steps)) { return @() }
    @(Select-String -Path $steps -SimpleMatch 'SL: doc ' -ErrorAction SilentlyContinue |
        Select-Object -Last 8 | ForEach-Object { $_.Line })
}
function Get-RssMb {
    $p = Get-Process Harness -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { return -1 }
    [int]($p.WorkingSet64 / 1MB)
}

$wait = (Get-Date).AddSeconds(45)
while (-not (Test-Path $log) -and (Get-Date) -lt $wait) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path $log)) { throw "no log.txt at $log" }

Write-Host ("package: {0}" -f $pkg.PackageFullName)
if ($SettleSec -gt 0) { Write-Host "settling ${SettleSec}s ..."; Start-Sleep -Seconds $SettleSec }

$before = Get-DiagCount
foreach ($u in $UrlList) {
    $url = if ($u -match '^[a-z]+:') { $u } else { "file:///$po/$u" }
    Set-Content -Path $nav -Value $url -Encoding ASCII
    $deadline = (Get-Date).AddSeconds($MaxSec)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 400
        if ((Get-DiagCount) -gt $before) { $ok = $true; break }
    }
    $before = Get-DiagCount
    Write-Host ''
    Write-Host ("=== {0}   RSS={1} MB{2}" -f $u, (Get-RssMb), $(if ($ok) { '' } else { '  <-- TIMEOUT' }))
    Get-PageMaps | ForEach-Object { Write-Host "    $_" }
    Get-DocTotals | ForEach-Object { Write-Host "    $_" }
    Get-DocUrls | ForEach-Object { Write-Host "      $_" }
    Start-Sleep -Milliseconds 800
}

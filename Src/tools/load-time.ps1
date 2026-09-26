# load-time.ps1 -- price a navigation: how long the engine's load takes, read from the harness's own
# `[STAGE] before-load` / `[STAGE] after-load` markers in log.txt.
#
# Apotheosis 2026-09-24. Exists because a per-navigation release lever is only promotable once its MEMORY
# effect and its TIME cost are both known -- a lever that halves the slope but doubles the load is the wrong
# trade on a Lumia. Read the two warnings below before trusting a number out of it: the first version of the
# script was invalid, and the one published result it produced was withdrawn for a different reason.
#
# WITHDRAWN RESULT, 2026-09-24 (.49/.50): this script's first real output -- dzen.ru, `8636 ms` disarmed
# against `9281 ms` armed, i.e. "+7 % for the decoded lever" -- does not survive a second run of the same
# arm, which gave a LOWER median (8096 ms) than the disarmed arm. Within-arm spread exceeds between-arm
# difference at this sample count, so the script was asked a question it cannot answer here. The numbers
# were withdrawn with the lever itself (see PLAN.md item 0j step 2d and the comment on
# `apoReleaseResourceCache()`); what remains is the instrument, which is still the right one -- just not at
# n=4 per arm on a page whose load time varies by seconds.
#
# The first version of this script polled for the next `diag:` line instead, and it was WRONG in a way worth
# recording: the harness writes `diag:` after every load AND after every resize, so the next `diag:` is not
# "the load finished" -- it can be a resize. Measured evidence: five samples read 1941, 8373, 214, 11732,
# 11481 ms for the same page and the same configuration, and the 214 ms one is simply a resize diag that
# arrived 214 ms after the nav write. No median of five noisy samples rescued that; the synchronisation
# itself was answering a different question than the one being asked.
#
# `before-load` and `after-load` are written by the harness around the engine's own load job and carry
# millisecond timestamps, so the duration is exact and needs no polling at all: drive the navigations, then
# read the pairs back. `after-load` carries rc= and compositing=, so a failed load can be excluded.
#
# The armed/disarmed state of the file switches is PRINTED, never inferred: the log cannot tell which arm
# produced a run, and a comparison across arms whose switches are unknown is how a "survives this" claim
# gets made about a configuration that was never running.
param(
    [string]$Url = 'https://dzen.ru/',
    [int]$Count = 5,
    [int]$MaxSec = 60,
    [int]$SettleSec = 20,
    [string]$Label = '',
    [switch]$NoDrive        # parse the log only, do not write nav.txt
)
$ErrorActionPreference = 'Stop'

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$nav = Join-Path $local 'nav.txt'
$log = Join-Path $local 'log.txt'

$cacheRelease = Test-Path (Join-Path $local 'cacherelease.txt')
Write-Host ("switch state: cacherelease.txt={0}  (absence = default: destroyDecodedDataForAllImages + evictResources both OFF)" -f $cacheRelease)
if ($Label) { Write-Host "arm: $Label" }

function Get-LoadPairs {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return @() }
    $stages = Select-String -Path $Path -Pattern '\[STAGE\] (before-load|after-load)' |
        ForEach-Object { $_.Line }
    $out = @()
    $start = $null
    foreach ($line in $stages) {
        if ($line.Length -lt 23) { continue }
        $ts = [datetime]::ParseExact($line.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff', $null)
        if ($line -match 'before-load') { $start = $ts; continue }
        if ($start -and $ts -gt $start) {
            $rc = if ($line -match 'rc=(-?\d+)') { [int]$Matches[1] } else { -99 }
            $out += [pscustomobject]@{ ms = [int]($ts - $start).TotalMilliseconds; rc = $rc }
            $start = $null
        }
    }
    return $out
}

$before = @(Get-LoadPairs $log).Count

if (-not $NoDrive) {
    $waitLog = (Get-Date).AddSeconds(45)
    while (-not (Test-Path $log) -and (Get-Date) -lt $waitLog) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $log)) { throw "no log.txt at $log -- is the app running?" }
    if ($SettleSec -gt 0) { Start-Sleep -Seconds $SettleSec }
    for ($i = 1; $i -le $Count; $i++) {
        Set-Content -Path $nav -Value $Url -Encoding ASCII
        # Wait for one more PAIR to appear, not for a diag: the pair is the load's own span.
        $deadline = (Get-Date).AddSeconds($MaxSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 300
            if (@(Get-LoadPairs $log).Count -gt $before) { break }
        }
        $before = @(Get-LoadPairs $log).Count
        Write-Host ("  [{0}/{1}] pairs so far: {2}" -f $i, $Count, $before)
        Start-Sleep -Milliseconds 700
    }
}

$all = @(Get-LoadPairs $log)
$ok = @($all | Where-Object { $_.rc -eq 0 })
if ($ok.Count -eq 0) { Write-Host 'no successful load pairs in the log'; exit }

$times = @($ok | ForEach-Object { $_.ms })
$sorted = $times | Sort-Object
$median = $sorted[[int]([math]::Floor($sorted.Count / 2))]
Write-Host ''
Write-Host ("arm={0}  successful loads={1}  median={2} ms  min={3}  max={4}" -f `
    $Label, $times.Count, $median, $sorted[0], $sorted[-1])
Write-Host ("all (ms): {0}" -f ($times -join ', '))
Write-Host 'The whole log is parsed, so run each arm in its own session or slice the list by position.'

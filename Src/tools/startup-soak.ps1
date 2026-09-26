# ============================================================================
# startup-soak.ps1 -- launch the harness N times and classify how each launch ended.
#
# Written 2026-09-24 for the intermittent startup death, which was a RACE and could not be measured
# by launching once:
#
#   mem-level UP: mem 35216/12497952 KB level=low     <- handler fires AT REGISTRATION (UI thread)
#   WebEngine: loop ready, waiting for jobs           <- 62 ms LATER
#   VEH: code=0xC0000005 tid=8312 faultaddr=0x8       <- engine thread, inside the first job
#
# The first job was `mem-release`, so `WebCoreReleaseMemory` -- the only ABI entry point that ran
# WebCore without first initialising the process -- reached `MemoryPressureHandler`'s constructor with
# `s_mainRunLoop` still null. The platform reports its first reading as an upward crossing only
# SOMETIMES, which is why the death appeared in roughly 3 launches in 10 and why a single successful
# launch proves nothing. This tool exists so the next such claim is a count, not an anecdote.
#
# What a launch is classified as:
#   alive  -- still running, no 0xC0000005 in its own log
#   AV     -- `VEH: code=0xC0000005` present (a real access violation, whichever thread)
#   died   -- the process is gone with no AV line (the silent-death shape: quota reap, fast-fail)
#
# Traps built in:
#   * `CRASHVERDICT: CRASHED` is written by the NEW process about the PREVIOUS one, so its presence in
#     the log is not this run's outcome. Only a 0xC0000005 line and the process list are read.
#   * The crash lands ~3 ms after `loop ready`, so the settle wait is not optional: sampling the
#     moment the ready line appears would classify every run alive.
#   * log.txt is truncated per run, so each run's file holds only that run -- but the truncation is
#     done by the app itself, so the previous file is read only after the new one exists.
#
# Usage:
#   pwsh -File Src\tools\startup-soak.ps1                 # 10 launches, no navigation
#   pwsh -File Src\tools\startup-soak.ps1 -Runs 20
#   pwsh -File Src\tools\startup-soak.ps1 -Url https://example.com   # navigate after each launch
# ============================================================================
[CmdletBinding()]
param(
    [int]$Runs = 10,
    [string]$Url = '',
    [int]$ReadyWaitSec = 12,
    [int]$SettleSec = 3
)
$ErrorActionPreference = 'Stop'

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$log = Join-Path $local 'log.txt'
$nav = Join-Path $local 'nav.txt'
$aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id

Write-Host ("startup-soak: {0} launches of {1}" -f $Runs, $pkg.PackageFullName)
Write-Host ("  version: {0}" -f ($pkg.PackageFullName -replace '.*_([0-9.]+)_.*', '$1'))
if ($Url) { Write-Host ("  navigating to {0} after each launch" -f $Url) }
Write-Host ''
Write-Host '  #   verdict  memUP  release  armed  ready  note'
Write-Host '  --  -------  -----  -------  -----  -----  ----'

$results = @()
for ($i = 1; $i -le $Runs; $i++) {
    $old = Get-Process Harness -ErrorAction SilentlyContinue
    if ($old) {
        Stop-Process -Id $old.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $old.Id -Timeout 20 -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 900
    }
    # The previous log is about to be truncated by the app; remember where it ended so a stale line
    # cannot be mistaken for this run's.
    $prevLen = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

    Start-Process ("shell:AppsFolder\{0}!{1}" -f $pkg.PackageFamilyName, $aid)

    $ready = $false
    $deadline = (Get-Date).AddSeconds($ReadyWaitSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        if (-not (Get-Process Harness -ErrorAction SilentlyContinue)) { break }
        if (-not (Test-Path $log)) { continue }
        # A shorter file than the previous run means the app truncated it -- this run's data starts here.
        if ((Get-Item $log).Length -lt $prevLen) { $prevLen = 0 }
        $txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($txt -and $txt -match 'WebEngine: loop ready') { $ready = $true; break }
    }
    if ($Url -and $ready) { Set-Content -Path $nav -Value $Url -Encoding ASCII }
    Start-Sleep -Seconds $SettleSec

    $alive = [bool](Get-Process Harness -ErrorAction SilentlyContinue)
    $txt = if (Test-Path $log) { Get-Content $log -Raw -ErrorAction SilentlyContinue } else { '' }
    $av = ($txt -match 'VEH: code=0xC0000005')
    $memUp = ([regex]::Matches($txt, 'mem-level UP')).Count
    $armedPost = ([regex]::Matches($txt, 'mem-release-race: posting')).Count
    # `mem-release: posting` and not `mem-release`: the arming probe logs `mem-release-race: checked,
    # armed=0` on every launch, and a pattern as loose as `mem-release` counts the probe's own line.
    # Measured on the first run of this tool: it reported one release per launch on a build where the
    # platform event never fired at all -- a counter satisfied by the instrument rather than by the
    # thing it measures.
    $memRel = ([regex]::Matches($txt, 'mem-release: posting')).Count

    if ($av) { $verdict = 'AV' }
    elseif (-not $alive) { $verdict = 'died' }
    else { $verdict = 'alive' }

    $note = ''
    if ($av) {
        # Name the thread and the faulting address: 0x8 is the MemoryPressureHandler signature, and a
        # different faultaddr means a different defect that only shares the timing.
        if ($txt -match 'VEH: code=0xC0000005[^\r\n]*') { $note = $Matches[0] -replace '\s+', ' ' }
    } elseif (-not $ready) { $note = 'never reached loop ready' }
    Write-Host ("  {0,2}  {1,-7}  {2,5}  {3,7}  {4,5}  {5,5}  {6}" -f $i, $verdict, $memUp, $memRel, $armedPost, $ready, $note)

    $results += [pscustomobject]@{ run = $i; verdict = $verdict; memUp = $memUp; memRelease = $memRel; armed = $armedPost; ready = $ready }
}

Write-Host ''
$byVerdict = $results | Group-Object verdict | Sort-Object Count -Descending
foreach ($g in $byVerdict) { Write-Host ("  {0,-7} {1,3} / {2}" -f $g.Name, $g.Count, $Runs) }
$trig = @($results | Where-Object { $_.memUp -gt 0 }).Count
$armed = @($results | Where-Object { $_.armed -gt 0 }).Count
Write-Host ("  launches where the memory event fired  : {0} / {1}" -f $trig, $Runs)
Write-Host ("  launches where the race was armed by hand: {0} / {1}   (LocalState\mem-release-race.txt)" -f $armed, $Runs)
if ($trig -eq 0 -and $armed -eq 0) {
    Write-Host '  NOTE: nothing posted a release at startup, so the race was NOT exercised. The platform' -ForegroundColor Yellow
    Write-Host '        reports the startup crossing only sometimes (3 launches in ~10 in the 0.1.10.51' -ForegroundColor Yellow
    Write-Host '        series, then 20 consecutive launches with no event at all). Create the arm file to' -ForegroundColor Yellow
    Write-Host '        post the job 45 ms before `loop ready` -- the position the dying launches had.' -ForegroundColor Yellow
}
$av = @($results | Where-Object { $_.verdict -eq 'AV' }).Count
if ($av -eq 0) { Write-Host '  verdict: no access violation in any launch.' -ForegroundColor Green }
else { Write-Host ("  verdict: {0} of {1} launches died with a real AV." -f $av, $Runs) -ForegroundColor Red }

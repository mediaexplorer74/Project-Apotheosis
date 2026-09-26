# ============================================================================
# arm-status.ps1 -- one look at whether the ARM32 bring-up is progressing, stalled,
# or waiting, without having to read four logs by hand.
#
# Why the "alive?" part matters more than the progress part. On the night of
# 2026-08-17 the chain sat for seven and a half hours on a download that had died
# silently: vcpkg had used six seconds of CPU, the laptop was cold, and from the
# outside it looked exactly like a long build. A cold machine during a build step is
# the symptom, so this script reports CPU time and how long ago each log was written,
# and says plainly which of the three states it thinks you are in.
#
# Usage:
#   pwsh -File Src\tools\arm-status.ps1
#   pwsh -File Src\tools\arm-status.ps1 -Watch        # refresh every 30 s
# ============================================================================
[CmdletBinding()]
param([switch]$Watch, [int]$IntervalSeconds = 30)

$ErrorActionPreference = 'Continue'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$LogDir = Join-Path $PSScriptRoot 'arm-bootstrap-logs'

function Show-Status {
    Write-Host ''
    Write-Host ("=== ARM32 bring-up status  {0} ===" -f (Get-Date -Format 'HH:mm:ss')) -ForegroundColor Cyan

    # --- where the chain thinks it is ---
    $summary = Join-Path $LogDir 'summary.txt'
    if (Test-Path $summary) {
        Write-Host 'steps' -ForegroundColor Cyan
        Get-Content $summary -Tail 200 |
            Where-Object { $_ -match 'step .* --|ok in|FAILED|stalled|attempt|skipped' } |
            Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host '  no summary.txt yet -- the bootstrap has not been run' -ForegroundColor DarkYellow
    }

    $verdict = Join-Path $LogDir 'verdict.txt'
    if (Test-Path $verdict) {
        $v = Get-Content $verdict -TotalCount 4
        $colour = if ($v -match 'SUCCESS|COMPLETE') { 'Green' } else { 'Red' }
        Write-Host 'last verdict' -ForegroundColor Cyan
        $v | ForEach-Object { Write-Host "  $_" -ForegroundColor $colour }
    }

    # --- dependencies actually installed ---
    $share = 'C:\vcpkg\installed\arm-uwp\share'
    if (Test-Path $share) {
        # doc/man are not ports; png and unofficial-brotli are aliases of ports already listed.
        $ports = @(Get-ChildItem $share -Directory | Where-Object { $_.Name -notin @('doc', 'man') } | Select-Object -ExpandProperty Name)
        Write-Host ("vcpkg arm-uwp: {0} entries" -f $ports.Count) -ForegroundColor Cyan
        Write-Host ("  " + ($ports -join ' '))
    } else {
        Write-Host 'vcpkg arm-uwp: nothing installed yet' -ForegroundColor DarkYellow
    }

    # --- the engine tree, if the chain has got that far ---
    $buildDir = Join-Path $Root 'build-arm32-gpu'
    if (Test-Path (Join-Path $buildDir 'build.ninja')) {
        $objs = @(Get-ChildItem $buildDir -Recurse -Filter '*.obj' -ErrorAction SilentlyContinue).Count
        Write-Host ("engine: build-arm32-gpu configured, {0} object files so far" -f $objs) -ForegroundColor Cyan
    } elseif (Test-Path $buildDir) {
        Write-Host 'engine: build-arm32-gpu exists but is not configured yet' -ForegroundColor DarkYellow
    }
}

function Show-Liveness {
    # --- is it alive, and is it doing anything? ---
    # CPU time is the honest signal. A build step that has used seconds of CPU over
    # tens of minutes is not building, whatever its log last said.
    $names = @('cl', 'link', 'cmake', 'ninja', 'vcpkg', 'nmake', 'perl', 'sh', 'bash', 'make', 'python')
    $procs = @(Get-Process -Name $names -ErrorAction SilentlyContinue)
    Write-Host 'processes' -ForegroundColor Cyan
    if ($procs.Count -eq 0) {
        Write-Host '  none of the build tools are running' -ForegroundColor DarkYellow
    } else {
        $totalCpu = ($procs | Measure-Object -Property CPU -Sum).Sum
        $procs | Group-Object Name | Sort-Object Count -Descending | ForEach-Object {
            $cpu = ($_.Group | Measure-Object -Property CPU -Sum).Sum
            Write-Host ("  {0,-8} x{1,-3} cpu={2,8:N1}s" -f $_.Name, $_.Count, $cpu)
        }
        Write-Host ("  total cpu across build tools: {0:N1}s" -f $totalCpu)
    }

    # --- log freshness: the thing that exposed the seven-hour hang ---
    $newest = $null
    if (Test-Path $LogDir) {
        $newest = Get-ChildItem $LogDir -Filter '*.log' -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTime -Descending | Select-Object -First 1
    }
    $staleMin = $null
    if ($newest) {
        $staleMin = [math]::Round(((Get-Date) - $newest.LastWriteTime).TotalMinutes, 1)
        Write-Host ("newest log: {0}, last written {1} min ago" -f $newest.Name, $staleMin) -ForegroundColor Cyan
    }

    # --- a plain verdict, so the three states are not left to interpretation ---
    Write-Host ''
    if ($procs.Count -eq 0) {
        Write-Host 'verdict: NOT RUNNING -- start it with  pwsh -File Src\tools\arm-bootstrap.ps1' -ForegroundColor DarkYellow
    } elseif ($null -ne $staleMin -and $staleMin -ge 15) {
        Write-Host ("verdict: LOOKS STALLED -- no log output for {0} min. The bootstrap's own watchdog" -f $staleMin) -ForegroundColor Red
        Write-Host '         kills a step after 15 min of silence and retries it, so give it a moment;' -ForegroundColor Red
        Write-Host '         if nothing changes, kill the tree with taskkill /T /F /PID <vcpkg pid>.' -ForegroundColor Red
    } elseif ($procs.Count -gt 0 -and (($procs | Measure-Object -Property CPU -Sum).Sum -lt 5) -and $null -ne $staleMin -and $staleMin -ge 3) {
        Write-Host 'verdict: PROBABLY WAITING ON THE NETWORK -- processes are up but barely using CPU.' -ForegroundColor DarkYellow
        Write-Host '         Normal while a tarball downloads; suspicious if it lasts.' -ForegroundColor DarkYellow
    } else {
        Write-Host 'verdict: WORKING' -ForegroundColor Green
    }
    Write-Host ''
}

if ($Watch) {
    while ($true) { Clear-Host; Show-Status; Show-Liveness; Start-Sleep -Seconds $IntervalSeconds }
} else {
    Show-Status
    Show-Liveness
}

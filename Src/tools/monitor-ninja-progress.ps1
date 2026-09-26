# Ninja progress monitor — read-only; does not control or restart a build.
# Usage:
#   pwsh -File Src/tools/monitor-ninja-progress.ps1 -BuildDir build-x64-19041-probe
#   pwsh -File Src/tools/monitor-ninja-progress.ps1 -BuildDir build-x64-19041-probe -Once
param(
    [string]$BuildDir = 'build-x64-19041-probe',
    [ValidateRange(60,86400)]
    [int]$IntervalSeconds = 1800,
    [switch]$Once
)

$ErrorActionPreference = 'Stop'
if (-not [IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path (Get-Location) $BuildDir
}
$log = Join-Path $BuildDir 'ninja-j1.log'
$state = Join-Path $BuildDir 'ninja-progress-monitor.json'
if (-not (Test-Path $log)) { throw "ninja log not found: $log" }

function Read-Progress {
    $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
    $matches = [regex]::Matches($text, '\[(\d+)/(\d+)\]')
    if ($matches.Count -eq 0) { return $null }
    $m = $matches[$matches.Count - 1]
    [PSCustomObject]@{
        Step = [int]$m.Groups[1].Value
        Total = [int]$m.Groups[2].Value
        At = Get-Date
    }
}

function Format-Duration([double]$seconds) {
    if ($seconds -lt 0 -or [double]::IsNaN($seconds) -or [double]::IsInfinity($seconds)) { return 'unknown' }
    $ts = [TimeSpan]::FromSeconds([math]::Round($seconds))
    if ($ts.TotalHours -ge 1) { return ('{0}h {1:00}m' -f [math]::Floor($ts.TotalHours), $ts.Minutes) }
    if ($ts.TotalMinutes -ge 1) { return ('{0}m {1:00}s' -f [math]::Floor($ts.TotalMinutes), $ts.Seconds) }
    return ('{0:0}s' -f $ts.TotalSeconds)
}

$previous = $null
if (Test-Path $state) { try { $previous = Get-Content $state -Raw | ConvertFrom-Json } catch {} }
$p = Read-Progress
if (-not $p) { Write-Host "No [step/total] progress found in $log"; exit 2 }
$percent = [math]::Round(100.0 * $p.Step / $p.Total, 1)
$eta = 'unknown'
if ($previous -and [int]$previous.Step -gt 0) {
    $dt = ($p.At - [datetime]$previous.At).TotalSeconds
    $ds = $p.Step - [int]$previous.Step
    if ($dt -gt 0 -and $ds -gt 0) {
        $eta = Format-Duration (($p.Total - $p.Step) * $dt / $ds)
    }
}
$remaining = $p.Total - $p.Step
$record = [PSCustomObject]@{ Step=$p.Step; Total=$p.Total; At=$p.At.ToString('o') }
$record | ConvertTo-Json | Set-Content $state -Encoding UTF8
$status = if (Test-Path (Join-Path $BuildDir 'build.ninja')) { 'running-or-paused' } else { 'log-found' }
Write-Host ("[{0}] {1}/{2} = {3}% | remaining={4} | ETA={5} | {6}" -f $p.At.ToString('yyyy-MM-dd HH:mm:ss'),$p.Step,$p.Total,$percent,$remaining,$eta,$status)
if ($p.Step -ge $p.Total) { exit 0 }
if ($Once) { exit 0 }
while ($true) {
    Start-Sleep -Seconds $IntervalSeconds
    try {
        $p = Read-Progress
        if (-not $p) { Write-Host 'No progress marker yet; waiting.'; continue }
        $percent = [math]::Round(100.0 * $p.Step / $p.Total, 1)
        $previous = Get-Content $state -Raw | ConvertFrom-Json
        $dt = ($p.At - [datetime]$previous.At).TotalSeconds
        $ds = $p.Step - [int]$previous.Step
        $eta = if ($dt -gt 0 -and $ds -gt 0) { Format-Duration (($p.Total - $p.Step) * $dt / $ds) } else { 'unknown' }
        Write-Host ("[{0}] {1}/{2} = {3}% | remaining={4} | ETA={5}" -f $p.At.ToString('yyyy-MM-dd HH:mm:ss'),$p.Step,$p.Total,$percent,($p.Total-$p.Step),$eta)
        ([PSCustomObject]@{ Step=$p.Step; Total=$p.Total; At=$p.At.ToString('o') } | ConvertTo-Json) | Set-Content $state -Encoding UTF8
        if ($p.Step -ge $p.Total) { exit 0 }
    } catch { Write-Warning $_.Exception.Message }
}

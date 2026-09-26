# build-monitor.ps1 — live progress for a WebKit/engine build on the x64 or ARM line.
#
# Why this exists: a `-j1` WebKit build takes hours on this machine and produces no console
# output worth watching, so the only honest progress signal is the ninja log itself. This reads
# .ninja_log, compares completed edges against build.ninja, derives a rate from the recent
# window, and prints a countdown. It is read-only: it never touches the build.
#
# Usage (leave running in its own window):
#   pwsh -File Src/tools/build-monitor.ps1
#   pwsh -File Src/tools/build-monitor.ps1 -Tree build-arm-gpu -IntervalSec 20
#   pwsh -File Src/tools/build-monitor.ps1 -Once          # single line, for scripted checks
param(
    [string]$Tree = 'build-x64-gpu',
    [int]$IntervalSec = 15,
    [switch]$Once
)

$ErrorActionPreference = 'Continue'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$dir = if ([IO.Path]::IsPathRooted($Tree)) { $Tree } else { Join-Path $Root $Tree }
$ninjaLog = Join-Path $dir '.ninja_log'
$ninjaFile = Join-Path $dir 'build.ninja'

function Format-Duration([double]$seconds) {
    if ($seconds -lt 0 -or [double]::IsInfinity($seconds) -or [double]::IsNaN($seconds)) { return '--' }
    $ts = [TimeSpan]::FromSeconds([math]::Round($seconds))
    if ($ts.TotalHours -ge 1) { return ('{0}h {1}m' -f [math]::Floor($ts.TotalHours), $ts.Minutes) }
    if ($ts.TotalMinutes -ge 1) { return ('{0}m {1}s' -f [math]::Floor($ts.TotalMinutes), $ts.Seconds) }
    return ('{0}s' -f [math]::Round($ts.TotalSeconds))
}

# Count the edges ninja will consider, and how many the log already records as finished.
# .ninja_log format (ninja >= 1.10): "start_ms end_ms mtime output hash" per completed edge.
function Get-Progress {
    if (-not (Test-Path $ninjaFile)) {
        return [PSCustomObject]@{ Total = 0; Done = 0; RatePerSec = 0.0; EtaSeconds = -1; LastActivity = $null; Running = $false; Mode = 'no build.ninja' }
    }

    # Expected outputs come from build.ninja: every "build <out>.obj: CXX_COMPILER__...|C_COMPILER__...".
    $expected = New-Object System.Collections.Generic.HashSet[string]
    $sr = [IO.StreamReader]::new($ninjaFile)
    try {
        while ($null -ne ($line = $sr.ReadLine())) {
            if ($line -like 'build *.obj: CXX_COMPILER__*' -or $line -like 'build *.obj: C_COMPILER__*') {
                # "build a/b/c.obj: RULE" -> a/b/c.obj
                $body = $line.Substring(6)
                $colon = $body.IndexOf(':')
                if ($colon -gt 0) { [void]$expected.Add($body.Substring(0, $colon).Trim()) }
            }
        }
    } finally { $sr.Dispose() }
    $total = $expected.Count

    # Source of truth for "done" depends on what survived:
    #
    #  * With a usable .ninja_log, its distinct .obj outputs are authoritative and cheap.
    #  * WITHOUT it, fall back to counting the .obj files actually present on disk. That is what
    #    you need for a full rebuild, and it is also the only honest answer after a tree has been
    #    touched in a way that drops the log (see the note on `ninja -n` below).
    $outputs = New-Object System.Collections.Generic.HashSet[string]
    $stamps = New-Object System.Collections.Generic.List[double]
    $fromLog = $false
    if (Test-Path $ninjaLog) {
        $sr = [IO.StreamReader]::new($ninjaLog)
        try {
            $header = $sr.ReadLine()
            # An empty or headerless file means ninja already reset it; treat that as "no log".
            $fromLog = ($header -like '# ninja log v*')
            if ($fromLog) {
                $line = $header
                while ($null -ne ($line = $sr.ReadLine())) {
                    if ([string]::IsNullOrWhiteSpace($line)) { continue }
                    $parts = $line -split '\s+'
                    # start_ms end_ms mtime output hash -- the OUTPUT is field 3 (index 3).
                    if ($parts.Count -ge 4) {
                        $v = 0.0
                        if ([double]::TryParse($parts[1], [ref]$v)) { $stamps.Add($v) }
                        # Count COMPILED objects only. A .ninja_log for this project holds ~20.5k
                        # entries, but ~19k of them are Python generator steps
                        # (generate-js-builtins.py and friends) that finish in milliseconds;
                        # including them makes the counter meaningless (1323%).
                        if ($parts[3] -like '*.obj') { [void]$outputs.Add($parts[3]) }
                    }
                }
            }
        } finally { $sr.Dispose() }
    }

    if (-not $fromLog) {
        # Disk fallback. For each expected object, check existence and note its mtime, so the rate
        # still means "how fast is this build producing files".
        $epoch = [DateTime]::new(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
        $now = [DateTime]::UtcNow
        foreach ($rel in $expected) {
            $abs = Join-Path $dir ($rel -replace '/', '\')
            if (Test-Path -LiteralPath $abs) {
                [void]$outputs.Add($rel)
                $stamps.Add(($now - $epoch.AddTicks((Get-Item -LiteralPath $abs).LastWriteTimeUtc.Ticks)).TotalMilliseconds)
            }
        }
    }

    $done = $outputs.Count
    $last = if ($done -gt 0) { ($stamps | Measure-Object -Maximum).Maximum } else { 0 }

    # Rate from the most recent window, not the whole run: early edges are small and fast, later
    # ones are the big unified sources, so an average over everything overestimates the finish time.
    $rate = 0.0
    $windowMs = 15 * 60 * 1000
    if ($done -ge 2) {
        $recent = @($stamps | Where-Object { $_ -gt ($last - $windowMs) })
        if ($recent.Count -ge 2) {
            $span = ($recent | Measure-Object -Maximum).Maximum - ($recent | Measure-Object -Minimum).Minimum
            if ($span -gt 0) { $rate = ($recent.Count - 1) / ($span / 1000.0) }
        }
    }

    $remaining = [math]::Max(0, $total - $done)
    $eta = if ($rate -gt 0) { $remaining / $rate } else { -1 }
    # "Idle" means no completed edge in the last two minutes. FromUnixTimeMilliseconds is not
    # available on every host PowerShell, so do the epoch conversion by hand.
    $running = $false
    if ($last -gt 0) {
        $epoch = [DateTime]::new(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
        $idleSec = ([DateTime]::UtcNow - $epoch.AddMilliseconds($last)).TotalSeconds
        $running = ($idleSec -lt 120)
    }

    [PSCustomObject]@{
        Total = $total; Done = $done; RatePerSec = $rate
        EtaSeconds = $eta; LastActivity = $last; Running = $running
        Mode = if ($fromLog) { 'ninja_log' } else { 'disk (no ninja_log)' }
    }
}

function Show([PSCustomObject]$p) {
    $pct = if ($p.Total -gt 0) { [math]::Round(100.0 * $p.Done / $p.Total, 1) } else { 0 }
    $barLen = 30
    $filled = if ($p.Total -gt 0) { [math]::Round($barLen * $p.Done / $p.Total) } else { 0 }
    $bar = ('#' * $filled).PadRight($barLen, '.')
    $state = if ($p.Done -eq 0) { 'not started' } elseif ($p.Running) { 'running' } else { 'IDLE (no entry in the last 2 min)' }
    $mode = if ($p.Mode) { $p.Mode } else { 'log' }
    $line = '[{0}] {1,6} / {2,-6} files  {3,5}%  ETA {4,9}  {5}' -f `
        (Get-Date -Format 'HH:mm:ss'), $p.Done, $p.Total, $pct, (Format-Duration $p.EtaSeconds), $bar
    if ($Once) { $line + "  ($state, via $mode)" } else { Write-Host $line -ForegroundColor Cyan }
}

if ($Once) {
    Show (Get-Progress)
    return
}

Write-Host "Watching $dir  (Ctrl+C to stop)" -ForegroundColor Yellow
if (-not (Test-Path $ninjaLog)) { Write-Host "  no .ninja_log yet -- the tree may not be configured" -ForegroundColor DarkYellow }
$lastDone = -1
while ($true) {
    $p = Get-Progress
    Show $p
    # Only say something extra on a change, so a two-hour watch does not become noise.
    if ($p.Done -ne $lastDone) { $lastDone = $p.Done }
    Start-Sleep -Seconds $IntervalSec
}
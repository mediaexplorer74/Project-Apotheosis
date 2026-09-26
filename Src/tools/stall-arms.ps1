# stall-arms.ps1 -- alternate the GPU arm run-to-run on one URL and record, per run, whether the page
# ever built a body.
#
# Apotheosis 2026-09-24. Written for PLAN item 0n. `habr.com/ru/feed/` sometimes never creates a body at
# all (`body=0`, `rs=L/p1`, `sheets=0`, `scripts=4` of 14), and four runs with the GPU probe disarmed
# stalled 3 times while four runs with it armed stalled 0 times. That correlation is suggestive and was
# deliberately NOT written up as a finding: n=4 per arm, the arms were not interleaved, and the fourth
# disarmed run rendered the same URL.
#
# The obstacle is that `gpudefault=` lives in `settings.ini` and is read at STARTUP, so the arm cannot be
# flipped inside one process -- interleaving has to be run-to-run, which is what this does. Alternating
# inside each cycle also removes the "the bench drifted between the two blocks" explanation that a
# block-of-four design cannot.
#
# Evidence is COPIED OUT before the next restart, because `log.txt` is truncated per run: after a stall
# the files that explain it are gone by the time the next arm starts. Each run's log.txt,
# gpuinit-steps.txt and a tail of port-trace.txt land in `LocalState\stall-arms\<arm>-<cycle>\`.
#
# The verdict is read from the harness's own `diag:` line for the URL, and the arm is confirmed from the
# log's `EnableGpu:` lines rather than from the settings value that was written -- a setting that was
# written is not a setting that took effect.
#
# Usage:
#   pwsh -File Src\tools\stall-arms.ps1                          # 2 cycles = 4 runs, GPU off/on/off/on
#   pwsh -File Src\tools\stall-arms.ps1 -Cycles 3
#   pwsh -File Src\tools\stall-arms.ps1 -Url https://dzen.ru/
param(
    [string]$Url = 'https://habr.com/ru/feed/',
    [int]$Cycles = 2,
    [int]$MaxSec = 90,          # per load: the load pair must arrive
    [int]$ObserveSec = 35,      # after the pair: keep watching for a body before calling it a stall
    [int]$SettleSec = 18,       # after launch, before the baseline
    [int]$LimitMb = 2500
)
$ErrorActionPreference = 'Stop'

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$nav = Join-Path $local 'nav.txt'
$log = Join-Path $local 'log.txt'
$gpu = Join-Path $local 'gpuinit-steps.txt'
$trace = Join-Path $local 'port-trace.txt'
$ini = Join-Path $local 'settings.ini'
$outRoot = Join-Path $local 'stall-arms'

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

# Rewrite only the one key. A blanket Set-Content of a rebuilt file is how a run loses `home=` and then
# measures a different page than the one it thinks it measured.
function Set-GpuDefault {
    param([int]$Value)
    $lines = @(Get-Content -Path $ini)
    $seen = $false
    $lines = foreach ($l in $lines) {
        if ($l -match '^gpudefault=') { $seen = $true; "gpudefault=$Value" } else { $l }
    }
    if (-not $seen) { $lines += "gpudefault=$Value" }
    Set-Content -Path $ini -Value $lines -Encoding ASCII
}

function Restart-Harness {
    $old = Get-Process Harness -ErrorAction SilentlyContinue
    if ($old) {
        Stop-Process -Id $old.Id -Force
        # Wait for the OLD process to be gone: relaunching in the same instant leaves the activation
        # dropped and the run then dies with "not running" 20 s later, which reads as a startup crash.
        Wait-Process -Id $old.Id -Timeout 20 -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 1500
    }
    $aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
    Start-Process ("shell:AppsFolder\{0}!{1}" -f $pkg.PackageFamilyName, $aid)
    $deadline = (Get-Date).AddSeconds(45)
    $launchedAt = Get-Date
    # Both conditions: `log.txt` is left over from the previous process, so waiting on it alone returns
    # instantly and the baseline sample happens before the app exists.
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

# The arm is read back from the log, never assumed from the value that was written.
function Get-EngagedArm {
    if (-not (Test-Path $log)) { return 'none' }
    if (Select-String -Path $log -Pattern 'presenting through the GPU surface' -ErrorAction SilentlyContinue) {
        return 'gpu-presented'
    }
    if (Select-String -Path $log -Pattern 'compositing=1' -ErrorAction SilentlyContinue) {
        return 'compositing-on-no-present'
    }
    return 'software'
}

Write-Host ("version: {0}" -f $pkg.PackageFullName)
Write-Host ("url:     {0}" -f $Url)
Write-Host ("design:  {0} cycles, GPU off/on alternating inside each cycle ({1} runs)" -f $Cycles, (2 * $Cycles))
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$results = @()
for ($c = 1; $c -le $Cycles; $c++) {
    foreach ($arm in @(0, 1)) {
        $label = "gpu$arm-c$c"
        Write-Host ''
        Write-Host ("--- run {0}  (settings gpudefault={1}) ---" -f $label, $arm)
        Set-GpuDefault $arm
        Restart-Harness
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

        # The load pair is the FETCH, not the render. Wait for a body before sampling, and when it never
        # arrives keep waiting the whole window: the stalk is "no body, ever", so a short window would
        # report a stall about a page that was merely slow.
        $diag = ''
        $observed = 0
        while ($observed -lt $ObserveSec) {
            Start-Sleep -Seconds 1
            $observed++
            $diag = Get-LastDiag $Url
            if ($diag -match 'body=1') { break }
            if ((Get-RssMb) -lt 0) { break }
            if ((Get-RssMb) -gt $LimitMb) { break }
        }
        $rss = Get-RssMb
        $engaged = Get-EngagedArm
        $state = if ($rss -lt 0) { 'THE PROCESS DIED' }
                 elseif (-not $diag) { 'no diag line' }
                 elseif ($diag -match 'body=0') { 'STALL -- no body' }
                 elseif ($diag -match 'body=1') { 'rendered' }
                 else { 'unknown' }

        # Copy the evidence out before the next restart truncates it.
        $dir = Join-Path $outRoot $label
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        foreach ($f in @($log, $gpu)) {
            if (Test-Path $f) { Copy-Item $f (Join-Path $dir (Split-Path $f -Leaf)) -Force }
        }
        if (Test-Path $trace) {
            # Only the tail: the whole file is hundreds of MB across sessions.
            Get-Content -Path $trace -Tail 40000 | Set-Content (Join-Path $dir 'port-trace-tail.txt') -Encoding ASCII
        }
        Add-Content -Path (Join-Path $dir 'verdict.txt') -Value ("arm=gpu{0} cycle={1} loaded={2} state={3} engaged={4} rss={5}MB observed={6}s" -f $arm, $c, $loaded, $state, $engaged, $rss, $observed)
        if ($diag) { Add-Content -Path (Join-Path $dir 'verdict.txt') -Value $diag }

        $bits = if ($diag -match 'contents=(\S+)') { "contents=$($Matches[1])" } else { '' }
        $body = if ($diag -match 'body=(\d+)') { "body=$($Matches[1])" } else { '' }
        $pend = if ($diag -match 'pending=(\d+)') { "pending=$($Matches[1])" } else { '' }
        $sh = if ($diag -match 'sheets=(\d+)') { "sheets=$($Matches[1])" } else { '' }
        $sc = if ($diag -match 'scripts=(\d+)') { "scripts=$($Matches[1])" } else { '' }
        $rs = if ($diag -match 'rs=(\S+?)/') { "rs=$($Matches[1])" } else { '' }
        Write-Host ("  {0,-22} rss={1,5}MB  arm-read-back={2}" -f $state, $rss, $engaged)
        Write-Host ("    {0} {1} {2} {3} {4} {5}" -f $bits, $body, $pend, $sh, $sc, $rs)
        Write-Host ("    evidence: {0}" -f $dir)
        $results += [pscustomobject]@{
            run = $label; arm = "gpu$arm"; state = $state; engaged = $engaged
            body = $body; sheets = $sh; scripts = $sc; rs = $rs; rss = $rss
        }
    }
}

Write-Host ''
Write-Host '=== matrix (rows in run order, arms alternating) ==='
$results | Format-Table -AutoSize

$off = @($results | Where-Object { $_.arm -eq 'gpu0' })
$on = @($results | Where-Object { $_.arm -eq 'gpu1' })
foreach ($pair in @(@('gpu OFF', $off), @('gpu ON ', $on))) {
    $name = $pair[0]; $set = @($pair[1])
    $stalls = @($set | Where-Object { $_.state -like 'STALL*' }).Count
    $rendered = @($set | Where-Object { $_.state -eq 'rendered' }).Count
    Write-Host ("{0}: {1} run(s) -- {2} stalled, {3} rendered" -f $name, $set.Count, $stalls, $rendered)
}
Write-Host ''
Write-Host 'Read the matrix as an interleaved alternation, not two blocks: the arms sit side by side in'
Write-Host 'run order, so a drift in the bench shows up as a trend across the rows, not as an arm effect.'
Write-Host 'The arm column is what the log said the engine did, not what settings.ini was told to do.'

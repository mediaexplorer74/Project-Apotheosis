# ============================================================================
# analyze-loader-trace.ps1 -- turn port-trace.txt and the diag line into an answer
# about a stalled page load.
#
# The question this exists to settle. When hh.ru stalls, WebCore reports resources
# at CachedResource::Pending (status s1 in the diag line's res: list) and the parser
# sits on one of them forever, while the scheduler in PortPlatformStrategies reports
# itself idle. Something between the two loses a load, and there are three candidate
# places. This script decides which, from evidence the running app already writes:
#
#   1. A load started and never retired  -> the completion path lost it. The
#      scheduler traces a matched pair per request, "loader: +<ptr> <leaf>" on start
#      and "loader: -<ptr> h=<0|1>" on retire, so a surplus of starts over finishes
#      for one address is exactly that. Look at the curl bridge and ResourceLoader's
#      terminal state next.
#   2. A resource WebCore is waiting for was never started at all -> the request
#      never reached the scheduler. Detected by taking the names from the diag res:
#      list and looking for ones with no "+" line. Look at loadResource and
#      scheduleLoad next; note that a declined SubresourceLoader::create already
#      traces "loader: DECLINED", so its absence rules that out.
#   3. Everything matched and nothing is missing -> the scheduler is exonerated and
#      the loss is inside WebCore's own notification, between CachedResource
#      finishing and PendingScript being told. That was the measured answer on
#      2026-08-17: 154 starts, 154 finishes, no h=0, no DECLINED, no curl errors.
#
# Three traps this encodes so nobody repeats them:
#   * MSVC's %p prints a pointer WITHOUT an 0x prefix. A pattern requiring 0x
#     matches nothing and reports a clean run -- a false negative that cost two
#     rounds of hunting.
#   * Addresses are recycled: a retired ResourceLoader's address can be handed to a
#     new one. Set membership therefore lies; the counts have to be per address.
#   * A trace with no "loader:" lines in it at all is not a clean run, it is no data.
#     Guarded below; see the note at the guard for what it cost.
#   * port-trace.txt is APPEND-ONLY across runs. A count taken over the whole file
#     mixes residue from sessions that were force-killed hours earlier. Guarded below
#     by scoping to the last session; measured 2026-09-24, where "STILL OPEN: 19"
#     described a run three restarts back and not the stall being investigated.
#
# Usage:
#   pwsh -File Src\tools\analyze-loader-trace.ps1                  # the live LocalState
#   pwsh -File Src\tools\analyze-loader-trace.ps1 -Dir <folder>     # a preserved copy
# ============================================================================
[CmdletBinding()]
param([string]$Dir)

$ErrorActionPreference = 'Stop'

if (-not $Dir) {
    $p = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Packages') -Filter 'EdgeHTMLReborn.Harness_*' -Directory |
         Select-Object -First 1
    if (-not $p) { throw 'no EdgeHTMLReborn.Harness package folder found; pass -Dir' }
    $Dir = Join-Path $p.FullName 'LocalState'
}
$trace = Join-Path $Dir 'port-trace.txt'
$log   = Join-Path $Dir 'log.txt'
foreach ($f in @($trace, $log)) { if (-not (Test-Path $f)) { throw "missing $f" } }

# Apotheosis 2026-09-18 (ST-5): read the marker channel as well.
# port-trace.txt was measured on 0.1.9.99 and 0.1.9.100 to carry zero `loader:` lines while the
# engine demonstrably loaded, so a tool that reads only that file is blind exactly when it is
# needed. The port's loader tracer now writes the decisive lines to both; this reads both, and the
# guard below fires only when NEITHER has anything. gpuinit-steps.txt is append-only across runs,
# so update-local-x64.ps1 moves it aside before each install for that reason.
$markers = Join-Path $Dir 'gpuinit-steps.txt'
$traceLines = @(Get-Content $trace)
$traceRawCount = $traceLines.Count
$markerCount = 0
$markerLines = @()
if (Test-Path $markers) {
    $markerLines = @(Get-Content $markers)
    $markerCount = $markerLines.Count
}
Write-Host ("reading {0}" -f $Dir) -ForegroundColor Gray
Write-Host ("  port-trace.txt {0} lines, gpuinit-steps.txt {1} lines" -f $traceRawCount, $markerCount) -ForegroundColor Gray

# --- 0. scope to the LAST session -----------------------------------------------
# port-trace.txt accumulates: update-local-x64.ps1 moves gpuinit-steps.txt aside before an install
# but nothing truncates this file, and every force-killed run leaves its pairs behind. Measured
# 2026-09-24 on a file of 209766 lines: `STILL OPEN: 19` was residue from earlier sessions, and
# reading it as the stall's pending set sent one investigation after requests that belonged to a
# different defect. The port restarts its `serve started=` counter at every session, so a DECREASE
# in `started=` across successive serve lines marks a session boundary; everything before the last
# one is somebody else's data. Scoping also protects the "never started" test below, which is built
# from the same `+` lines: a leaf started only in an earlier session used to look started forever.
#
# The scoping is done on THIS FILE ALONE, before the marker file is concatenated, and that ordering
# is not cosmetic. gpuinit-steps.txt also carries mirrored `loader: serve` lines (portLoaderTraceBoth)
# and is append-only across runs, so its `started=` values restart whenever the trace does not -- a
# merged array put the "last decrease" 400 lines inside the marker file and scoped the analysis to a
# 322-line slice with no `+`/`-` pairs in it at all, which the script then reported as a scheduler
# with nothing to say. Measured on LocalState\stall-arms\gpu1-c2: 526 serve lines in port-trace.txt
# with exactly 2 decreases, versus a nonsense 732 once the two files were merged.
$serveStarts = @()
for ($i = 0; $i -lt $traceRawCount; $i++) {
    if ($traceLines[$i] -match '^loader: serve started=(\d+)') { $serveStarts += [pscustomobject]@{ i = $i; started = [int]$matches[1] } }
}
$boundary = 0; $decreases = @()
for ($k = 1; $k -lt $serveStarts.Count; $k++) {
    if ($serveStarts[$k].started -lt $serveStarts[$k - 1].started) { $boundary = $serveStarts[$k].i; $decreases += $boundary }
}
if ($boundary -gt 0) {
    $traceLines = @($traceLines[$boundary..($traceRawCount - 1)])
    Write-Host ("  scoped to the last session: dropped {0} lines, {1} boundary(ies) found at {2}" -f `
        $boundary, $decreases.Count, (($decreases | Select-Object -First 8) -join ', ')) -ForegroundColor Gray
} elseif ($serveStarts.Count -gt 1) {
    Write-Host '  one session in this trace (started= never went backwards)' -ForegroundColor Gray
}
$traceLines += $markerLines

# --- 1. the scheduler's own view -------------------------------------------------
# The g<n> field is the session generation and is optional here on purpose: traces captured before
# the driver started stamping it must still parse. Without the optional group the leaf name would
# come back as "g0 news.css" and every "never started" comparison below would silently fail to
# match, which is worse than not reading the field at all.
$plus = @{}; $minus = @{}; $name = @{}; $hostMisses = 0; $declined = @(); $stale = @()
foreach ($line in $traceLines) {
    if ($line -match '^loader: \+([0-9A-Fa-f]+)(?:\s+g(\d+))?\s+(.*)') {
        $p = $matches[1]; $plus[$p] = 1 + [int]$plus[$p]; $name[$p] = $matches[3].Trim()
    } elseif ($line -match '^loader: -([0-9A-Fa-f]+)(?:\s+g(\d+))?(?:\s+h=(\d))?(?:\s+STALE started-in-g(\d+))?') {
        $p = $matches[1]; $minus[$p] = 1 + [int]$minus[$p]
        if ($matches[3] -eq '0') { $hostMisses++ }
        if ($matches[4]) { $stale += [pscustomobject]@{ addr = $p; startedIn = $matches[4]; retiredIn = $matches[2] } }
    } elseif ($line -match '^loader: DECLINED (.*)') {
        $declined += $matches[1]
    }
}
$marks = @($traceLines | Where-Object { $_ -match '^loader: MARK ' })
$totalStarts = [int]($plus.Values | Measure-Object -Sum).Sum
$totalFins   = [int]($minus.Values | Measure-Object -Sum).Sum
$lastServe   = ($traceLines | Where-Object { $_ -match '^loader: serve' } | Select-Object -Last 1)

# Apotheosis 2026-09-18 (ST-5): the pairing analysis needs the +/- pairs, and those are the bulk
# channel -- written to port-trace.txt only, to keep the marker file readable. So a run can have
# MARK and CALL lines and still have nothing to pair. "starts / finishes : 0 / 0" is then a fact
# about the trace, not about the scheduler, and the verdict at the bottom must not be reached.
$havePairs = ($totalStarts -gt 0 -or $totalFins -gt 0)

# --- 1a. refuse to conclude from a trace that carries no loader traffic ----------
# Measured 2026-09-18 on builds 0.1.9.99 and 0.1.9.100: port-trace.txt held ~1700 lines and ZERO
# `loader:` lines, while the same session rendered Hacker News with five subresources at
# CachedResource::Cached (s2) and nonwhite=587134/710656. Every other writer of that file was
# present in it. This script ran against that empty input and printed, in yellow,
#
#     starts / finishes : 0 / 0
#     verdict: the scheduler is exonerated -- every request started and retired
#
# Every number was a zero read from nothing, and the verdict was believed for a while -- it
# briefly "proved" that a blank page was not a stalled resource. A confident answer from an empty
# trace is worse than no tool at all, so stop instead of proceeding to a verdict. Absence of
# evidence is not evidence of a clean scheduler.
if ($totalStarts -eq 0 -and $totalFins -eq 0 -and $declined.Count -eq 0 -and $marks.Count -eq 0) {
    Write-Host ''
    Write-Host 'NO LOADER ACTIVITY IN THIS TRACE -- cannot conclude anything.' -ForegroundColor Red
    Write-Host ("  neither {0} nor gpuinit-steps.txt has a 'loader: +', a 'loader: -', a DECLINED" -f (Split-Path -Leaf $trace)) -ForegroundColor Red
    Write-Host '  or a MARK line. The engine may well have loaded plenty; nothing here records it.' -ForegroundColor Red
    Write-Host '  Install a build whose PortPlatformStrategies.cpp writes through portDiagLog (0.1.9.101' -ForegroundColor Red
    Write-Host '  and later do), then retry. It also reports its own sink status once per run.' -ForegroundColor Red
    exit 2
}

$open = @()
foreach ($p in $plus.Keys) {
    $d = [int]$plus[$p] - [int]$minus[$p]
    if ($d -gt 0) { $open += [pscustomobject]@{ addr = $p; starts = [int]$plus[$p]; fins = [int]$minus[$p]; name = $name[$p] } }
}

Write-Host ''
Write-Host 'scheduler' -ForegroundColor Cyan
Write-Host ("  last state        : {0}" -f $lastServe)
Write-Host ("  starts / finishes : {0} / {1}   ({2} distinct addresses, so some were recycled)" -f $totalStarts, $totalFins, $plus.Count)
if (-not $havePairs) {
    Write-Host '                    ^ NO PAIRS: neither file has a loader: + or loader: - line.' -ForegroundColor Red
    Write-Host '                      Everything below about exhaustion is unsupported -- the pairs' -ForegroundColor Red
    Write-Host '                      are written to port-trace.txt only, and that file is empty.' -ForegroundColor Red
}
Write-Host ("  host lookup misses: {0}   (h=0 on retire; nonzero means a leaked in-flight slot)" -f $hostMisses)
Write-Host ("  declined creates  : {0}" -f $declined.Count)
foreach ($d in $declined) { Write-Host ("      DECLINED {0}" -f $d) -ForegroundColor Yellow }
# A request that started under one session generation and retired under a later one outlived the
# session that asked for it. That is the shape behind the silent deaths in
# Doc/PUMPLOOP-SILENT-DEATH.md, so it is reported before anything else that survives.
if ($stale.Count) {
    Write-Host ("  CROSSED TEARDOWN  : {0}" -f $stale.Count) -ForegroundColor Red
    foreach ($s in $stale | Select-Object -First 20) {
        Write-Host ("      {0} started in g{1}, retired in g{2}  {3}" -f $s.addr, $s.startedIn, $s.retiredIn, $name[$s.addr]) -ForegroundColor Red
    }
    if ($stale.Count -gt 20) { Write-Host ("      ... and {0} more" -f ($stale.Count - 20)) -ForegroundColor Red }
    Write-Host '      => a response came back after its session was torn down' -ForegroundColor Red
} elseif ($marks.Count) {
    Write-Host '  CROSSED TEARDOWN  : 0' -ForegroundColor Green
}
if ($marks.Count) {
    Write-Host '  teardown marks    :'
    foreach ($m in $marks) { Write-Host ("      {0}" -f $m) }
}
if ($open.Count) {
    Write-Host ("  STILL OPEN        : {0}" -f $open.Count) -ForegroundColor Red
    foreach ($o in $open) { Write-Host ("      {0} +{1}/-{2}  {3}" -f $o.addr, $o.starts, $o.fins, $o.name) -ForegroundColor Red }
    Write-Host '      => a load started and never retired: follow the completion path' -ForegroundColor Red
} else {
    Write-Host '  STILL OPEN        : 0' -ForegroundColor Green
}

# --- 2. what WebCore is still waiting for, and whether it was ever asked for ------
# The diag line's res: list names each resource with its CachedResource::status in
# parentheses: 1 = Pending, 2 = Cached, 3 = LoadError. A name sitting at (s1) that
# never appears in a "+" line was never handed to the scheduler at all, which is a
# different bug from one that was started and lost.
$diagLine = (Select-String -Path $log -Pattern 'latediag\[' | Select-Object -Last 1).Line
if (-not $diagLine) { $diagLine = (Select-String -Path $log -Pattern '^diag: |diag: url=' | Select-Object -Last 1).Line }

Write-Host ''
Write-Host 'document' -ForegroundColor Cyan
if (-not $diagLine) {
    Write-Host '  no diag line in log.txt yet' -ForegroundColor DarkYellow
} else {
    foreach ($f in @('rs=[A-Z]/p\d', 'body=\d', 'nonwhite=\d+/\d+', 'pending=\d+', 'sheets=\d+', 'scripts=\d+', 'defer=\d+ err=\d+ blk=\d+')) {
        if ($diagLine -match $f) { Write-Host ("  {0}" -f $matches[0]) }
    }
    # The stall signature, stated once so it does not have to be re-derived: readyState
    # Loading with no body is the hard stall; rs=I with p1 is the common partial one.
    if ($diagLine -match 'rs=L/' -and $diagLine -match 'body=0') {
        Write-Host '  => HARD STALL: readyState Loading, no body' -ForegroundColor Red
    } elseif ($diagLine -match 'rs=I/p1') {
        Write-Host '  => partial: interactive but still parsing' -ForegroundColor Yellow
    } elseif ($diagLine -match 'rs=C/') {
        Write-Host '  => complete' -ForegroundColor Green
    }

    $pendingNames = @()
    foreach ($m in [regex]::Matches($diagLine, '([^\s\[\]]+)\(s1\)')) { $pendingNames += $m.Groups[1].Value }
    $startedLeaves = @($name.Values | Sort-Object -Unique)
    Write-Host ''
    Write-Host ("  resources at Pending (s1): {0}" -f $pendingNames.Count) -ForegroundColor Cyan
    $neverStarted = @()
    foreach ($n in ($pendingNames | Sort-Object -Unique)) {
        # The trace keeps only the leaf name, truncated to 44 chars, so compare on a prefix.
        $stem = if ($n.Length -gt 20) { $n.Substring(0, 20) } else { $n }
        if (-not ($startedLeaves | Where-Object { $_.StartsWith($stem) })) { $neverStarted += $n }
    }
    if ($neverStarted.Count) {
        Write-Host ("  NEVER STARTED: {0}" -f $neverStarted.Count) -ForegroundColor Red
        foreach ($n in $neverStarted) { Write-Host ("      {0}" -f $n) -ForegroundColor Red }
        Write-Host '      => the request never reached the scheduler: follow loadResource / scheduleLoad' -ForegroundColor Red
    } else {
        Write-Host '  every Pending resource was started at least once' -ForegroundColor Green
    }
}

Write-Host ''
if (-not $open.Count -and $havePairs -and $diagLine -and -not $neverStarted.Count -and ($diagLine -notmatch 'rs=C/')) {
    Write-Host 'verdict: the scheduler is exonerated -- every request started and retired, and nothing' -ForegroundColor Yellow
    Write-Host '         WebCore waits for was skipped. The loss is inside the notification between a' -ForegroundColor Yellow
    Write-Host '         finished CachedResource and the PendingScript / parser that waits on it.' -ForegroundColor Yellow
}

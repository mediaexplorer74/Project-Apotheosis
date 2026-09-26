<#
  analyze-runloop-trace.ps1 -- join the WTF RunLoop `rlife:` markers in port-trace.txt.

  Why this exists: the `crashDueToCheckedPtrToDeadObject()` abort (Doc/RUNLOOP-TEARDOWN-ABORT.md)
  is a `CheckedPtr` to a ScheduledTask that was deleted while the RunLoop schedule tree still
  linked it. The tree is the only holder of `CheckedPtr<ScheduledTask>` in the tree, so the
  question is always "which task was destroyed while still scheduled, and from which thread".
  The markers are flushed per line but carry no timestamps, so they are joined by ADDRESS:

      rlife: loop-create a=<loop>            tid=<creator>     -- the loop's owning thread
      rlife: timer-create a=<timer> b=<loop> tid=<creator  nt>
      rlife: timer-destroy-enter a=<timer>   tid=<destroyer>
      rlife: sched|unsched a=<task> b=<loop> tid=<actor>

  A timer whose creating thread differs from its loop's creating thread is a timer driven from
  outside its own run loop -- legal only because the fork made the loop lock re-entrant
  (ApoLoopLocker), and the shape to look for when the schedule tree goes wrong.

  Usage:  pwsh -File Src\tools\analyze-runloop-trace.ps1 [-Path <port-trace.txt>]
#>
[CmdletBinding()]
param(
    [string]$Path
)

$ErrorActionPreference = 'Stop'

if (-not $Path) {
    $pkg = Get-ChildItem "$env:LOCALAPPDATA\Packages" -Directory -Filter 'EdgeHTMLReborn.Harness_*' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $pkg) { throw "no LocalState found; pass -Path" }
    $Path = Join-Path $pkg.FullName 'LocalState\port-trace.txt'
}
if (-not (Test-Path $Path)) { throw "not found: $Path" }

Write-Host "trace: $Path ($([Math]::Round((Get-Item $Path).Length/1MB,1)) MB)"

# --- one pass, keep only the markers we join on ------------------------------------------------
$loopCreate  = @{}   # loop  -> creator tid
$timerCreate = @{}   # timer -> @{ loop; tid }
$timerDie    = @{}   # timer -> @{ tid; count; exit }
$scheds      = 0
$unscheds    = 0
$lastSched   = @{}   # task  -> line (for tasks scheduled more often than unscheduled)
$lastUnsch   = @{}   # task  -> line
$loopTreeEnd = @{}   # loop  -> last extra= seen

foreach ($line in [System.IO.File]::ReadLines($Path)) {
    if (-not $line.StartsWith('rlife:')) { continue }
    if ($line -match '^rlife: loop-create a=(\S+).*tid=(\d+)') {
        $loopCreate[$Matches[1]] = [int]$Matches[2]; continue
    }
    if ($line -match '^rlife: timer-create a=(\S+) b=(\S+).*tid=(\d+)') {
        $timerCreate[$Matches[1]] = @{ loop = $Matches[2]; tid = [int]$Matches[3] }; continue
    }
    if ($line -match '^rlife: timer-destroy-enter a=(\S+) b=(\S+) count=(\d+) extra=(\d+) tid=(\d+)') {
        $timerDie[$Matches[1]] = @{ tid = [int]$Matches[5]; count = [int]$Matches[3]; scheduled = [int]$Matches[4]; b = $Matches[2] }
        continue
    }
    if ($line -match '^rlife: sched a=(\S+) b=(\S+) count=(\d+) extra=(\d+) tid=(\d+)') {
        $scheds++; $lastSched[$Matches[1]] = $line; $loopTreeEnd[$Matches[2]] = [int]$Matches[4]; continue
    }
    if ($line -match '^rlife: unsched a=(\S+) b=(\S+) count=(\d+) extra=(\d+) tid=(\d+)') {
        $unscheds++; $lastUnsch[$Matches[1]] = $line; $loopTreeEnd[$Matches[2]] = [int]$Matches[4]; continue
    }
}

Write-Host ""
Write-Host "loops created   : $($loopCreate.Count)"
Write-Host "timers created  : $($timerCreate.Count)"
Write-Host "timers destroyed: $($timerDie.Count)   (created-but-never-destroyed: $($timerCreate.Count - $timerDie.Count))"
Write-Host "sched / unsched : $scheds / $unscheds   (scheds with no matching unsched: $($scheds - $unscheds))"

# --- cross-thread timers: the timer's thread vs its loop's thread -------------------------------
$cross = @()
foreach ($t in $timerCreate.Keys) {
    $loop = $timerCreate[$t].loop
    $loopTid = if ($loopCreate.ContainsKey($loop)) { $loopCreate[$loop] } else { -1 }
    if ($loopTid -ne $timerCreate[$t].tid) {
        $cross += [pscustomobject]@{ timer = $t; loop = $loop; createdOn = $timerCreate[$t].tid; loopOwner = $loopTid }
    }
}
Write-Host ""
if ($cross.Count) {
    Write-Host "CROSS-THREAD TIMERS (timer created on a thread that does not own its RunLoop): $($cross.Count)"
    $cross | Group-Object { "createdOn=$($_.createdOn) loopOwner=$($_.loopOwner)" } |
        Sort-Object Count -Descending | Select-Object Count, Name | Format-Table -AutoSize
} else {
    Write-Host "cross-thread timers: none -- every timer was created on its own loop's thread"
}

# --- timers destroyed by a thread other than the one that created them --------------------------
$crossDie = @()
foreach ($t in $timerDie.Keys) {
    if ($timerCreate.ContainsKey($t) -and $timerCreate[$t].tid -ne $timerDie[$t].tid) {
        $crossDie += [pscustomobject]@{ timer = $t; createdOn = $timerCreate[$t].tid; destroyedOn = $timerDie[$t].tid }
    }
}
Write-Host ""
if ($crossDie.Count) {
    Write-Host "TIMERS DESTROYED CROSS-THREAD: $($crossDie.Count)"
    $crossDie | Group-Object { "createdOn=$($_.createdOn) destroyedOn=$($_.destroyedOn)" } |
        Sort-Object Count -Descending | Select-Object Count, Name | Format-Table -AutoSize
} else {
    Write-Host "cross-thread timer destruction: none"
}

# --- THE POISON SIGNATURE: a task destroyed while still scheduled ------------------------------
Write-Host ""
$poison = $timerDie.GetEnumerator() | Where-Object { $_.Value.scheduled -ne 0 -or $_.Value.count -eq 0 -and $_.Value.b -ne '0000000000000000' }
if ($poison) {
    Write-Host "tasks destroyed with a non-zero isScheduled flag (THE POISON SIGNATURE):"
    $poison | ForEach-Object { "  $($_.Key)  $($_.Value | ConvertTo-Json -Compress)" }
} else {
    Write-Host "no timer was destroyed while its ScheduledTask still reported isScheduled -- the poison"
    Write-Host "(a task deleted while the tree linked it) did NOT happen before timer-destroy lines."
}

# --- tasks scheduled but never unscheduled -----------------------------------------------------
Write-Host ""
$leaked = $lastSched.Keys | Where-Object { -not $lastUnsch.ContainsKey($_) }
if ($leaked) {
    Write-Host "tasks with a sched and no unsched at all: $($leaked.Count)"
    $leaked | Select-Object -First 10 | ForEach-Object { "  $_  " + $lastSched[$_] }
} else {
    Write-Host "every scheduled task was also unscheduled at least once"
}

# --- loops whose tree was non-empty the last time we saw them ----------------------------------
Write-Host ""
$live = $loopTreeEnd.GetEnumerator() | Where-Object { $_.Value -gt 0 }
Write-Host "loops last seen with a non-empty schedule tree: $($live.Count)"
$live | Sort-Object { -$_.Value } | Select-Object -First 10 | ForEach-Object {
    $owner = if ($loopCreate.ContainsKey($_.Key)) { $loopCreate[$_.Key] } else { '?' }
    "  loop=$($_.Key) ownerTid=$owner nodesLeft=$($_.Value)"
}

# --- the RedBlackTree integrity probe: TREE-* and task-destroy (added 2026-09-19) ---------------
#
# `apoRBCheck` (RunLoopGeneric.cpp) walks m_schedules after every insert and remove and compares the
# SUM of the nodes' checked-ptr counts against the tree size; `TREE-BAD-REM-COUNT` / `-INS-COUNT` are
# the O(1) checks at the two call sites, and `task-destroy` is the poison alarm in ~ScheduledTask.
#
# How to read it: in a well-formed tree of n nodes the structure holds exactly n CheckedPtrs, so the
# counts must sum to exactly n. A sum larger than the size is a link held by a node the tree no longer
# owns -- `RedBlackTree::remove()` left the removed node's own children attached to it -- and once it
# has happened the trace is contaminated: the abort that follows is a consequence, not a second bug.
# Do not read a TREE-BAD line as harmless: it is the poison, one step before it kills something.
# Doc/RUNLOOP-TEARDOWN-ABORT.md §8-§9.
$tree = @{}          # marker -> count
$firstTree = @{}     # marker -> first raw line
$poisonTask = @()    # task-destroy with a live link on it

foreach ($line in [System.IO.File]::ReadLines($Path)) {
    if ($line -match '^rlife: (TREE-[A-Z-]+) ') {
        $m = $Matches[1]
        $tree[$m] = 1 + [int]$tree[$m]
        if (-not $firstTree.ContainsKey($m)) { $firstTree[$m] = $line }
        continue
    }
    if ($line -match '^rlife: task-destroy a=(\S+) b=(\S+) count=(\d+) extra=(\d+)') {
        if ([int]$Matches[3] -ne 0 -or [int]$Matches[4] -ne 0) { $poisonTask += $line }
    }
}

Write-Host ""
Write-Host "=== RedBlackTree integrity probe (TREE-*) ==="
if (-not $tree.Count) {
    Write-Host "no TREE-* marker at all. Either this build predates the probe (< 0.1.10.29), or the tree"
    Write-Host "never went wrong in this session. Check the build before concluding the second."
    Write-Host "  how: the probe lives in JavaScriptCore.dll, NOT WebCore.dll -- search the appx payload,"
    Write-Host "  since that is what the installed package contains, e.g."
    Write-Host "    unzip -p <Harness_x_y_z.appx> JavaScriptCore.dll > /tmp/j.dll; grep -a -c TREE-BAD-AFTER-UNSCHED /tmp/j.dll"
    Write-Host "  verified 2026-09-19: 0 lines on 0.1.10.30 (fixed) vs 330 on 0.1.10.29 (probe, no fix)."
} else {
    $tree.GetEnumerator() | Sort-Object Name | ForEach-Object { "  {0,-26} {1,6}" -f $_.Key, $_.Value }
    Write-Host ""
    Write-Host "first occurrence of each marker:"
    $firstTree.GetEnumerator() | Sort-Object Name | ForEach-Object { "  " + $_.Value }
}
Write-Host ""
if ($poisonTask.Count) {
    Write-Host "tasks DESTROYED with a live link on them (count != 0) or still flagged scheduled: $($poisonTask.Count)"
    $poisonTask | Select-Object -First 10 | ForEach-Object { "  $_" }
} else {
    Write-Host "no task was destroyed with a link on it -- the poison never reached a delete"
}

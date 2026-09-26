# tap-test.ps1 -- exercise the MVP acceptance criterion ("responds to touch without crashing") on the
# bench, from a script, and report the verdict the harness itself produced.
#
# Apotheosis 2026-09-24. The tap path is the one code path whose behaviour matters most on a phone and
# the one that could not be driven without a human finger: synthetic input never reaches XAML's
# manipulation stack on the bench (memory `synthetic-input-cannot-reach-uwp`, measured), and the tap
# dispatcher is only reachable from that stack. `LocalState\nav.txt` closes that gap since 2026-09-18
# (`taplink:<n>` / `taplinkstr:<text>` -> `TapFromScript` -> `HandleTapAt`, the identical function a
# finger enters), but nothing drove it -- the capability existed and had never been used. This is the
# driver.
#
# What it does, in order: load a link-rich page through nav.txt, wait for that load's own
# `[STAGE] after-load` pair (NOT for a `diag:` -- the harness writes diag after resizes too, so the
# next diag is not "the load finished"; see load-time.ps1), tap link n BY NAME, wait for the harness's
# verdict, then navigate once more to prove the app still works. That last step is half the criterion:
# "without crashing" is not observable at the moment of the tap, only after it.
#
# The verdict is read from three markers the harness writes, never inferred from exit status:
#   `simtap: ...`          which command was honoured, which link was chosen and its rect/url; also
#                          the form every refusal takes (REFUSED/OUT-OF-VIEWPORT/BAD COMMAND/no match)
#   `[HIT] x,y -> <tag>`   the element under the point BEFORE dispatch, plus `[HIT] settled:` (WebCore's
#                          own view) and `[HIT] dom rc=...` (the page's own view of the event)
#   `TapDone: ... branch=` the branch the dispatcher actually took, with refused=/preEmpty=/termDiff=
# Silence is reported as the finding it is, and it is attributed: a missing `simtap:` is the nav.txt
# watcher not firing, a `simtap:` with no `[HIT]` is the click never reaching the engine, and a `[HIT]`
# with no `TapDone:` is the verdict never being written -- three different defects that a bare
# "no result" would merge into one.
#
# Usage:
#   pwsh -File Src\tools\tap-test.ps1                                  # HN, link #0
#   pwsh -File Src\tools\tap-test.ps1 -Link 3
#   pwsh -File Src\tools\tap-test.ps1 -Url https://habr.com/ru/feed/ -LinkText habr.com
#   pwsh -File Src\tools\tap-test.ps1 -NoDrive -Since '12:30:00'       # parse an existing log only
param(
    [string]$Url = 'https://news.ycombinator.com/',
    [int]$Link = 0,
    [string]$LinkText = '',
    [string]$At = '',              # "x,y" in ContentArea DIP -> `tap:x,y`, no link table needed
    [int]$SettleSec = 4,          # after the load pair, before the tap; the link table must be built
    [int]$MaxSec = 45,            # per wait (load, tap, alive check)
    [string]$AliveUrl = 'https://example.com/',
    [switch]$NoAliveCheck,
    [switch]$NoLoad,              # tap the page that is already there; do not navigate first
    [switch]$NoDrive
)
$ErrorActionPreference = 'Stop'

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$nav = Join-Path $local 'nav.txt'
$log = Join-Path $local 'log.txt'
# `[HIT]` is written by the DRIVER, so it lands in gpuinit-steps.txt; `simtap:` / `TapDone:` are written
# by the harness (LogWriteF) and land in log.txt. Reading only one of the two files makes the two
# halves of a tap invisible to each other and the report says "the click never reached the engine"
# about a click the engine answered in full. Measured on the first run of this script.
$gpu = Join-Path $local 'gpuinit-steps.txt'
if (-not (Test-Path $log)) { throw "no log.txt at $log -- is the app running?" }

function Count-Pat {
    param([string]$Pattern, [string]$Path = $log)
    return @(Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue).Count
}

# Wait for a pattern's occurrence count to EXCEED a baseline. Counting, not testing existence: every
# marker here repeats across a session, so "present" is always true after the first tap and a test on
# presence would return immediately with the previous tap's line.
function Wait-Pat {
    param([string]$Pattern, [int]$Baseline, [int]$TimeoutSec, [string]$Path = $log)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Count-Pat $Pattern $Path) -gt $Baseline) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

# The lines a pattern matched since a baseline count, so the report shows evidence rather than a boolean.
function Get-PatLines {
    param([string]$Pattern, [int]$Skip, [string]$Path = $log)
    $all = @(Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue | ForEach-Object { $_.Line })
    if ($Skip -ge $all.Count) { return @() }
    return $all[$Skip..($all.Count - 1)]
}

function Invoke-Nav {
    param([string]$Value)
    Set-Content -Path $nav -Value $Value -Encoding ASCII
}

$proc = Get-Process -Name Harness -ErrorAction SilentlyContinue | Select-Object -First 1
Write-Host ("harness: pid={0} {1}" -f ($(if ($proc) { $proc.Id } else { 'none' })),
    $(if ($proc) { "rss=$([int]($proc.WorkingSet64 / 1MB))MB" } else { 'NOT RUNNING' }))
if (-not $proc) { throw 'Harness is not running -- launch it first (launch-local-x64.ps1)' }

$cmd = if ($At) { "tap:$At" } elseif ($LinkText) { "taplinkstr:$LinkText" } else { "taplink:$Link" }

# --------------------------------------------------------------------------------------------- drive
if (-not $NoDrive) {
    if (-not $NoLoad) {
        if ($SettleSec -gt 0 -and (Count-Pat '\[STAGE\] after-load') -eq 0) {
            Write-Host 'waiting for the first page to settle...'
            [void](Wait-Pat '\[STAGE\] after-load' 0 $MaxSec)
        }
        $navBase = Count-Pat '\[STAGE\] after-load'
        Write-Host ("load: {0}" -f $Url)
        Invoke-Nav $Url
        if (-not (Wait-Pat '\[STAGE\] after-load' $navBase $MaxSec)) {
            Write-Host 'RESULT: the page never finished loading -- no tap was sent'
            Write-Host "  last stage line: $(Get-PatLines '\[STAGE\] after-load' $navBase | Select-Object -Last 1)"
            exit 2
        }
        $links = Get-PatLines 'links=\d+' $navBase | Select-Object -Last 1
        if ($links -and $links -match 'links=(\d+)') { Write-Host ("  link table: {0} entries" -f $Matches[1]) }
        Start-Sleep -Seconds $SettleSec
    } else {
        $navBase = Count-Pat '\[STAGE\] after-load'
        Write-Host 'load: skipped (-NoLoad) -- tapping the page already there'
        # The link table is rebuilt on every frame, and a tap needs the CURRENT one. `simtap: taplink:N
        # -- only M links in the table` is the marker that says this went wrong, so it is reported
        # rather than retried. `tap:<x>,<y>` needs no table at all and is the fallback when the page
        # never produced one (a page stuck before its body, for instance).
        $links = Get-PatLines 'links=\d+' 0 | Select-Object -Last 1
        if ($links -and $links -match 'links=(\d+)') { Write-Host ("  last known link table: {0} entries" -f $Matches[1]) }
    }

    # Baselines are taken immediately before the tap, so the report cannot quote the previous tap's
    # verdict as this one's -- the single most likely way to misread a session of several taps.
    $simBase  = Count-Pat 'simtap:'
    $hitBase  = Count-Pat '\[HIT\]' $gpu
    $doneBase = Count-Pat 'TapDone:'
    Write-Host ("tap: {0}" -f $cmd)
    Invoke-Nav $cmd
    $sawSim  = Wait-Pat 'simtap:'   $simBase  $MaxSec
    # The verdict is NOT a beat after the click -- it is the END of the click. `ForwardClickToEngine`
    # runs a pump loop over the tap (30 s watchdog, 160-tick cap) and only then writes TapDone, so on a
    # heavy page the wait is seconds: measured 2026-09-24 on habr.com/ru/feed/, `simtap:` at 20:54:25.189
    # and `TapDone:` at 20:54:33.968 -- 8.8 s, which an 8 s window reported as "the verdict path itself
    # failed to run". Wait the full MaxSec and measure the latency, because the latency is the finding:
    # the harness gives the user NO feedback for those seconds.
    $tapAt = Get-Date
    $sawDone = Wait-Pat 'TapDone:'  $doneBase $MaxSec
    $verdictSec = [math]::Round(((Get-Date) - $tapAt).TotalSeconds, 1)
} else {
    $simBase = 0; $hitBase = 0; $doneBase = 0
    $sawSim = (Count-Pat 'simtap:') -gt 0
    $sawDone = (Count-Pat 'TapDone:') -gt 0
}

# --------------------------------------------------------------------------------------------- report
Write-Host ''
foreach ($l in (Get-PatLines 'simtap:' $simBase))          { Write-Host "  $l" }
foreach ($l in (Get-PatLines '\[HIT\]' $hitBase $gpu))     { Write-Host "  $l" }
foreach ($l in (Get-PatLines 'TapDone:' $doneBase))        { Write-Host "  $l" }

# Silence is attributed, never just reported: each of these three is a different defect.
if (-not $sawSim) {
    Write-Host ''
    Write-Host 'RESULT: NO VERDICT -- nav.txt was written and no `simtap:` line appeared.'
    Write-Host '  The nav.txt watcher did not fire (app not running / watcher not started / write raced'
    Write-Host '  a previous command). This says nothing about the tap path itself.'
    exit 2
}
if (-not $sawDone) {
    $hit = @(Get-PatLines '\[HIT\]' $hitBase $gpu).Count
    Write-Host ''
    if ($hit -eq 0) {
        Write-Host 'RESULT: NO VERDICT -- the command was honoured but the click never reached the engine.'
        Write-Host '  Expect a simtap: REFUSED / OUT-OF-VIEWPORT / BAD COMMAND / "no match" line above;'
        Write-Host '  if the only line is the chosen link, the harness refused the tap while loading.'
    } else {
        Write-Host 'RESULT: NO VERDICT -- the click reached the engine (HIT markers present) and the'
        Write-Host '  dispatcher never wrote TapDone: the harness verdict path itself failed to run.'
    }
    exit 3
}

$done = (Get-PatLines 'TapDone:' $doneBase | Select-Object -Last 1)
if ($done -match 'branch=(\S+)') {
    $branch = $Matches[1]
    Write-Host ''
    Write-Host ("branch taken: {0}   (verdict {1} s after the tap)" -f $branch, $verdictSec)
    Write-Host '  nav-link = the harness navigated to the link;  in-page = it decided the page handled it'
    Write-Host '  itself;  nav-fail = the link was dead;  dead = no link under the point'
    Write-Host '  NOTE: `in-page` with navEmpty=0 is the ENGINE following the anchor itself -- the good'
    Write-Host '  path, not a miss. `nav-link` means the ENGINE did not move and the harness navigated.'
}

# ----------------------------------------------------------------------------------- still alive?
if (-not $NoAliveCheck) {
    $aliveBase = Count-Pat '\[STAGE\] after-load'
    Write-Host ''
    Write-Host ("alive check: {0}" -f $AliveUrl)
    Invoke-Nav $AliveUrl
    if (-not (Wait-Pat '\[STAGE\] after-load' $aliveBase $MaxSec)) {
        $p2 = Get-Process -Name Harness -ErrorAction SilentlyContinue
        Write-Host ("RESULT: NO ANSWER AFTER THE TAP -- process {0}" `
            -f $(if ($p2) { 'is alive but never finished the next load' } else { 'IS GONE' }))
        exit 4
    }
    $p2 = Get-Process -Name Harness -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p2) { Write-Host 'RESULT: THE PROCESS DIED AFTER THE TAP'; exit 4 }
    Write-Host ("  survived: rss={0}MB (was {1}MB at the start)" -f `
        [int]($p2.WorkingSet64 / 1MB), [int]($proc.WorkingSet64 / 1MB))
}

Write-Host ''
Write-Host 'RESULT: a tap verdict was produced and the app answered afterwards.'

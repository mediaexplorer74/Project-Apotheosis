<#
.SYNOPSIS
    Watch the Apotheosis process's memory on the Lumia while a page loads, over WDP.

.DESCRIPTION
    A UWP app on Windows 10 Mobile is killed by the OS when it busts its memory limit, and that kill
    leaves nothing behind: no crash dump, no WER record, and no dangling line in the port's own trace,
    because the process is destroyed between two statements rather than inside one. On 2026-10-03 both
    recorded dzen.ru deaths had exactly that shape, so the question "did the engine fault or did the OS
    reap it?" cannot be answered from the logs at all -- it has to be sampled from outside, live.

    This polls /api/resourcemanager/processes, which this device's WDP does implement (measured 3.9 s
    for the whole 66-process list; /api/taskmanager/processes answers 501 Not Implemented, so do not
    reach for that one). Each sample is one line, flushed, and the last sample before the process
    disappears is the number to read: a smooth climb that approaches a ceiling looks like a quota kill,
    while a flat curve that simply stops looks like a fault.

    The call itself costs ~4 s on this phone's WiFi, so -IntervalSec is a floor, not a period; the
    per-sample elapsed time is printed so the real cadence is never guessed at.

.PARAMETER Ip
    Device IP. Defaults to 192.168.1.35, the Lumia 950 this repo is developed against.

.PARAMETER NameMatch
    Substring matched against ImageName, case-insensitively. Defaults to 'potheosis'.

.PARAMETER IntervalSec
    Minimum seconds between samples. The WDP call usually takes longer than this.

.PARAMETER MaxSec
    Give up after this many seconds even if the process is still alive.

.PARAMETER OutFile
    TSV log, appended. Defaults to watch-ws.txt in the current directory.

.EXAMPLE
    pwsh -File Src/tools/watch-ws.ps1
    # then, on the phone: open the app and navigate to dzen.ru
#>
[CmdletBinding()]
param(
    [string]$Ip = '192.168.1.35',
    [string]$NameMatch = 'potheosis',
    [double]$IntervalSec = 3,
    [int]$MaxSec = 600,
    [string]$OutFile = 'watch-ws.txt'
)

$ErrorActionPreference = 'Stop'
$uri = "https://${Ip}:443/api/resourcemanager/processes"
$sw = [Diagnostics.Stopwatch]::StartNew()

function Sample {
    # Returns the row for our process, or $null when it is gone. A failed call is NOT a death: the
    # phone's WiFi drops WDP calls, so an error is reported and the loop keeps trying.
    try {
        $rm = Invoke-RestMethod -Uri $uri -SkipCertificateCheck -TimeoutSec 30 -Method GET
    } catch {
        Write-Host ("  {0,7:N1}s  WDP call failed: {1}" -f $sw.Elapsed.TotalSeconds, $_.Exception.Message) -ForegroundColor DarkYellow
        return 'retry'
    }
    return @($rm.Processes | Where-Object { $_.ImageName -like "*$NameMatch*" }) | Select-Object -First 1
}

Write-Host "watching '$NameMatch' on $Ip; start the app on the phone now" -ForegroundColor Cyan
$saw = $false
$last = $null
$lastPid = 0
$n = 0

while ($sw.Elapsed.TotalSeconds -lt $MaxSec) {
    $row = Sample
    if ($row -ne 'retry') {
        if ($null -eq $row) {
            if ($saw) {
                # A disappearance is reported but does NOT end the run. The app being relaunched is
                # the normal shape of a crash investigation -- the whole point is to catch the next
                # attempt too -- and stopping here once cost a measurement on 2026-10-03, when the
                # watcher quit on a six-second sighting and was not running for the two launches that
                # actually mattered. A new pid is announced so the samples are not read as one life.
                Write-Host ("{0,8:N1}s  PROCESS GONE (pid={1}; still watching)" -f $sw.Elapsed.TotalSeconds, $lastPid) -ForegroundColor Red
                $lastPid = 0
                # Clearing $saw is what makes the disappearance a one-sample event. Left set, the
                # pid=0 line repeats on every subsequent sample forever, which reads as "the process
                # keeps dying" rather than "it is not running" -- and drowns the relaunch it is
                # supposed to be watching for. Cleared, the next samples fall back to the "not running"
                # cadence below, and a relaunch re-announces a new pid exactly as it should.
                $saw = $false
            }
            elseif ($n % 5 -eq 0) { Write-Host ("{0,8:N1}s  not running" -f $sw.Elapsed.TotalSeconds) -ForegroundColor DarkGray }
        } else {
            if ($row.ProcessId -ne $lastPid) {
                Write-Host ("{0,8:N1}s  -- new process pid={1}" -f $sw.Elapsed.TotalSeconds, $row.ProcessId) -ForegroundColor Cyan
                $lastPid = $row.ProcessId
            }
            $saw = $true
            $last = $row
            $line = "{0,7:N1}s  pid={1}  WS={2,7:N1} MB  privWS={3,7:N1} MB  pagefile={4,7:N1} MB  virt={5,7:N1} MB  cpu={6}" -f `
                $sw.Elapsed.TotalSeconds, $row.ProcessId, ($row.WorkingSetSize / 1MB), `
                ($row.PrivateWorkingSet / 1MB), ($row.PageFileUsage / 1MB), ($row.VirtualSize / 1MB), $row.CPUUsage
            Write-Host $line
            Add-Content -LiteralPath $OutFile -Value ("{0}`t{1}`t{2}`t{3}`t{4}" -f `
                [DateTime]::Now.ToString('HH:mm:ss'), $row.ProcessId, $row.WorkingSetSize, $row.PrivateWorkingSet, $row.PageFileUsage)
        }
    }
    $n++
    $wait = $IntervalSec - ($sw.Elapsed.TotalSeconds % $IntervalSec)
    Start-Sleep -Milliseconds ([int]([Math]::Max(0.2, $wait) * 1000))
}

if ($null -ne $last) {
    Write-Host ("`nlast seen: pid={0}  WS={1:N1} MB  privWS={2:N1} MB  pagefile={3:N1} MB" -f `
        $last.ProcessId, ($last.WorkingSetSize / 1MB), ($last.PrivateWorkingSet / 1MB), ($last.PageFileUsage / 1MB)) -ForegroundColor Cyan
}
Write-Host "log: $OutFile" -ForegroundColor DarkGray

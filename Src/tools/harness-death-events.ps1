# Apotheosis: ask Windows what it thought of the last Harness death.
#
# Why this exists. On 2026-09-18 the harness died on the x64 bench with every in-process channel
# silent at once: log.txt simply stops, unhandled.txt is absent, the UEF lines are absent, the
# wedgedump machinery never arms (the UI thread was still beating), and WER leaves no dump. That
# signature is produced by at least three different mechanisms, and they need different fixes:
#
#   * an unhandled SEH exception  -> Application Error event 1000, and LocalDumps would have caught it;
#   * a fail-fast (abort / __fastfail / RoFailFastWithErrorContext) -> WER event 1001 with a bucket,
#     but no exception dispatch, so SetUnhandledExceptionFilter never runs;
#   * a platform kill (PLM suspend-then-reap, memory, app-model)   -> no Application-error event at
#     all, only AppModel-Runtime / TWinUI records, and nothing in-process could ever see it.
#
# The event log distinguishes them where the app's own logs cannot, which is the whole point: the
# app's logs are the thing that went silent.
#
# Read-only. It changes nothing.

param(
    [string]$ProcessName = 'Harness',
    [int]$Minutes = 120
)

$ErrorActionPreference = 'SilentlyContinue'
$since = (Get-Date).AddMinutes(-$Minutes)

function Show($evts, $label) {
    $list = @($evts)
    Write-Host ""
    Write-Host "=== $label : $($list.Count) event(s) ==="
    foreach ($e in ($list | Select-Object -First 15)) {
        Write-Host ("  {0}  [{1}] id={2}" -f $e.TimeCreated.ToString('HH:mm:ss'), $e.ProviderName, $e.Id)
        $msg = ($e.Message -replace "`r", "")
        foreach ($line in (($msg -split "`n") | Where-Object { $_.Trim() } | Select-Object -First 6)) {
            Write-Host ("      " + $line.Trim())
        }
    }
}

$app = Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$since} |
       Where-Object { $_.Id -in 1000, 1001, 1002 } -ErrorAction SilentlyContinue
Show $app "Application log: crash / WER events"

$model = Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-AppModel-Runtime/Admin'; StartTime=$since} -ErrorAction SilentlyContinue
Show $model "AppModel-Runtime: package / process lifecycle"

$twinui = Get-WinEvent -LogName 'Microsoft-Windows-TWinUI/Operational' -MaxEvents 40 -ErrorAction SilentlyContinue |
          Where-Object { $_.TimeCreated -gt $since }
Show $twinui "TWinUI: activation / suspend"

$dumps = @()
foreach ($d in @("$env:LOCALAPPDATA\CrashDumps", "$env:LOCALAPPDATA\Packages")) {
    if (Test-Path $d) {
        $dumps += Get-ChildItem -Path $d -Recurse -Filter '*.dmp' -ErrorAction SilentlyContinue |
                  Where-Object { $_.LastWriteTime -gt $since }
    }
}
Write-Host ""
Write-Host "=== dumps written in the last $Minutes min: $($dumps.Count) ==="
foreach ($d in $dumps) { Write-Host ("  {0}  {1:N0} bytes  {2}" -f $d.LastWriteTime.ToString('HH:mm:ss'), $d.Length, $d.FullName) }

$key = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Harness.exe'
Write-Host ""
Write-Host "=== LocalDumps<=Harness.exe> ==="
if (Test-Path $key) { Get-ItemProperty $key | Format-List } else { Write-Host "  (not configured)" }

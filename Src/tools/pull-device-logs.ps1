#Requires -Version 7.0
<#
  pull-device-logs.ps1 -- copy the diagnostic files out of the app's LocalState over Device Portal
  and SAVE THEM TO DISK, one local file per remote file.

  Why this exists next to pull-file.ps1. That one prints a file to the console, which is fine for
  stage.txt and useless for port-trace.txt: the trace appends for the whole life of the install and
  reaches megabytes, so printing it floods whatever is reading the output. This saves instead, and
  prints only a size and the tail, so a caller decides what to look at.

  It also resolves the package itself. Every earlier script took -Ver and -Pub by hand and the
  defaults in them are years stale; the version on the device is whatever was last deployed, which
  is exactly the thing an investigator does not know. So ask WDP.

  Reading is the only filesystem operation Device Portal can do here -- writing LocalState returns
  HTTP 500 in every spelling. Nothing in this script deletes or uploads anything: the logs on the
  device are evidence, and clearing them has already cost one investigation its crash trail.

  The app must be CLOSED for the read to be complete: the process holds these files open and the
  last lines may not be visible otherwise. Waking the phone's radio takes tens of seconds after it
  has been idle, which is why the timeout is generous and the first request is retried.

  Usage:
    pwsh -File Src\tools\pull-device-logs.ps1 -Ip 192.168.1.35
    pwsh -File Src\tools\pull-device-logs.ps1 -Ip 192.168.1.35 -Files port-trace.txt,log.txt
#>
[CmdletBinding()]
param(
    [string]$Ip = '192.168.1.35',
    [string]$OutDir,
    # Which installed package's LocalState to read. This used to be hardcoded to the C++ harness, so
    # the script answered "Harness is not installed on this device" on any machine where only the
    # Apotheosis app was deployed -- the app being the thing the ARM line now ships. WDP needs the
    # package FULL name (name_version_arch__publisher), which is also what makes it unambiguous when
    # several versions are registered side by side; the pattern picks which one.
    # S5 (2026-10-21): the default follows the shipped app. The old default named the C++ harness
    # package, which no line has deployed since the app became the product; every recent pull passed
    # -PackagePattern 'ME.Apotheosis_*' by hand (probe authors kept forgetting it -- see ROADMAP S5).
    [string]$PackagePattern = 'ME.Apotheosis_*',
    [int]$TimeoutSec = 90,
    [string[]]$Files = @(
        'port-trace.txt',       # driver: loader scheduling and the settle loop, appended and flushed per line
        # The chunks the rotation is *supposed* to move aside, as the driver's (.1) and the loader's
        # (.2) -- distinct names on purpose, so the destructive step cannot delete the other writer's
        # chunk. Kept in the default list because a pull that dropped them would silently lose
        # everything but the newest chunk IF rotation ever happened.
        #
        # Measured 2026-10-09 (second `.30` run): it never has. port-trace.txt reached 6,223,695 B
        # against the 4 MB ceiling with no .1 and no .2, and still carried the .26-.29 sessions, so
        # it has never rotated once in this install's life. Cause: apoTraceAppend
        # (WebKit/Source/WTF/wtf/generic/RunLoopGeneric.cpp:57) caches its FILE* for the process
        # lifetime, and MSVC's fopen omits FILE_SHARE_DELETE, so the rotation's rename() fails with a
        # sharing violation and its handler is a bare `return`. Expect these two to 404; pull
        # port-trace.txt whole. See Doc/Archive/Summary.md and Doc/BUILD-ARM32.md.
        'port-trace.1.txt',
        'port-trace.2.txt',
        # The driver's step-marker channel, under BOTH names -- the app calls
        # WebCoreSetGpuInitLogFile with "gpu-steps.txt" (MainPage.xaml.cs:219) while the C++ harness
        # passes "gpuinit-steps.txt". Listing only the harness name is why several `.30` pulls came
        # back 404 for a file that existed and was being written: the delivered app writes
        # gpu-steps.txt. Read it happily WHILE the app runs -- gpuLogMarker does
        # fopen("a")/fclose per line, so unlike port-trace.txt this file is never held open.
        'gpu-steps.txt',        # app: the zoom/layout line and every DBG_STAGE marker
        'gpuinit-steps.txt',    # harness: same channel, harness's filename
        'log.txt',              # harness: startup, navigation, STAGE markers, the diag line
        # The dying session's log.txt, copied aside by the crash-verdict code before LogInit truncates
        # it. Only present after a crash, and it is the only copy left once the app has been relaunched
        # -- which is easy to do by reflex and unrecoverable.
        'crash-report.txt',
        'heartbeat.txt',        # last UI beat, and the engine's busy/pending/finished/stage at that moment
        'ctor-trace.txt',       # harness: MainPage constructor checkpoints, before log.txt is live
        'unhandled.txt',        # harness: unhandled UI-thread exception. Absent unless one happened
        'stage.txt',            # last stage reached
        'stage-port.txt',       # driver side of the same
        'layertree.txt',        # layer tree dump
        'jitresult.txt',        # JIT probe verdict
        'navseq-active.txt'     # the sequence the player actually saw, if it ran
    ),
    [int]$TailLines = 0        # print this many trailing lines per file; 0 prints none
)

# `-Files a,b,c` does NOT arrive as three elements when this script is invoked as `pwsh -File`, which
# is how the usage lines above call it. `-File` passes every argument as a single string, so a
# comma-separated list binds to a ONE-element array holding "a,b,c" -- and the script then asks WDP for
# a file literally named "a,b,c" and reports it "absent", which reads exactly like a missing log. That
# cost a diagnosis on 2026-10-03. Splitting here makes the documented invocation mean what it says.
$Files = @($Files | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http

if (-not $OutDir) {
    $root = $env:APOTHEOSIS_ROOT
    if (-not $root) { $root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutDir = Join-Path $root "Src\tools\device-logs\$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$base = "https://${Ip}:443"
$handler = [System.Net.Http.HttpClientHandler]::new()
$handler.ServerCertificateCustomValidationCallback =
    [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$handler.CookieContainer = [System.Net.CookieContainer]::new()
$cli = [System.Net.Http.HttpClient]::new($handler)
# A long session's trace can grow past what 90 s of WDP read gets through (the .42 run timed out at
# 90 s on port-trace.txt alone). Callers with a known-big file pass a longer -TimeoutSec; the value
# bounds the whole download, not the per-read idle time.
$cli.Timeout = [TimeSpan]::FromSeconds($TimeoutSec)

$script:Csrf = $null
function Sync-Csrf($resp) {
    $sc = $null
    if ($resp.Headers.TryGetValues('Set-Cookie', [ref]$sc)) {
        foreach ($line in $sc) {
            if ($line -match 'CSRF-Token=([^;,\s]+)') {
                $script:Csrf = $Matches[1]
                try {
                    $handler.CookieContainer.Add([Uri]$base,
                        [System.Net.Cookie]::new('CSRF-Token', $script:Csrf, '/', $Ip))
                } catch { }
            }
        }
    }
}

function Invoke-Wdp([string]$url) {
    $req = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $url)
    if ($script:Csrf) { $req.Headers.Add('X-CSRF-Token', $script:Csrf) }
    return $cli.SendAsync($req).GetAwaiter().GetResult()
}

# The radio sleeps. Retry the first contact rather than reporting the phone as absent.
$osInfo = $null
foreach ($attempt in 1..3) {
    try { $osInfo = Invoke-Wdp "$base/api/os/info"; break }
    catch { Write-Host "  contact attempt $attempt failed, retrying" -ForegroundColor DarkYellow }
}
if (-not $osInfo) { Write-Host "DEVICE UNREACHABLE at $Ip" -ForegroundColor Red; exit 2 }
Sync-Csrf $osInfo

# Resolve the installed package rather than trusting a -Ver argument.
$pkgResp = Invoke-Wdp "$base/api/app/packagemanager/packages"
Sync-Csrf $pkgResp
$pkgs = ($pkgResp.Content.ReadAsStringAsync().Result | ConvertFrom-Json).InstalledPackages |
        Where-Object { $_.PackageFullName -like $PackagePattern }
if (-not $pkgs) { Write-Host "no installed package matches '$PackagePattern'" -ForegroundColor Red; exit 3 }
if ($pkgs.Count -gt 1) {
    Write-Host "NOTE: $($pkgs.Count) packages match '$PackagePattern'; using the highest version" -ForegroundColor Yellow
    $pkgs = $pkgs | Sort-Object { [version]("{0}.{1}.{2}.{3}" -f $_.Version.Major, $_.Version.Minor, $_.Version.Build, $_.Version.Revision) } -Descending
}
$full = @($pkgs)[0].PackageFullName
Write-Host "package : $full"
Write-Host "outdir  : $OutDir"

$fullEnc = [uri]::EscapeDataString($full)
$results = @()
foreach ($file in $Files) {
    $url = "$base/api/filesystem/apps/file?knownfolderid=LocalAppData" +
           "&packagefullname=$fullEnc&path=%5CLocalState&filename=$([uri]::EscapeDataString($file))"
    $status = 0
    try { $resp = Invoke-Wdp $url; $status = [int]$resp.StatusCode }
    catch { Write-Host ("  {0,-22} FAILED: {1}" -f $file, $_.Exception.Message) -ForegroundColor DarkYellow; continue }
    if ($status -ne 200) {
        Write-Host ("  {0,-22} HTTP {1} (absent)" -f $file, $status) -ForegroundColor DarkGray
        continue
    }
    # Save bytes, not a decoded string: these files are written by fopen("ab") in the driver and
    # a decode round-trip would rewrite the line endings we may need to reason about.
    $bytes = $resp.Content.ReadAsByteArrayAsync().Result
    $dest = Join-Path $OutDir $file
    [System.IO.File]::WriteAllBytes($dest, $bytes)
    $lines = (Get-Content -LiteralPath $dest | Measure-Object -Line).Lines
    Write-Host ("  {0,-22} {1,9} bytes  {2,6} lines" -f $file, $bytes.Length, $lines) -ForegroundColor Green
    $results += [pscustomobject]@{ File = $file; Bytes = $bytes.Length; Lines = $lines; Path = $dest }
    if ($TailLines -gt 0) {
        Get-Content -LiteralPath $dest -Tail $TailLines | ForEach-Object { Write-Host "      $_" }
    }
}
Write-Host ''
Write-Host "PULLED: $($results.Count) file(s) into $OutDir"

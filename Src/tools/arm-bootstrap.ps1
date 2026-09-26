# ============================================================================
# arm-bootstrap.ps1 -- unattended bring-up of the ARM32 build line.
#
# Purpose: the ARM line needs a long chain of prerequisites and then a
# multi-hour engine build. Run by hand, that means babysitting. This script runs
# the whole chain unattended, timestamps every step, stops at the FIRST failure,
# and leaves a one-page verdict -- so an unattended window produces either real
# progress or a precise diagnosis, never silence.
#
# Design notes, each of them the result of something that went wrong before:
#   * Every step runs in its OWN pwsh process. arm32-uwp-env.ps1 rewrites
#     INCLUDE / LIB / PATH in the current session, so sharing one shell between
#     vcpkg, the meson deps and ninja is how the two architectures poison each
#     other (see Src\port\assert-target.ps1).
#   * The preflight refuses to start if the MSVC ARM toolset is absent. That
#     check costs a second and saves four hours: without it the first hours-long
#     step fails at the end instead of the beginning.
#   * ninja runs with -j1, always: 4 GB of RAM and unified-source translation
#     units above 1 GB each.
#   * The vcpkg port list is not hardcoded. It is read from what the working
#     x64-uwp triplet already has installed, so the ARM line mirrors the line
#     that is known to work instead of a list someone typed once.
#
# Usage:
#   pwsh -File Src\tools\arm-bootstrap.ps1                 # whole chain
#   pwsh -File Src\tools\arm-bootstrap.ps1 -PreflightOnly  # just check, no work
#   pwsh -File Src\tools\arm-bootstrap.ps1 -SkipVcpkg -SkipDeps   # resume later
#   pwsh -File Src\tools\arm-bootstrap.ps1 -StopBeforeEngine      # deps only
#
# Logs: Src\tools\arm-bootstrap-logs\<step>.log, plus summary.txt and
# verdict.txt in the same directory.
# ============================================================================
[CmdletBinding()]
param(
    [switch]$PreflightOnly,
    [switch]$SkipVcpkg,
    [switch]$SkipSqlite,
    [switch]$SkipDeps,
    [switch]$SkipConfigure,
    [switch]$StopBeforeEngine
)

$ErrorActionPreference = 'Stop'

$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$Port = Join-Path $Root 'Src\port'
$LogDir = Join-Path $PSScriptRoot 'arm-bootstrap-logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
# Overlay triplet/port paths are needed by both the vcpkg mirror step and the ICU step.
$overlay = Join-Path $Root 'Src\port\vcpkg-triplets'
$overlayPorts = Join-Path $Root 'Src\port\vcpkg-ports'

$Summary = Join-Path $LogDir 'summary.txt'
$Verdict = Join-Path $LogDir 'verdict.txt'
Remove-Item $Verdict -ErrorAction SilentlyContinue

$VS = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$Toolset = '14.44.35207'
$T = "$VS\VC\Tools\MSVC\$Toolset"
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$SdkVer = '10.0.19041.0'

function Say([string]$msg, [string]$colour = 'Gray') {
    $line = ('{0}  {1}' -f (Get-Date -Format 'HH:mm:ss'), $msg)
    Write-Host $line -ForegroundColor $colour
    Add-Content -Path $Summary -Value $line
}

function Fail([string]$step, [string]$why, [string]$log) {
    $text = @()
    $text += "ARM bootstrap FAILED"
    $text += "step:   $step"
    $text += "reason: $why"
    $text += "time:   $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    if ($log -and (Test-Path $log)) {
        $text += ''
        $text += "--- last 40 lines of $(Split-Path -Leaf $log) ---"
        $text += (Get-Content $log -Tail 40)
    }
    Set-Content -Path $Verdict -Value $text
    Say "FAILED at $step -- $why" 'Red'
    Say "verdict written to $Verdict" 'Yellow'
    exit 1
}

# Run one step as its own process, with a stall watchdog, retrying on failure.
#
# Retries exist because fetching a dozen dependency tarballs over a long run reliably trips over
# transient network trouble: the first full run died on GitHub answering "429 Too Many Requests" from
# codeload for the whole IP.
#
# The watchdog exists because retries alone were wishful thinking. A step that *fails* gets retried; a
# step that *hangs* never does. Measured 2026-08-17 into the small hours: libiconv's tarball stopped at
# 14216 bytes and vcpkg then sat there for seven and a half hours, having used six seconds of CPU, with
# the laptop stone cold. curl passes no timeout of its own, and the _curlrc guard that should have
# caught it was in %USERPROFILE% while curl on Windows reads %APPDATA%\_curlrc unless HOME is set --
# true inside Git Bash, false for a process vcpkg starts. Both are fixed, but a watchdog is the thing
# that makes it not matter which of those two ever breaks again: no output for $stallMinutes means the
# process tree gets killed and the attempt counts as a failure, which is what the retry loop knows how
# to handle. Fifteen minutes is chosen to be longer than any single quiet stretch a real build has --
# vcpkg prints per port, ninja prints per file.
function Step([string]$name, [string]$exe, [string[]]$argv, [int]$expectMinutes, [int]$retries = 1,
              [int]$pauseSeconds = 90, [int]$stallMinutes = 15) {
    $log = Join-Path $LogDir "$name.log"
    $errLog = Join-Path $LogDir "$name.err.log"
    for ($attempt = 1; $attempt -le $retries; $attempt++) {
        $suffix = if ($retries -gt 1) { " (attempt $attempt of $retries)" } else { '' }
        Say ("step {0} -- starting{1} (expect roughly {2} min)" -f $name, $suffix, $expectMinutes) 'Cyan'
        Set-Content -Path $log -Value '' -ErrorAction SilentlyContinue
        Set-Content -Path $errLog -Value '' -ErrorAction SilentlyContinue
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $proc = Start-Process -FilePath $exe -ArgumentList $argv -NoNewWindow -PassThru `
                    -RedirectStandardOutput $log -RedirectStandardError $errLog
        $stalled = $false
        while (-not $proc.HasExited) {
            Start-Sleep -Seconds 20
            $last = (Get-Item $log -ErrorAction SilentlyContinue).LastWriteTime
            if ($null -eq $last) { $last = $sw.Elapsed.Ticks }   # log not created yet; the elapsed cap below still applies
            if ($last -is [datetime] -and ((Get-Date) - $last).TotalMinutes -ge $stallMinutes) {
                Say ("step {0} -- no output for {1} min, killing it as stalled" -f $name, $stallMinutes) 'Red'
                & taskkill /T /F /PID $proc.Id *> $null
                $stalled = $true
                break
            }
        }
        if (-not $stalled) { $proc.WaitForExit() }
        $rc = if ($stalled) { 9009 } else { $proc.ExitCode }
        $sw.Stop()
        $mins = [math]::Round($sw.Elapsed.TotalMinutes, 1)
        if ($rc -eq 0) { Say ("step {0} -- ok in {1} min" -f $name, $mins) 'Green'; return }
        $why = if ($stalled) { 'stalled' } else { "exit $rc" }
        if ($attempt -lt $retries) {
            Say ("step {0} -- {1} after {2} min; waiting {3} s before retrying" -f $name, $why, $mins, $pauseSeconds) 'DarkYellow'
            Start-Sleep -Seconds $pauseSeconds
        } else {
            Fail $name "$why after $mins min ($attempt attempt(s))" $log
        }
    }
}

Set-Content -Path $Summary -Value ("=== ARM bootstrap started {0} ===" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Say "root=$Root"

# ---------------------------------------------------------------------------
# Preflight. Everything here is a second's work and a four-hour saving.
# ---------------------------------------------------------------------------
Say 'preflight' 'Cyan'
$missing = @()

# The MSVC ARM toolset. This is the one thing this script cannot install itself:
# it comes from the Visual Studio Installer and needs the user's consent.
if (-not (Test-Path "$T\bin\Hostx64\arm\cl.exe")) { $missing += "MSVC ARM compiler: $T\bin\Hostx64\arm\cl.exe" }
if (-not (Test-Path "$T\lib\arm\store"))          { $missing += "MSVC ARM store libraries: $T\lib\arm\store" }
if ($missing.Count) {
    Say ''
    Say 'The MSVC ARM32 toolset is not installed. Add it with:' 'Yellow'
    Say ''
    Say '  "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify \' 'White'
    Say '      --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" \' 'White'
    Say ('      --add Microsoft.VisualStudio.Component.VC.{0}.ARM --passive --norestart' -f '14.44.17.14') 'White'
    Say ''
    Say 'or in the installer GUI: Individual components, search "ARM", tick' 'Yellow'
    Say '"MSVC v143 - VS 2022 C++ ARM build tools (v14.44-17.14)".' 'Yellow'
    Say 'Pick the VERSIONED component, not "(latest)": the whole tree is pinned to' 'Yellow'
    Say ('toolset {0} and a newer one would not match.' -f $Toolset) 'Yellow'
    Fail 'preflight' ("missing: " + ($missing -join '; ')) $null
}
Say "  ok   MSVC ARM toolset $Toolset"

# The SDK half. 19041 is the only SDK with ARM32 libraries at all.
foreach ($p in @("$SDK\Lib\$SdkVer\um\arm\WindowsApp.lib", "$SDK\Lib\$SdkVer\ucrt\arm", "$SDK\Include\$SdkVer\ucrt")) {
    if (-not (Test-Path $p)) { Fail 'preflight' "Windows SDK $SdkVer is incomplete: $p is missing" $null }
}
Say "  ok   Windows SDK $SdkVer with ARM32 libraries"

# Apotheosis: three shims the ARM32 line needs on a stock VS 2022 17.14 + SDK box. All were found
# the hard way on 2026-08-17, each one costing a failed vcpkg run, so they are asserted (and fixed)
# here rather than left to be rediscovered.
#
# 1. VS 2022 17.14 installs the ARM32 compiler but ships no arm32 vcvars wrappers, and vcpkg
#    enumerates available target architectures purely by looking for those files. Without them it
#    reports "available toolchain combinations are: x86, amd64, x86_arm64, amd64_arm64" and refuses
#    arm before reading any triplet. Each wrapper is one line and vcvarsall still supports x64_arm.
$vcBuild = "$VS\VC\Auxiliary\Build"
foreach ($pair in @(@{ f = 'vcvarsamd64_arm.bat'; a = 'x64_arm' }, @{ f = 'vcvarsx86_arm.bat'; a = 'x86_arm' })) {
    $path = Join-Path $vcBuild $pair.f
    if (-not (Test-Path $path)) {
        $body = @(
            '',
            '@rem Apotheosis: created by Src\tools\arm-bootstrap.ps1. VS 2022 17.14 ships no arm32',
            '@rem wrapper, and vcpkg enumerates target architectures by the presence of these files.',
            ('@call "%~dp0vcvarsall.bat" {0} {1} %*' -f $pair.a, $SdkVer)
        )
        try {
            Set-Content -Path $path -Value $body -Encoding ASCII
            Say ("  made {0} (was missing; vcpkg needs it to offer the arm target)" -f $pair.f)
        } catch {
            Fail 'preflight' ("cannot create {0}: {1}" -f $path, $_.Exception.Message) $null
        }
    } else { Say ("  ok   {0}" -f $pair.f) }
}

# 2. vcvarsall composes LIB with `ucrt` from 19041 (26100 has no ucrt\arm, so it falls back) but
#    `um` from the NEWEST SDK, 26100, whose um\arm does not exist at all. The result is a LIB entry
#    pointing at a missing directory and every ARM link dying with
#    "LINK : fatal error LNK1104: cannot open file 'WindowsApp.lib'" inside vcpkg's own compiler
#    probe. vcvarsall ignores WindowsSdkVersion from the environment and vcpkg never passes the SDK
#    positionally, so the practical fix is to make the path it insists on resolve to the only ARM
#    libraries that exist. A junction does that, needs no elevation, and is removed with rmdir.
$umArm26100 = "$SDK\Lib\10.0.26100.0\um\arm"
$umArm19041 = "$SDK\Lib\$SdkVer\um\arm"
if (-not (Test-Path $umArm26100)) {
    if (Test-Path "$SDK\Lib\10.0.26100.0\um") {
        & cmd.exe /c "mklink /J `"$umArm26100`" `"$umArm19041`"" *> $null
        if (Test-Path (Join-Path $umArm26100 'WindowsApp.lib')) {
            Say '  made Lib\10.0.26100.0\um\arm as a junction to 19041 (vcvarsall points LIB there)'
        } else {
            Fail 'preflight' "could not make $umArm26100 resolve to $umArm19041; ARM links will fail on WindowsApp.lib" $null
        }
    }
} else { Say '  ok   Lib\10.0.26100.0\um\arm resolves' }

# Generators. Ruby is a hard dependency: offlineasm builds JSC's LLInt from it.
# Resolve it the same way arm32-uwp-env.ps1 does, and then export the answer so every child step
# inherits it instead of re-guessing. On this machine ruby is at C:\tools\ruby34\bin and is NOT on
# PATH, which an earlier version of this check treated as "ruby missing" -- a false blocker that
# would have stopped the ARM bring-up after the VS component was already installed.
if (-not $env:APOTHEOSIS_RUBY) {
    $rubyDir = if (Get-Command ruby -ErrorAction SilentlyContinue) { Split-Path -Parent (Get-Command ruby).Source }
               else { @('C:\tools\ruby34\bin', 'C:\Ruby34-x64\bin', 'C:\Ruby33-x64\bin') |
                      Where-Object { Test-Path (Join-Path $_ 'ruby.exe') } | Select-Object -First 1 }
    if ($rubyDir) { $env:APOTHEOSIS_RUBY = $rubyDir }
}
if (-not $env:APOTHEOSIS_RUBY -or -not (Test-Path (Join-Path $env:APOTHEOSIS_RUBY 'ruby.exe'))) {
    Fail 'preflight' 'ruby not found on PATH, in APOTHEOSIS_RUBY, or in the known install layouts; offlineasm cannot run' $null
}
Say ("  ok   ruby ({0})" -f $env:APOTHEOSIS_RUBY)
foreach ($pair in @(@{n='python'; p="$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"},
                    @{n='perl';   p='C:\Program Files\Git\usr\bin\perl.exe'})) {
    if (-not (Test-Path $pair.p)) { Fail 'preflight' ("{0} is missing at {1}" -f $pair.n, $pair.p) $null }
    Say ('  ok   {0}' -f $pair.n)
}

# ANGLE. Tracked headers plus re-downloadable binaries; both must be present.
foreach ($p in @("$Root\Src\angle\arm", "$Root\Src\angle\include")) {
    if (-not (Test-Path $p)) { Fail 'preflight' "ANGLE is missing: $p" $null }
}
Say '  ok   ANGLE ARM binaries and headers'

# The upstream tree.
if (-not (Test-Path "$Root\WebKit\Source\WebCore")) { Fail 'preflight' "upstream tree missing: $Root\WebKit\Source\WebCore (see Src\tools\fetch-webkit-source.ps1)" $null }
Say '  ok   WebKit source tree'

if (-not (Test-Path 'C:\vcpkg\vcpkg.exe')) { Fail 'preflight' 'vcpkg is not at C:\vcpkg' $null }
Say '  ok   vcpkg'

Say 'preflight passed' 'Green'
if ($PreflightOnly) { Say 'preflight only, stopping here'; exit 0 }

# ---------------------------------------------------------------------------
# 1. vcpkg third-party stack for arm-uwp.
# The port list mirrors whatever the working x64-uwp triplet has, so the ARM line
# gets the same stack instead of a list someone typed once and forgot. All ports go
# in one invocation: vcpkg resolves the build order itself, and a failure inside it
# is exactly the diagnosis we want to stop on.
# ---------------------------------------------------------------------------
if (-not $SkipVcpkg) {
    $listLog = Join-Path $LogDir 'vcpkg-list.log'
    # Apotheosis: `vcpkg list` needs the same lock a build holds, so it comes back empty if another
    # vcpkg is running -- which happened once when two bootstrap runs overlapped, and the guard below
    # then reported "nothing to mirror" as if the x64 triplet were empty. Retry before believing it.
    $ports = $null
    for ($listTry = 1; $listTry -le 5; $listTry++) {
        & 'C:\vcpkg\vcpkg.exe' list *>&1 | Tee-Object -FilePath $listLog | Out-Null
        if ((Select-String -Path $listLog -Pattern ':x64-uwp' -Quiet -ErrorAction SilentlyContinue)) { break }
        Say ("vcpkg list came back without any x64-uwp rows (attempt {0} of 5); another vcpkg may hold the lock" -f $listTry) 'DarkYellow'
        Start-Sleep -Seconds 30
    }
    # Mirror the x64-uwp triplet WITH its installed features, not just the bare port names.
    # `vcpkg list` prints one row per port and one further row per enabled feature, e.g.
    #   curl:x64-uwp            8.21.0#1   A library for transferring data with URLs
    #   curl[openssl]:x64-uwp              TLS support (OpenSSL)
    # Half of the 28 x64-uwp rows on this machine are feature rows, and they are not cosmetic:
    # curl[openssl] is what gives the port its own TLS 1.3, which is the whole reason HTTPS works
    # inside an App Container with no system certificate store. Installing plain `curl:arm-uwp`
    # would quietly build the ARM stack against a weaker curl and the divergence would only show
    # up much later, on the device, as failing HTTPS.
    $features = @{}
    foreach ($line in (Get-Content $listLog)) {
        if ($line -match '^([A-Za-z0-9_.+-]+)(?:\[([^\]]+)\])?:x64-uwp\b') {
            $port = $matches[1]
            if (-not $features.ContainsKey($port)) { $features[$port] = New-Object System.Collections.Generic.List[string] }
            if ($matches[2]) { [void]$features[$port].Add($matches[2]) }
        }
    }
    if ($features.Count -eq 0) {
        Fail 'vcpkg' 'could not read any x64-uwp ports from "vcpkg list"; nothing to mirror' $listLog
    }
    $specs = foreach ($port in ($features.Keys | Sort-Object)) {
        if ($features[$port].Count) { '{0}[{1}]:arm-uwp' -f $port, (($features[$port] | Sort-Object -Unique) -join ',') }
        else { '{0}:arm-uwp' -f $port }
    }
    # Apotheosis: mirror x64 minus what the ARM line demonstrably does not need and cannot build.
    #
    # pthreads (pthreads-win32 3.0.0) calls LoadLibrary in pthread_win32_process_attach_np, to pick up
    # QUSER100.DLL for QueueUserAPCEx-based cancellation. LoadLibrary sits outside
    # WINAPI_PARTITION_APP in SDK 19041 -- the same constraint that stopped the harness compiling until
    # its loader calls became LoadPackagedLibrary -- so the port fails with
    #   pthread_win32_attach_detach_np.c(92): warning C4013: 'LoadLibrary' undefined
    #   pthread.obj : error LNK2019: unresolved external symbol LoadLibrary
    # It builds for x64-uwp only because the newer SDK exposes LoadLibrary to app-container code.
    # Patching it would be pointless work: Doc/BUILD-ARM32.md section 3 lists what the ARM stack
    # actually takes from vcpkg -- curl, OpenSSL, libxml2, sqlite3, zlib, bzip2, brotli, libjpeg,
    # libpng, libwebp, expat, pixman, freetype -- and pthreads is not among them. Nothing in the
    # installed arm-uwp tree references it, and neither configure-gpu-arm32.ps1 nor
    # link-driver-gpu-arm32.ps1 mentions it. It is a leftover of the x64 tree, so it is dropped rather
    # than carried.
    # If a port ever genuinely needs it, vcpkg will pull it in as a dependency and the build will say
    # so, which is the right way to find out.
    $excluded = @('pthreads')
    $specs = @($specs | Where-Object { $_ -notmatch ('^(' + ($excluded -join '|') + ')(\[|:)') })
    foreach ($e in $excluded) { Say ("vcpkg: excluding {0} (see the comment in arm-bootstrap.ps1)" -f $e) 'DarkYellow' }
    Say ("vcpkg: mirroring {0} ports to arm-uwp -- {1}" -f $features.Count, ($specs -join ' '))
    # The overlay triplet is not optional: vcpkg's stock community arm-uwp triplet does not pin the
    # SDK, so CMake picks 26100 and the compiler probe fails to link. See the file's own comments.
    # The overlay *port* is equally not optional: openssl 3.6.3 compiles its winstore provider for
    # any Windows target, and that provider needs CertOpenSystemStoreW, which 19041's ARM
    # WindowsApp.lib does not export. Src\port\vcpkg-ports\openssl adds no-winstore for UWP and
    # explains why that is correct rather than merely convenient.
    $argv = @('install') + $specs + @("--overlay-triplets=$overlay", "--overlay-ports=$overlayPorts", '--clean-after-build')
    # Retries with a long pause, not a short one: the failure this survives is GitHub answering
    # "429: Too Many Requests" from codeload.github.com for the whole IP, which is a time window of
    # minutes, not seconds. Six attempts eight minutes apart buys about three quarters of an hour of
    # patience, which is nothing next to the hours the rest of the chain takes.
    Step 'vcpkg-arm-uwp' 'C:\vcpkg\vcpkg.exe' $argv 150 6 480
} else { Say 'vcpkg: skipped by switch' 'DarkYellow' }

# ---------------------------------------------------------------------------
# 2. ICU 78 via vcpkg for arm-uwp, then the SQLite amalgamation for ARM.
# ICU is NOT part of the x64-uwp mirror list (the x64 line keeps a custom build
# at C:\icu-x64-uwp instead), so the ARM line installs it explicitly. It must run
# before the engine configure, and find_package(ICU 70.1) then resolves against
# C:\vcpkg\installed\arm-uwp, which is also configure-gpu-arm32.ps1's IcuRoot.
# vcpkg's sqlite3 port has no WinRT code path and cannot compile for arm-uwp, so
# sqlite3 comes from the amalgamation -- see build-sqlite-arm32.ps1.
# ---------------------------------------------------------------------------
if (-not $SkipVcpkg) {
    Step 'icu-arm-uwp' 'C:\vcpkg\vcpkg.exe' @('install', 'icu', '--triplet', 'arm-uwp',
        "--overlay-triplets=$overlay", "--overlay-ports=$overlayPorts", '--clean-after-build') 60 1 120 15
} else { Say 'icu: skipped by switch' 'DarkYellow' }
if (-not $SkipSqlite) {
    Step 'sqlite-arm32' 'pwsh' @('-NoProfile', '-File', (Join-Path $Port 'build-sqlite-arm32.ps1')) 5
} else { Say 'sqlite: skipped by switch' 'DarkYellow' }

# ---------------------------------------------------------------------------
# 3. The meson-built deps, in the order Doc/BUILD-ARM32.md section 3 gives.
# Each is its own process, so each dot-sources arm32-uwp-env.ps1 into a clean shell.
# ---------------------------------------------------------------------------
if (-not $SkipDeps) {
    Step 'pixman-arm32'     'pwsh' @('-NoProfile', '-File', (Join-Path $Port 'build-pixman-arm32.ps1'))   10
    Step 'cairo-arm32'      'pwsh' @('-NoProfile', '-File', (Join-Path $Port 'build-cairo-arm32.ps1'))      40
    Step 'fontconfig-arm32' 'pwsh' @('-NoProfile', '-File', (Join-Path $Port 'build-fontconfig-arm32.ps1')) 15
    Step 'harfbuzz-arm32'   'pwsh' @('-NoProfile', '-File', (Join-Path $Port 'build-harfbuzz-arm32.ps1'))   25
} else { Say 'deps: skipped by switch' 'DarkYellow' }

# ---------------------------------------------------------------------------
# 4. Configure build-arm32-gpu.
# ---------------------------------------------------------------------------
if (-not $SkipConfigure) {
    Step 'configure-arm32-gpu' 'pwsh' @('-NoProfile', '-File', (Join-Path $Port 'configure-gpu-arm32.ps1')) 10
} else { Say 'configure: skipped by switch' 'DarkYellow' }

$BuildDir = Join-Path $Root 'build-arm32-gpu'
if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    Fail 'configure-arm32-gpu' "no build.ninja in $BuildDir -- configure did not produce a usable tree" (Join-Path $LogDir 'configure-arm32-gpu.log')
}

if ($StopBeforeEngine) {
    Say 'deps and configure done; stopping before the engine build as asked' 'Green'
    Set-Content -Path $Verdict -Value @(
        'ARM bootstrap: prerequisites COMPLETE',
        "build.ninja exists in $BuildDir",
        'next: pwsh -File Src\tools\arm-bootstrap.ps1 -SkipVcpkg -SkipSqlite -SkipDeps -SkipConfigure'
    )
    exit 0
}

# ---------------------------------------------------------------------------
# 5. The engine. This is the multi-hour part: WebCore, JavaScriptCore and WTF for
# thumbv7. -j1 is not a preference, it is a memory limit -- single unified-source
# translation units exceed 1 GB and the machine has 4 GB.
# ---------------------------------------------------------------------------
$ninja = @('C:\Program Files\CMake\bin\ninja.exe', 'C:\ProgramData\chocolatey\bin\ninja.exe') |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ninja) {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) { $ninja = $cmd.Source } else { Fail 'engine' 'ninja not found in CMake, chocolatey or PATH' $null }
}
Say "engine: using $ninja"
Step 'engine-arm32-gpu' $ninja @('-C', $BuildDir, '-j1') 600

Say 'ARM bootstrap finished' 'Green'
Set-Content -Path $Verdict -Value @(
    'ARM bootstrap: SUCCESS',
    "engine built in $BuildDir",
    "finished $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    '',
    'next steps:',
    '  pwsh -File Src\port\link-driver-gpu-arm32.ps1     # WebCoreDriver-gpu.lib for ARM',
    '  then build the ARM appx and deploy with Src\tools\deploy-launch.ps1 -Ip <device-ip> -Ver <version>'
)


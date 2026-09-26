# update-local-x64.ps1 -- install the freshly built x64 appx IN PLACE, preserving LocalState.
#
# Apotheosis 2026-09-18: this exists because both older installers REMOVE the package first
# (install-local-x64.ps1 line 30, x64-cycle.ps1 lines 98-101), and Remove-AppxPackage deletes the
# app's local data folder -- LocalState, i.e. settings.ini, the collected logs and any crash dumps.
# Its -KeepState switch does not help: that only decides whether stale log files are deleted AFTER an
# install which has already wiped everything. The in-place route (bump the manifest, then
# Add-AppxPackage without a removal) is what preserved state across 0.1.9.93 through 0.1.9.99.
#
# Old logs are moved aside rather than deleted, because gpuinit-steps.txt is APPENDED to: without
# this, a previous run's markers sit above this run's and read as fresh evidence.
#
# Add-AppxPackage refuses a same-version reinstall with different contents (0x80073CFB), so the
# manifest version must be bumped for every install. The check below makes that failure legible
# instead of leaving a bare HRESULT.
param(
    [string]$Ver = '',        # pin an exact version; default = newest appx under AppPackages
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'

$root = $env:APOTHEOSIS_ROOT
if (-not $root) { $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$pkgRoot = Join-Path $root 'Src\harness\AppPackages\Harness'

if ($Ver) {
    $appx = Join-Path $pkgRoot "Harness_${Ver}_x64_Test\Harness_${Ver}_x64.appx"
    if (-not (Test-Path $appx)) { throw "not found: $appx" }
    $appx = Get-Item $appx
} else {
    $appx = Get-ChildItem -Path $pkgRoot -Recurse -Filter 'Harness_*_x64.appx' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $appx) { throw "no Harness_*_x64.appx under $pkgRoot" }
}
Write-Host ("appx {0} ({1:N1} MB)" -f $appx.Name, ($appx.Length / 1MB))   # .Length, never ls -l

if (-not ($appx.Name -match '^Harness_(?<v>[0-9.]+)_x64\.appx$')) { throw "unexpected appx name: $($appx.Name)" }
$appxVer = $Matches['v']

$local = $null
$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if ($pkg) {
    Write-Host "installed: $($pkg.PackageFullName)"
    if ("$($pkg.Version)" -eq $appxVer) {
        throw "installed version is already $appxVer -- bump <Identity Version> in Src\harness\Package.appxmanifest, rebuild, and retry (Add-AppxPackage refuses 0x80073CFB)"
    }
    $local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
    # Apotheosis: stop the running instance FIRST. Add-AppxPackage over a live process either fails
    # with "package in use" or leaves the old binaries mapped in the running app -- and a test run
    # against the old engine that looks like a test run against the new one is worse than a failed
    # install. Stop before moving logs, so the app's final flush lands in the files being moved.
    $running = Get-Process Harness -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host "stopping running Harness (pid $($running.Id -join ', '))"
        $running | Stop-Process -Force
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-Process Harness -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
        }
        if (Get-Process Harness -ErrorAction SilentlyContinue) { throw "Harness did not exit; refusing to install over it" }
        Start-Sleep -Milliseconds 500      # let LocalState handles close before we move files
    }
    if (Test-Path $local) {
        $aside = Join-Path $local ("prelaunch-" + (Get-Date -Format 'MMdd-HHmm'))
        New-Item -ItemType Directory -Force -Path $aside | Out-Null
        # crash-report.txt is in this list as of 2026-09-24: it was missing, so the *only* preserved
        # copy of a crashed session (RunCrashVerdict writes it, one file, with trunc) was left in
        # LocalState root for the next crashed launch to overwrite. The harness now also rotates
        # every session's log to log-prev-<MMdd-HHmmss>.txt (MainPage.xaml.cpp, LogInit), which is
        # what keeps the 12 newest corpses; this list keeps the in-app viewer's copy in step.
        foreach ($f in 'log.txt', 'gpuinit-steps.txt', 'port-trace.txt', 'stage.txt', 'glyph.log', 'crash-report.txt') {
            Move-Item (Join-Path $local $f) $aside -ErrorAction SilentlyContinue
        }
        Write-Host "previous logs moved to $aside (LocalState itself untouched)"
    }
} else {
    Write-Host "no installed package -- this will be a fresh install"
}

# No Remove-AppxPackage anywhere in this file, deliberately. See the header.
Add-AppxPackage -Path $appx.FullName

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness
Write-Host "installed: $($pkg.PackageFullName)"
$aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
Write-Host "app id: $aid"
if (-not $NoLaunch) {
    Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!$aid"
    Write-Host "launched; localstate=$local"
}

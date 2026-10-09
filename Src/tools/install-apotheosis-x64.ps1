# install-apotheosis-x64.ps1 -- install the freshly built x64 package of the C# app in place.
#
# Apotheosis 2026-10-03. The two installers in this folder (install-local-x64.ps1,
# update-local-x64.ps1) both target EdgeHTMLReborn.Harness, the C++/CX harness. The C# line
# (ME.Apotheosis) had no installer at all, so every iteration either went through Visual
# Studio's deploy or lost the app's LocalState -- and LocalState is where settings.ini,
# history.tsv and the whole autocmd log live, i.e. exactly the state an iteration is trying
# to preserve. This is the same in-place route update-local-x64.ps1 uses, pointed at the
# other package.
#
# Add-AppxPackage refuses a same-version reinstall whose contents differ (0x80073CFB), so the
# manifest version must be bumped for every install; the pre-check below says so in words
# instead of leaving a bare HRESULT.
param(
    [string]$Bundle = '',
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'

$root = $env:APOTHEOSIS_ROOT
if (-not $root) { $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }

if (-not $Bundle) {
    $dir = Join-Path $root 'Src\Apotheosis\AppPackages'
    $Bundle = (Get-ChildItem -Path $dir -Recurse -Filter 'Apotheosis_*_x64_Debug.appxbundle' -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    if (-not $Bundle) { throw "no Apotheosis_*_x64_Debug.appxbundle under $dir" }
}
Write-Host "bundle: $Bundle"

# Version of the package being installed, read out of the bundle name, vs what is installed now.
$newVer = [regex]::Match((Split-Path $Bundle -Leaf), 'Apotheosis_([\d\.]+)_').Groups[1].Value
$cur = Get-AppxPackage -Name 'ME.Apotheosis' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cur) {
    $curVer = $cur.Version.ToString()
    Write-Host "installed: $curVer -> installing: $newVer"
    if ($curVer -eq $newVer) {
        throw "$curVer is already installed; Add-AppxPackage will refuse the same-version reinstall " +
              "(0x80073CFB). Bump <Identity Version> in Package.appxmanifest and rebuild."
    }
    # No Remove-AppxPackage anywhere in this script: it deletes LocalState, i.e. settings and logs.
    Write-Host "in-place update; LocalState is preserved"
} else {
    Write-Host "ME.Apotheosis is not installed; first install"
}

# A running instance holds the package's files open and the deployment fails with 0x80073D02
# ("resources it modifies are currently in use"), naming the app that has to be closed. Stop it
# first; unlike a debugger, killing the app loses nothing -- its state is in LocalState.
$running = Get-Process -Name 'Apotheosis' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host ("stopping {0} running instance(s)" -f @($running).Count)
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

Add-AppxPackage -Path $Bundle
$pkg = Get-AppxPackage -Name 'ME.Apotheosis' | Select-Object -First 1
if (-not $pkg) { throw 'install appeared to succeed but the package is not registered' }
Write-Host "installed: $($pkg.PackageFullName)"

$ls = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
Write-Host "localstate: $ls"

if (-not $NoLaunch) {
    $aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
    Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!$aid"
    Write-Host "launched $aid"
}

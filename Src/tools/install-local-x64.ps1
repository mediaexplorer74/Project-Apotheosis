# Install the freshly built x64 appx locally and launch it.
#
# Apotheosis: this used to hardcode both the repo path and one appx version (0.1.8.25), so every
# version bump silently broke it. The newest Harness_*_x64.appx under AppPackages wins by default;
# -Ver pins an exact one. Paths come from $env:APOTHEOSIS_ROOT (Src\setenv.ps1) like every other
# script here.
param(
    [string]$Ver = '',
    # Keep LocalState across the reinstall (settings, bookmarks, previous logs).
    [switch]$KeepState
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

$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
if ($pkg) { Write-Host "removing $($pkg.PackageFullName)"; Remove-AppxPackage $pkg.PackageFullName }

Add-AppxPackage -Path $appx.FullName
$pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness
Write-Host "installed: $($pkg.PackageFullName)"

$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
if (-not $KeepState) {
    # Stale diagnostics read as fresh evidence, which is worse than having none -- gpuinit-steps.txt
    # in particular is appended to, so a previous run's markers would sit above this run's.
    foreach ($f in 'log.txt', 'shot_ui.bmp', 'gpuinit-steps.txt') {
        Remove-Item (Join-Path $local $f) -ErrorAction SilentlyContinue
    }
}

$aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!$aid"
Write-Host "launched $aid; localstate=$local"

#Requires -Version 7.0
<#
  build-app-accept.ps1 -- build the app package with an acceptance batch staged inside it.

  Why this exists. Device Portal can READ the app's LocalState but every spelling of a write to it
  returns HTTP 500 (measured; see the header of pull-device-logs.ps1). So the x64 acceptance
  harness, which drives the app by rewriting LocalState\autocmd.txt from outside, has no twin on
  the Lumia: a scripted run there can only be started by a file that ships inside the appx.

  This script is that bridge and nothing else. It stages Src\tools\device-accept\device-run.txt as
  Src\Apotheosis\Assets\device-run.txt, bumps the package version (the OS refuses an in-place update
  of the same version: 0x80073CFB), runs the ordinary build for the arch, and removes the staged
  copy in a finally block -- so a product build never contains a batch and the app's startup check
  finds nothing. MainPage.AutoCmd.cs runs the batch once per package version; the marker it writes
  before the first command is LocalState\device-run.done.

  Why bump by default. A repeat of the same list needs a new version anyway (that is what makes
  device-run.done stale), so the bump is the mechanism that lets a run be repeated at all.

  Usage:
    pwsh -File Src\tools\build-app-accept.ps1
    pwsh -File Src\tools\build-app-accept.ps1 -Arch x64
    pwsh -File Src\tools\build-app-accept.ps1 -Batch Src\tools\device-accept\some-other-list.txt

  Then audit and deploy:
    pwsh -File Src\tools\Test-AppxPayload.ps1 -Path <bundle> -ExpectedArch arm
    pwsh -File Src\tools\Wdp-Deploy.ps1 -Appx <bundle> -DeviceIp 192.168.1.35

  This script does NOT deploy and does NOT launch. Deployment is an outward action against a device
  that may be in someone's hand, and it is the maintainer's call to make when it is made.
#>
[CmdletBinding()]
param(
    [ValidateSet('arm', 'x64')]
    [string]$Arch = 'arm',

    # The batch to embed. Defaults to the standing thirteen-site acceptance list.
    [string]$Batch = (Join-Path $PSScriptRoot 'device-accept\device-run.txt'),

    # Build without touching the manifest version. Only useful for a build that will not be
    # installed; the device refuses a same-version update.
    [switch]$NoBump
)

$ErrorActionPreference = 'Stop'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$Proj = Join-Path $Root 'Src\Apotheosis'
$Staged = Join-Path $Proj 'Assets\device-run.txt'

if (-not (Test-Path -LiteralPath $Batch)) {
    Write-Host ("batch not found: {0}" -f $Batch) -ForegroundColor Red
    exit 1
}

# The verb list is the app's own; a typo here would otherwise surface ten minutes into a device run
# as one "unknown verb" line among hundreds. `hist` and `hash` are read-only and need no session;
# every other verb is the app's.
$Known = @('goto', 'scroll', 'swipe', 'scrolln', 'tap', 'taplink', 'links', 'ui', 'lang', 'ua',
           'zoom', 'dtap', 'suggest', 'enter', 'wait', 'hash', 'diag', 'back', 'forward', 'hist')
$bad = @()
$n = 0
foreach ($raw in Get-Content -LiteralPath $Batch) {
    $t = ($raw ?? '').Trim()
    if ($t.Length -eq 0 -or $t.StartsWith('#')) { continue }
    $n++
    $verb = $t.Split(' ')[0].ToLowerInvariant()
    if ($verb -notin $Known) { $bad += ("line {0}: {1}" -f $n, $t) }
}
if ($bad.Count) {
    Write-Host 'the batch uses verbs this app does not have:' -ForegroundColor Red
    $bad | ForEach-Object { Write-Host ("  {0}" -f $_) -ForegroundColor Red }
    exit 1
}

$builder = Join-Path $PSScriptRoot ("build-app-{0}.bat" -f $Arch)
if (-not (Test-Path -LiteralPath $builder)) {
    Write-Host ("no builder for arch '{0}': {1}" -f $Arch, $builder) -ForegroundColor Red
    exit 1
}

Write-Host ("batch:  {0} ({1} commands)" -f $Batch, $n)
Write-Host ("staging to {0}" -f $Staged)

try {
    Copy-Item -LiteralPath $Batch -Destination $Staged -Force

    if (-not $NoBump) {
        Write-Host 'bumping the package version'
        & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'Set-AppVersion.ps1')
        if ($LASTEXITCODE -ne 0) { throw ("Set-AppVersion.ps1 exited {0}" -f $LASTEXITCODE) }
    }

    Write-Host ("building ({0}, Release)" -f $Arch)
    & cmd.exe /c $builder
    if ($LASTEXITCODE -ne 0) { throw ("builder exited {0}" -f $LASTEXITCODE) }
}
finally {
    # The staged file MUST NOT survive this script: an Assets\device-run.txt left in the tree is
    # copied into every later build, x64 included, and would hijack the bench's own runs.
    if (Test-Path -LiteralPath $Staged) {
        Remove-Item -LiteralPath $Staged -Force
        Write-Host 'staged batch removed'
    }
}

$manifest = Join-Path $Proj 'Package.appxmanifest'
$ver = [regex]::Match((Get-Content -LiteralPath $manifest -Raw), 'Version="([0-9.]+)"').Groups[1].Value
# Two different suffixes, measured from the trees the builders actually leave behind:
#   ARM Release: AppPackages\Apotheosis_<ver>_Test\Apotheosis_<ver>_arm.appxbundle
#   x64 Debug:   AppPackages\Apotheosis_<ver>_Debug_Test\Apotheosis_<ver>_x64_Debug.appxbundle
if ($Arch -eq 'arm') { $folderSuffix = '_Test'; $pkgSuffix = 'arm' }
else { $folderSuffix = '_Debug_Test'; $pkgSuffix = 'x64_Debug' }
$bundle = Join-Path $Proj ("AppPackages\Apotheosis_{0}{1}\Apotheosis_{0}_{2}.appxbundle" -f $ver, $folderSuffix, $pkgSuffix)
Write-Host ''
Write-Host ("built: {0}" -f $ver) -ForegroundColor Green
if (Test-Path -LiteralPath $bundle) {
    $mb = [math]::Round((Get-Item -LiteralPath $bundle).Length / 1MB, 1)
    Write-Host ("bundle: {0} ({1} MB)" -f $bundle, $mb)
    Write-Host ''
    Write-Host ("next: pwsh -File Src\tools\Test-AppxPayload.ps1 -Path `"{0}`" -ExpectedArch {1}" -f $bundle, $Arch)
    Write-Host ("      pwsh -File Src\tools\Wdp-Deploy.ps1 -Appx `"{0}`" -DeviceIp 192.168.1.35" -f $bundle)
}
else {
    Write-Host ("bundle not found at the expected path -- check AppPackages under {0}" -f $Proj) -ForegroundColor Yellow
}

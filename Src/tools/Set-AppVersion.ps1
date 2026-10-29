<#
.SYNOPSIS
    Set the package version, and keep the app-list display name in step with it.

.DESCRIPTION
    Two facts force this into one tool rather than two edits.

    First, the OS refuses to install a package whose identity matches an installed one but whose
    contents differ: "Increment the version number of the package to be installed, or remove the old
    package for every user" (0x80073CFB, measured on the Lumia 2026-10-03). So every device iteration
    needs a version bump -- there is no force-update path through Device Portal.

    Second, while developing it is worth being able to tell two installs apart in the phone's app
    list, where several builds of this app may coexist. The name shown there is the literal
    `uap:VisualElements/@DisplayName` in the manifest -- there is no version token MSBuild will
    substitute into it, and the project ships no .resw to localize it through, so it has to be
    written into the file. Doing that by hand is exactly how the name and the version drift apart.

    So: bump the version and stamp the name in a single pass. Run without -Version to increment the
    Revision (the fourth field), which is the field the install gate compares. The Revision is
    capped at 100: bumping 0.2.3.99 gives 0.2.3.100, and bumping 0.2.3.100 rolls over into
    0.2.4.0 instead of growing a five-digit tail like 0.2.3.101 or 0.2.3.19999. The third field is
    incremented and the Revision reset to 0 once it has reached 100.

    The manifest is read and written as raw text so its line endings survive untouched.

.PARAMETER Version
    The version to set, as x.y.z.w. Default: the current Identity version with the Revision bumped
    by one, rolling over at 100 into the Build field (0.2.3.99 -> 0.2.3.100 -> 0.2.4.0).

.PARAMETER Plain
    Set the display name to the bare product name, with no version. Use this for a release build,
    where a version suffix in the app list is noise rather than a diagnostic.

.PARAMETER Manifest
    Path to the manifest. Defaults to the app's own Package.appxmanifest.

.PARAMETER ProductName
    The name to show, before the version suffix. Defaults to Apotheosis.

.EXAMPLE
    pwsh -File Src/tools/Set-AppVersion.ps1
    # Apotheosis 0.2.3.20 -> 0.2.3.21, display name "Apotheosis 0.2.3.21"

.EXAMPLE
    pwsh -File Src/tools/Set-AppVersion.ps1
    # at the cap: Apotheosis 0.2.3.100 -> 0.2.4.0, display name "Apotheosis 0.2.4.0"

.EXAMPLE
    pwsh -File Src/tools/Set-AppVersion.ps1 -Version 1.0.0.0 -Plain
    # release: display name back to "Apotheosis"

.EXAMPLE
    pwsh -File Src/tools/Set-AppVersion.ps1 -WhatIf
    # report what would change, write nothing
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Version,
    [switch]$Plain,
    [string]$Manifest = (Join-Path $PSScriptRoot '..\Apotheosis\Package.appxmanifest'),
    [string]$ProductName = 'Apotheosis'
)

$ErrorActionPreference = 'Stop'

$path = (Resolve-Path -LiteralPath $Manifest).Path
$text = [IO.File]::ReadAllText($path)

$cur = [regex]::Match($text, '<Identity\b[^>]*\bVersion="(\d+)\.(\d+)\.(\d+)\.(\d+)"', 'Singleline')
if (-not $cur.Success) { throw "no <Identity ... Version=x.y.z.w> element in $path" }
$curVer = '{0}.{1}.{2}.{3}' -f $cur.Groups[1].Value, $cur.Groups[2].Value, $cur.Groups[3].Value, $cur.Groups[4].Value

if ($Version) {
    if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "-Version must be x.y.z.w, got '$Version'" }
    $newVer = $Version
} else {
    $build = [int]$cur.Groups[3].Value
    $rev = [int]$cur.Groups[4].Value
    if ($rev -ge 100) {
        # 0.2.3.100 -> 0.2.4.0: keep the Revision a two-digit device-iteration counter.
        $build++
        $rev = 0
    } else {
        $rev++
    }
    $newVer = '{0}.{1}.{2}.{3}' -f $cur.Groups[1].Value, $cur.Groups[2].Value, $build, $rev
}

$newName = if ($Plain) { $ProductName } else { "$ProductName $newVer" }

# Only the two display-name sites are touched. `<PublisherDisplayName>` is an element, not an
# attribute, so it cannot match either pattern; the negative lookbehind makes that explicit.
$out = $text
$out = [regex]::Replace($out, '(<Identity\b[^>]*\bVersion=")\d+\.\d+\.\d+\.\d+(")', "`${1}$newVer`${2}", 'Singleline')
$out = [regex]::Replace($out, '(?<!Publisher)<DisplayName>[^<]*</DisplayName>', "<DisplayName>$newName</DisplayName>")
$out = [regex]::Replace($out, '(?<!Publisher)DisplayName="[^"]*"', "DisplayName=`"$newName`"")

Write-Host "manifest    : $path"
Write-Host "version     : $curVer -> $newVer"
Write-Host "app-list    : `"$newName`""
if ($out -ceq $text) {
    Write-Host "no change (already current)." -ForegroundColor DarkGray
    exit 0
}

if (-not $PSCmdlet.ShouldProcess($path, 'write manifest')) { exit 0 }
[IO.File]::WriteAllText($path, $out)

# Prove the edit landed by re-reading, rather than trusting the replacement above.
$check = [IO.File]::ReadAllText($path)
$v = [regex]::Match($check, '<Identity\b[^>]*\bVersion="([^"]+)"', 'Singleline').Groups[1].Value
$names = @([regex]::Matches($check, '(?<!Publisher)(?:<DisplayName>([^<]*)</DisplayName>|DisplayName="([^"]*)")') |
           ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } })
Write-Host "written     : Version=$v  DisplayName=$($names -join ', ')" -ForegroundColor Green

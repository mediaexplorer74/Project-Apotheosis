# archive-webcorefull.ps1 - rebuild the static WebCore archive the driver links.
#
# WebCoreDriver.dll contains WebCore statically. CMake produces hundreds of object
# files under build-<arch>-gpu\Source\WebCore\CMakeFiles\WebCore.dir, but not the
# single archive the port link step wants. This script regenerates that archive from
# whatever objects are in the build tree, so a fresh machine does not need the
# historical response files from the original developer's disk.
#
# Usage:
#   pwsh -File Src\port\archive-webcorefull.ps1 -Arch x64
#   pwsh -File Src\port\archive-webcorefull.ps1 -Arch arm
#
# Run it AFTER: ninja -C build-x64-gpu WebCore   (or the ARM equivalent).
# Output:
#   build-x64-gpu\lib\WebCoreFull-x64-19041.lib
#   build-arm32-gpu\lib\WebCoreFull-arm32.lib
#
# The archive is large (3+ GB) and lib.exe takes a few minutes. It is not published;
# it is a build artifact.

param(
    [ValidateSet('x64', 'arm')]
    [string]$Arch = 'x64',
    [string]$Root = $(if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }),
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'

$BuildDir = if ($Arch -eq 'x64') { Join-Path $Root 'build-x64-gpu' } else { Join-Path $Root 'build-arm32-gpu' }
$ObjRoot = Join-Path $BuildDir 'Source\WebCore\CMakeFiles\WebCore.dir'
$LibDir = Join-Path $BuildDir 'lib'
$OutName = if ($Arch -eq 'x64') { 'WebCoreFull-x64-19041.lib' } else { 'WebCoreFull-arm32.lib' }
$Out = Join-Path $LibDir $OutName
$Rsp = Join-Path $BuildDir ("WebCoreFull-$Arch.rsp")

if (-not (Test-Path $ObjRoot)) {
    throw "WebCore object directory not found: $ObjRoot`nBuild WebCore first: ninja -C $BuildDir WebCore -j1"
}

# Find the VS toolset. ARM needs the ARM-targeting lib.exe; the x64 lib.exe records
# the wrong machine for ARM objects and produces an archive the ARM linker rejects.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) {
    (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
} else { $null }
if (-not $vs -or -not (Test-Path $vs)) {
    $vs = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community'
}
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvc) { throw "No MSVC toolset found under $vs\VC\Tools\MSVC" }
$LibExe = Join-Path $msvc.FullName ("bin\Hostx64\{0}\lib.exe" -f $(if ($Arch -eq 'x64') { 'x64' } else { 'arm' }))
if (-not (Test-Path $LibExe)) { throw "lib.exe not found: $LibExe" }

$objects = @(Get-ChildItem $ObjRoot -Recurse -File -Filter '*.obj' |
    Where-Object { $_.Name -notmatch 'stubs-sqlite' })
# The historical ARM archive also carries the 24 PAL objects that the ARM CMake tree
# places under Source\WebCore\PAL. x64 links PAL-x64-19041.lib separately instead.
if ($Arch -eq 'arm') {
    $palRoot = Join-Path $BuildDir 'Source\WebCore\PAL\pal\CMakeFiles\PAL.dir'
    if (Test-Path $palRoot) {
        $objects += @(Get-ChildItem $palRoot -Recurse -File -Filter '*.obj')
    }
}
if ($objects.Count -lt 500) {
    throw "Only $($objects.Count) WebCore objects found under $ObjRoot; the build looks incomplete."
}

Write-Host "==> archiving $($objects.Count) WebCore objects for $Arch" -ForegroundColor Cyan
Write-Host "    lib.exe: $LibExe"
Write-Host "    output : $Out"

# lib.exe response file: one quoted absolute member per line. Set-Content with ASCII
# avoids a BOM, which lib.exe would treat as part of the first path.
New-Item -ItemType Directory -Force -Path $LibDir | Out-Null
$objects | ForEach-Object { '"' + $_.FullName + '"' } | Set-Content -Path $Rsp -Encoding ascii
Write-Host "    rsp    : $Rsp ($((Get-Item $Rsp).Length) bytes)"

if ($CheckOnly) {
    Write-Host "==> CheckOnly: would archive $($objects.Count) objects; lib.exe not run." -ForegroundColor Yellow
    return
}

& $LibExe /NOLOGO "/OUT:$Out" "@$Rsp"
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $Out)) { throw "lib.exe reported success but $Out does not exist" }

$size = (Get-Item $Out).Length
Write-Host ("==> {0} ({1:N0} bytes, {2:N2} GB)" -f $Out, $size, ($size / 1GB)) -ForegroundColor Green
Write-Host "    The driver link step consumes this archive; rerun build-driver-dll-arm32.ps1 -Arch $Arch." -ForegroundColor Gray

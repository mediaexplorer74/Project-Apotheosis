# archive-pal.ps1 - rebuild the PAL archive used by the driver link.
#
# x64 CMake produces PAL objects but not the name the driver expects
# (build-x64-gpu\lib\PAL-x64-19041.lib). ARM already calls its archive PAL.lib,
# and this script can regenerate that too. Run after WebCore has been built.
#
# Usage:
#   pwsh -File Src\port\archive-pal.ps1 -Arch x64
#   pwsh -File Src\port\archive-pal.ps1 -Arch arm
#
# Output:
#   build-x64-gpu\lib\PAL-x64-19041.lib
#   build-arm32-gpu\lib\PAL.lib

param(
    [ValidateSet('x64', 'arm')]
    [string]$Arch = 'x64',
    [string]$Root = $(if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }),
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'

$BuildDir = if ($Arch -eq 'x64') { Join-Path $Root 'build-x64-gpu' } else { Join-Path $Root 'build-arm32-gpu' }
$ObjRoot = Join-Path $BuildDir 'Source\WebCore\PAL\pal\CMakeFiles\PAL.dir'
$LibDir = Join-Path $BuildDir 'lib'
$Out = if ($Arch -eq 'x64') { Join-Path $LibDir 'PAL-x64-19041.lib' } else { Join-Path $LibDir 'PAL.lib' }
$Rsp = Join-Path $BuildDir ("PAL-$Arch.rsp")

if (-not (Test-Path $ObjRoot)) {
    throw "PAL object directory not found: $ObjRoot`nBuild WebCore first: ninja -C $BuildDir WebCore -j1"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) {
    (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
} else { $null }
if (-not $vs) { $vs = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvc) { throw "No MSVC toolset found under $vs\VC\Tools\MSVC" }
$LibExe = Join-Path $msvc.FullName ("bin\Hostx64\{0}\lib.exe" -f $(if ($Arch -eq 'x64') { 'x64' } else { 'arm' }))
if (-not (Test-Path $LibExe)) { throw "lib.exe not found: $LibExe" }

$objects = @(Get-ChildItem $ObjRoot -Recurse -File -Filter '*.obj')
if ($objects.Count -lt 20) {
    throw "Only $($objects.Count) PAL objects found under $ObjRoot; the build looks incomplete."
}

Write-Host "==> archiving $($objects.Count) PAL objects for $Arch" -ForegroundColor Cyan
Write-Host "    output: $Out"
New-Item -ItemType Directory -Force -Path $LibDir | Out-Null
$objects | ForEach-Object { '"' + $_.FullName + '"' } | Set-Content -Path $Rsp -Encoding ascii
Write-Host "    rsp:    $Rsp"

if ($CheckOnly) {
    Write-Host "==> CheckOnly: lib.exe not run." -ForegroundColor Yellow
    return
}

& $LibExe /NOLOGO "/OUT:$Out" "@$Rsp"
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed with exit code $LASTEXITCODE" }
$size = (Get-Item $Out).Length
Write-Host ("==> {0} ({1:N0} bytes)" -f $Out, $size) -ForegroundColor Green

# build-psl-stub.ps1 - compile the four-symbol libpsl stub used by the x64 link.
#
# The UWP curl build has no libpsl, but a WebCore object references
# psl_builtin/psl_free/psl_is_public_suffix2/psl_registrable_domain. This
# archives Src\port\psl-stub.cpp into build-<arch>-gpu\libs\psl\psl.lib.
#
# Usage:
#   pwsh -File Src\port\build-psl-stub.ps1 -Arch x64
#   pwsh -File Src\port\build-psl-stub.ps1 -Arch arm
#
# Only x64 currently lists psl.lib on the driver link line; building the ARM
# archive is harmless and keeps the script symmetric.

param(
    [ValidateSet('x64', 'arm')]
    [string]$Arch = 'x64',
    [string]$Root = $(if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

if ($Arch -eq 'x64') {
    . (Join-Path $PSScriptRoot 'x64-uwp-env.ps1') | Out-Null
} else {
    . (Join-Path $PSScriptRoot 'arm32-uwp-env.ps1') | Out-Null
}

$clang = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'
if (-not (Test-Path $clang)) { $clang = (Get-Command clang-cl -ErrorAction SilentlyContinue).Source }
if (-not $clang) { throw 'clang-cl.exe not found. Install LLVM and rerun.' }

$BuildDir = if ($Arch -eq 'x64') { Join-Path $Root 'build-x64-gpu' } else { Join-Path $Root 'build-arm32-gpu' }
$OutDir = Join-Path $BuildDir 'libs\psl'
$Obj = Join-Path $OutDir 'psl_stub.obj'
$Lib = Join-Path $OutDir 'psl.lib'
$Src = Join-Path $PSScriptRoot 'psl-stub.cpp'

if ($Clean) { Remove-Item $Obj, $Lib -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "==> compiling psl-stub.cpp for $Arch" -ForegroundColor Cyan
& $clang /nologo /c /MD /O2 /EHsc /std:c++17 /Fo:$Obj -- $Src
if ($LASTEXITCODE -ne 0) { throw "clang-cl failed with exit code $LASTEXITCODE" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) {
    (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
} else { Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvc) { throw "No MSVC toolset found under $vs\VC\Tools\MSVC" }
$libExe = Join-Path $msvc.FullName ('bin\Hostx64\' + $(if ($Arch -eq 'x64') { 'x64' } else { 'arm' }) + '\lib.exe')
if (-not (Test-Path $libExe)) { throw "lib.exe not found: $libExe" }

& $libExe /nologo /OUT:$Lib $Obj
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed with exit code $LASTEXITCODE" }
Write-Host ("==> {0} ({1:N0} bytes)" -f $Lib, (Get-Item $Lib).Length) -ForegroundColor Green

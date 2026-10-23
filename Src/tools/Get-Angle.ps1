# Get-Angle.ps1 - fetch the UWP ANGLE binaries used by the x64 and ARM driver builds.
#
# ANGLE is not vendored in this repository (the DLLs/LIBs are ~2.5 MB per arch and are
# re-downloadable). Microsoft publishes the UWP build as the NuGet package
# ANGLE.WindowsStore; 2.1.13 is the last version and the one the project was built with.
#
# Usage:
#   pwsh -File Src\tools\Get-Angle.ps1
#   pwsh -File Src\tools\Get-Angle.ps1 -Version 2.1.13
#
# Result:
#   Src\angle\x64\{libEGL,libGLESv2}.{dll,lib}
#   Src\angle\arm\{libEGL,libGLESv2}.{dll,lib}     (NuGet calls the folder ARM)
#   Src\angle\include\angle_windowsstore.h          (only if missing)

param(
    [string]$Version = '2.1.13',
    [string]$Root = $(if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }),
    [switch]$WithHeaders
)

$ErrorActionPreference = 'Stop'

$nupkg = Join-Path ([IO.Path]::GetTempPath()) "angle.windowsstore.$Version.nupkg"
$extract = Join-Path ([IO.Path]::GetTempPath()) "angle-windowsstore-$Version"
$url = "https://api.nuget.org/v3-flatcontainer/angle.windowsstore/$Version/angle.windowsstore.$Version.nupkg"

Write-Host "==> downloading ANGLE.WindowsStore $Version" -ForegroundColor Cyan
Write-Host "    $url"
Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing
Write-Host ("    {0:N2} MB" -f ((Get-Item $nupkg).Length / 1MB))

Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -LiteralPath $nupkg -DestinationPath $extract -Force

$map = @{ x64 = 'x64'; arm = 'ARM' }
foreach ($arch in $map.Keys) {
    $src = Join-Path $extract ("bin\UAP\{0}" -f $map[$arch])
    if (-not (Test-Path $src)) { throw "NuGet package has no $($map[$arch]) payload: $src" }
    $dst = Join-Path $Root "Src\angle\$arch"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    foreach ($name in 'libEGL.dll', 'libEGL.lib', 'libGLESv2.dll', 'libGLESv2.lib') {
        $from = Join-Path $src $name
        if (-not (Test-Path $from)) { throw "missing $from" }
        Copy-Item $from (Join-Path $dst $name) -Force
    }
    Write-Host "==> $arch -> $dst" -ForegroundColor Green
    Get-ChildItem $dst -File | Where-Object { $_.Name -match '^lib(EGL|GLESv2)\.(dll|lib)$' } |
        Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } } |
        Format-Table -AutoSize | Out-String | Write-Host
}

$includeSrc = Join-Path $extract 'Include'
$includeDst = Join-Path $Root 'Src\angle\include'
New-Item -ItemType Directory -Force -Path $includeDst | Out-Null
if ($WithHeaders) {
    Copy-Item (Join-Path $includeSrc '*') $includeDst -Recurse -Force
    Write-Host "==> ANGLE headers copied to $includeDst" -ForegroundColor Green
} else {
    # The driver only needs this one; WebKit carries its own EGL/GLES headers.
    $one = Join-Path $includeSrc 'angle_windowsstore.h'
    if (Test-Path $one) { Copy-Item $one $includeDst -Force }
    Write-Host "==> angle_windowsstore.h ensured (use -WithHeaders for the full set)" -ForegroundColor Green
}

Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Done. ANGLE binaries are ignored by git; keep them out of commits." -ForegroundColor Cyan

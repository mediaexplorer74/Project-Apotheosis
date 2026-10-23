# Stage-NativeDeps.ps1 - fill Src\Apotheosis\native\<arch> with the DLLs the driver imports.
#
# A fresh clone has no native payload (DLLs are deliberately not committed). This script
# reads the import table of WebCoreDriver.dll, copies each dependency from the place it
# belongs (vcpkg, the WebKit build tree, ANGLE, the MSVC redistributable), then walks the
# imports of the copied DLLs so transitive dependencies (brotli, zlib, ICU data, ...) are
# not missed.
#
# Usage:
#   pwsh -File Src\tools\Stage-NativeDeps.ps1 -Arch x64
#   pwsh -File Src\tools\Stage-NativeDeps.ps1 -Arch arm
#   pwsh -File Src\tools\Stage-NativeDeps.ps1 -Arch x64 -DryRun
#
# Prerequisites: the driver is already built/staged; vcpkg triplet installed; ANGLE
# fetched by Get-Angle.ps1; WebKit build tree has bin\JavaScriptCore.dll.

param(
    [ValidateSet('x64', 'arm')]
    [string]$Arch = 'x64',
    [string]$Root = $(if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }),
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }),
    [string]$Driver,
    [string]$JavaScriptCore,
    [switch]$KeepExistingJsc,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$BuildDir = if ($Arch -eq 'x64') { Join-Path $Root 'build-x64-gpu' } else { Join-Path $Root 'build-arm32-gpu' }
$Triplet = if ($Arch -eq 'x64') { 'x64-uwp' } else { 'arm-uwp' }
$NativeDir = Join-Path $Root "Src\Apotheosis\native\$Arch"
$VcpkgBin = Join-Path $VcpkgRoot "installed\$Triplet\bin"
$BuildBin = Join-Path $BuildDir 'bin'
$AngleDir = Join-Path $Root "Src\angle\$Arch"
$JscSource = if ($JavaScriptCore) { $JavaScriptCore } else { Join-Path $BuildBin 'JavaScriptCore.dll' }
if (-not (Test-Path $JscSource) -and -not $KeepExistingJsc) {
    throw "JavaScriptCore.dll not found: $JscSource`nBuild it first: ninja -C $BuildDir JavaScriptCore"
}
if ($JavaScriptCore -and -not (Test-Path $JavaScriptCore)) {
    throw "JavaScriptCore.dll not found at -JavaScriptCore path: $JavaScriptCore"
}

if (-not $Driver) {
    $Driver = Join-Path $NativeDir 'WebCoreDriver.dll'
    if (-not (Test-Path $Driver)) {
        $built = Join-Path $Root ("Src\port\WebCoreDriver-{0}.dll" -f $(if ($Arch -eq 'x64') { 'x64' } else { 'arm' }))
        if (Test-Path $built) {
            New-Item -ItemType Directory -Force -Path $NativeDir | Out-Null
            Copy-Item $built $Driver -Force
            Write-Host "==> staged driver from $built" -ForegroundColor Green
        }
    }
}
if (-not (Test-Path $Driver)) {
    throw "Driver not found: $Driver`nBuild it first: pwsh Src\port\build-driver-dll-arm32.ps1 -Arch $Arch"
}
if (-not (Test-Path $VcpkgBin)) {
    throw "vcpkg bin directory not found: $VcpkgBin`nInstall the $Triplet packages first (see Doc\DEVELOPER-SETUP.md)."
}

$llvm = (Get-Command llvm-readobj -ErrorAction SilentlyContinue).Source
if (-not $llvm) {
    $candidate = Join-Path $env:ProgramFiles 'LLVM\bin\llvm-readobj.exe'
    if (Test-Path $candidate) { $llvm = $candidate }
}
if (-not $llvm) { throw "llvm-readobj.exe not found. Install LLVM (winget install LLVM.LLVM)." }

$redist = $null
if ($Arch -eq 'x64') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = if (Test-Path $vswhere) {
        (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
    } else { $null }
    if (-not $vs) { $vs = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community' }
    $redistRoot = Join-Path $vs 'VC\Redist\MSVC'
    if (Test-Path $redistRoot) {
        $redist = Get-ChildItem $redistRoot -Directory |
            Where-Object { $_.Name -match '^\d' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
    }
}
if ($Arch -eq 'x64' -and -not $redist) {
    Write-Host "!! x64 CRT redistributable directory not found; MSVCP140/VCRUNTIME140 will be reported missing." -ForegroundColor Yellow
}

function Get-PeImports([string]$Path) {
    $names = & $llvm --coff-imports $Path 2>$null | Select-String '^\s*Name:\s*(.+?)\s*$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
    return @($names | Sort-Object -Unique)
}

$systemPattern = '^(kernel32|kernelbase|ntdll|ws2_32|wsock32|mswsock|bcrypt|ncrypt|crypt32|advapi32|user32|gdi32|ole32|oleaut32|shell32|shlwapi|shcore|dwmapi|d3d11|dxgi|d2d1|dwrite|windowscodecs|propsys|oleacc|winmm|wldap32|secur32|iphlpapi|dnsapi|normaliz|uxtheme|combase|rpcrt4|version|setupapi|cfgmgr32|powrprof|pdh|psapi|dbghelp|imm32|msimg32|usp10|gdiplus|wintrust|urlmon|wininet|winhttp|netapi32|userenv|profapi|sechost|sspicli|msvcrt|ucrtbase)\.dll$'

$queue = [System.Collections.Generic.Queue[string]]::new()
$seen = @{}
$missing = @()
$copied = @()
foreach ($name in (Get-PeImports $Driver)) { $queue.Enqueue($name) }

function Resolve-Dependency([string]$Name) {
    $n = $Name.ToLowerInvariant()
    if ($n.StartsWith('api-ms-win-')) { return @{ Kind = 'system' } }
    if ($n -match $systemPattern) { return @{ Kind = 'system' } }
    # VCLibs framework package supplies the *_APP.dll UWP CRT; D3DCompiler_47 is OS.
    # Both are framework/system on x64 and ARM and must never be staged app-local.
    if ($n -match '_app\.dll$') { return @{ Kind = 'system' } }
    if ($n -match '^d3dcompiler_\d+\.dll$') { return @{ Kind = 'system' } }
    if ($n -eq 'javascriptcore.dll') {
        if ($KeepExistingJsc) {
            $existing = Join-Path $NativeDir 'JavaScriptCore.dll'
            if (Test-Path $existing) { return @{ Kind = 'keep'; Path = $existing; Why = 'existing native (KeepExistingJsc)' } }
        }
        if (Test-Path $JscSource) { return @{ Kind = 'copy'; Path = $JscSource; Why = 'WebKit build' } }
        return @{ Kind = 'missing'; Why = "build it: ninja -C $BuildDir JavaScriptCore" }
    }
    if ($n -eq 'libegl.dll' -or $n -eq 'libglesv2.dll') {
        $p = Join-Path $AngleDir $Name
        if (Test-Path $p) { return @{ Kind = 'copy'; Path = $p; Why = 'ANGLE' } }
        return @{ Kind = 'missing'; Why = 'run Src\tools\Get-Angle.ps1' }
    }
    if ($n -match '^(msvcp140|vcruntime140).*\.dll$') {
        if ($Arch -eq 'arm') { return @{ Kind = 'system' } }
        if ($redist) {
            $p = Get-ChildItem $redist -File | Where-Object { $_.Name.ToLowerInvariant() -eq $n } | Select-Object -First 1
            if ($p) { return @{ Kind = 'copy'; Path = $p.FullName; Why = 'MSVC redist' } }
        }
        return @{ Kind = 'missing'; Why = 'MSVC x64 redistributable' }
    }
    $p = Get-ChildItem $VcpkgBin -File | Where-Object { $_.Name.ToLowerInvariant() -eq $n } | Select-Object -First 1
    if ($p) { return @{ Kind = 'copy'; Path = $p.FullName; Why = "vcpkg $Triplet" } }
    return @{ Kind = 'missing'; Why = "vcpkg $Triplet bin" }
}

while ($queue.Count -gt 0) {
    $name = $queue.Dequeue()
    $key = $name.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true

    $r = Resolve-Dependency $name
    if ($r.Kind -eq 'system') { continue }
    if ($r.Kind -eq 'missing') {
        $missing += "$name ($($r.Why))"
        continue
    }
    if ($r.Kind -eq 'keep') {
        Write-Host ("  {0,-24} =  {1}" -f $name, $r.Why)
        $copied += $name
        foreach ($dep in (Get-PeImports $r.Path)) { $queue.Enqueue($dep) }
        continue
    }

    $dst = Join-Path $NativeDir $name
    if ($DryRun) {
        Write-Host ("  would copy {0,-24} <- {1}" -f $name, $r.Why)
    } else {
        New-Item -ItemType Directory -Force -Path $NativeDir | Out-Null
        Copy-Item $r.Path $dst -Force
        Write-Host ("  {0,-24} <- {1}" -f $name, $r.Why)
    }
    $copied += $name

    # ICU's data DLL is not in any import table; it is opened by name at runtime.
    if ($name -match '^icu(in|uc)\d+\.dll$') {
        $data = Get-ChildItem $VcpkgBin -File -Filter 'icudt*.dll' | Select-Object -First 1
        if ($data -and -not $seen.ContainsKey($data.Name.ToLowerInvariant())) {
            $queue.Enqueue($data.Name)
        }
    }

    if ($DryRun) {
        # Walk the source binary too, so a dry run reports transitive gaps instead of
        # only the driver's direct imports.
        foreach ($dep in (Get-PeImports $r.Path)) { $queue.Enqueue($dep) }
    } else {
        foreach ($dep in (Get-PeImports $dst)) { $queue.Enqueue($dep) }
    }
}

Write-Host ""
if ($missing.Count) {
    Write-Host "MISSING ($($missing.Count)):" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    throw "Could not stage every dependency. Fix the entries above and rerun."
}

$verb = if ($DryRun) { 'Would stage' } else { 'Staged' }
Write-Host ("{0} {1} DLL(s) for {2} into {3}" -f $verb, $copied.Count, $Arch, $NativeDir) -ForegroundColor Green
Write-Host "The WebCoreDriver.dll itself is staged by build-driver-dll-arm32.ps1." -ForegroundColor Gray

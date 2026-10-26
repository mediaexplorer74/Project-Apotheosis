<#
.SYNOPSIS
    Audit a built Apotheosis package (.appx or .appxbundle) before it is deployed.

.DESCRIPTION
    Deployment failures on a device are expensive to diagnose, so check the artifact where it is
    cheap. This script opens the package, unwraps the bundle if needed, and answers four questions
    that have each caused, or would have caused, a failure:

      1. Is every binary the architecture the package claims? An x64 DLL inside an ARM package
         fails only when the loader reaches it, as a LoadPackagedLibrary refusal with no useful
         message. Names do not tell you; PE headers do.
      2. Are the native engine DLLs at the package ROOT? The DllImport declarations in
         WebCoreDriver.cs use bare filenames and an AppContainer has no SetDllDirectory, so a
         native\ARM\ subdirectory makes every P/Invoke fail with 0x8007007E.
      3. Is resources.pri present? A package whose .resw files were staged as Content instead of
         PRIResource carries no resources.pri, and every x:Uid in the XAML resolves to an empty
         string -- which is how the whole chrome once ended up with blank labels while the web page
         rendered fine.
      4. Does the driver DLL in the package carry every export the .def promises? A function missing
         from the .def fails as EntryPointNotFoundException at runtime, not at link time, so a stale
         artifact passes the build and dies at startup.

.PARAMETER Path
    Path to a .appx or .appxbundle. A bundle is unwrapped and its main architecture package chosen.

.PARAMETER ExpectedArch
    arm, x64 or x86. Defaults to arm. Every binary must match, or the script fails.

.PARAMETER Keep
    Leave the extracted package on disk and print its location.

.EXAMPLE
    pwsh -File Src/tools/Test-AppxPayload.ps1 -Path Src/Apotheosis/AppPackages/Apotheosis_0.2.3.19_Test/Apotheosis_0.2.3.19_arm.appxbundle
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [ValidateSet('arm', 'x64', 'x86')][string]$ExpectedArch = 'arm',
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

# PE Machine field -> name. ARMNT (0x1C4) is what Windows 10 Mobile runs; 0x1C0 and 0x1C2 are the
# older ARM encodings and should not appear in a modern UWP package.
$machineNames = @{
    0x14c = 'x86'; 0x8664 = 'x64'; 0xaa64 = 'arm64'
    0x1c0 = 'arm'; 0x1c2 = 'thumb'; 0x1c4 = 'armnt'
}
$archToMachines = @{
    arm = @('arm', 'armnt', 'thumb'); x64 = @('x64'); x86 = @('x86')
}

function Get-PeMachine {
    param([string]$File)
    $fs = [System.IO.File]::OpenRead($File)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        $fs.Position = $peOff + 4
        return $br.ReadUInt16()
    } finally { $fs.Dispose() }
}

function Expand-Package([string]$Archive, [string]$Dest) {
    if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Dest)
}

$source = (Resolve-Path $Path).Path
if (-not (Test-Path $source)) { throw "not found: $source" }

$work = Join-Path $env:TEMP ("apx-audit-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$failures = New-Object System.Collections.Generic.List[string]

try {
    Write-Host "Package : $source"
    Write-Host "Expected: $ExpectedArch"
    Write-Host ""

    $main = $source
    if ($source -like '*.appxbundle') {
        $unwrapped = Join-Path $work 'bundle'
        Expand-Package $source $unwrapped

        # A bundle holds one .appx per architecture plus the resource packages. Pick the one whose
        # manifest declares the expected ProcessorArchitecture; the rest are languages and scales.
        $candidates = @(Get-ChildItem $unwrapped -Filter '*.appx' |
            Where-Object { $_.Name -notmatch '_language-|_scale-' })
        if ($candidates.Count -eq 0) { throw "no architecture package inside the bundle" }

        $main = $null
        foreach ($c in $candidates) {
            $probe = Join-Path $work 'probe'
            Expand-Package $c.FullName $probe
            $mf = Join-Path $probe 'AppxManifest.xml'
            if (Test-Path $mf) {
                $raw = Get-Content $mf -Raw
                if ($raw -match "ProcessorArchitecture=`"$ExpectedArch`"") { $main = $c.FullName; break }
            }
            if ($candidates.Count -eq 1) { $main = $c.FullName; break }
        }
        if (-not $main) {
            $names = ($candidates | ForEach-Object { $_.Name }) -join ', '
            throw "no package in the bundle declares ProcessorArchitecture=$ExpectedArch (found: $names)"
        }
        Write-Host "Bundle  : $($candidates.Count) architecture package(s); using $([IO.Path]::GetFileName($main))"
    }
    else {
        Write-Host "Bundle  : none (plain .appx)"
    }

    $inner = Join-Path $work 'inner'
    Expand-Package $main $inner

    # --- 1. architecture of every binary -------------------------------------------------------
    Write-Host ""
    Write-Host "Architecture (every .dll/.exe/.winmd must be $ExpectedArch):"
    $allowed = $archToMachines[$ExpectedArch]
    $bad = @()
    Get-ChildItem $inner -Recurse -File |
        Where-Object { $_.Extension -in '.dll', '.exe', '.winmd' } |
        Sort-Object Name | ForEach-Object {
            $m = Get-PeMachine $_.FullName
            $name = if ($machineNames.ContainsKey([int]$m)) { $machineNames[[int]$m] } else { '0x{0:X}' -f $m }
            if ($allowed -notcontains $name) {
                $bad += $_.Name
                Write-Host ("  {0,-28} {1,-8}  <-- MISMATCH" -f $_.Name, $name) -ForegroundColor Red
            }
        }
    $count = @(Get-ChildItem $inner -Recurse -File |
        Where-Object { $_.Extension -in '.dll', '.exe', '.winmd' }).Count
    if ($bad.Count -eq 0) {
        Write-Host ("  {0} binaries, all {1}" -f $count, $ExpectedArch) -ForegroundColor Green
    } else {
        $failures.Add("$($bad.Count) binary/binaries are not $ExpectedArch : $($bad -join ', ')")
    }

    # --- 2. payload placement ------------------------------------------------------------------
    Write-Host ""
    Write-Host "Native payload at the package root:"
    $driver = Join-Path $inner 'WebCoreDriver.dll'
    if (Test-Path $driver) {
        $inv = [Globalization.CultureInfo]::InvariantCulture
        $mb = [math]::Round((Get-Item $driver).Length / 1MB, 1).ToString('0.0', $inv)
        Write-Host ("  WebCoreDriver.dll present ({0} MB)" -f $mb) -ForegroundColor Green
    } else {
        $stray = @(Get-ChildItem $inner -Recurse -Filter 'WebCoreDriver.dll' |
            Where-Object { $_.DirectoryName -ne $inner })
        $where = if ($stray.Count -gt 0) { " -- found under $($stray[0].DirectoryName), which DllImport cannot resolve" } else { "" }
        Write-Host "  WebCoreDriver.dll MISSING from the root$where" -ForegroundColor Red
        $failures.Add("WebCoreDriver.dll is not at the package root$where")
    }
    if (Test-Path (Join-Path $inner 'WebCore.dll')) {
        Write-Host "  WebCore.dll present -- it should be excluded: the driver links WebCore statically" -ForegroundColor Yellow
        $failures.Add("WebCore.dll is in the package; it is dead weight (~33 MB) since the static link")
    } else {
        Write-Host "  WebCore.dll absent, as intended (static link)" -ForegroundColor Green
    }

    # --- 3. resource PRI -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "Resources:"
    if (Test-Path (Join-Path $inner 'resources.pri')) {
        # Invariant formatting: the default under a ru-RU host inserts a non-breaking space as the
        # group separator, which renders as mojibake in a console that is not UTF-8.
        $priLen = (Get-Item (Join-Path $inner 'resources.pri')).Length.ToString(
            'N0', [Globalization.CultureInfo]::InvariantCulture)
        Write-Host "  resources.pri present ($priLen bytes)" -ForegroundColor Green
    } else {
        Write-Host "  resources.pri MISSING -- every x:Uid in the XAML will resolve to an empty string" -ForegroundColor Red
        $failures.Add("resources.pri is missing; the chrome labels will be blank")
    }

    # --- 4. native exports ---------------------------------------------------------------------
    Write-Host ""
    Write-Host "Driver exports:"
    $readobj = @(
        "$env:ProgramFiles\LLVM\bin\llvm-readobj.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\llvm-readobj.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $readobj) {
        Write-Host "  llvm-readobj not found -- skipping the export check" -ForegroundColor Yellow
    }
    elseif (-not (Test-Path $driver)) {
        Write-Host "  no driver in the package -- skipping the export check" -ForegroundColor Yellow
    }
    else {
        $names = & $readobj --coff-exports $driver 2>$null |
            Select-String -Pattern 'Name: (WebCore[A-Za-z0-9_]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
        Write-Host ("  {0} WebCore* exports" -f $names.Count)

        # The .def beside the sources is the contract; the payload must not fall behind it.
        $def = Join-Path $PSScriptRoot '..\port\WebCoreDriver.exports.def'
        if (Test-Path $def) {
            $declared = Select-String -Path $def -Pattern '^\s+(WebCore[A-Za-z0-9_]+)\s*$' |
                ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
            $missing = @($declared | Where-Object { $names -notcontains $_ })
            if ($missing.Count -eq 0) {
                Write-Host ("  all {0} declared symbols are exported" -f $declared.Count) -ForegroundColor Green
            } else {
                Write-Host ("  MISSING from the payload: {0}" -f ($missing -join ', ')) -ForegroundColor Red
                Write-Host "  (a symbol absent from the .def fails as EntryPointNotFoundException at run time)" -ForegroundColor Red
                $failures.Add("driver is missing exports: $($missing -join ', ')")
            }
        } else {
            Write-Host "  .def not found at $def -- cannot cross-check" -ForegroundColor Yellow
        }
    }

    # --- 5. declared capabilities --------------------------------------------------------------
    #
    # Apotheosis 2026-10-25: `internetClient` alone does NOT reach a private address. Measured on
    # x64: `goto http://192.168.1.34:8765/` produced `curlcode=28 Connection timed out` and the
    # server's log held no request, exactly as Doc/Archive/TOPLEVEL-FETCH-BUDGET.md recorded for an
    # earlier probe. Without privateNetworkClientServer a device run against the LAN harness dies on
    # its first navigation, and the diag that says why is 40 lines deep in a log pulled after the
    # fact -- so say it here, where the artifact is being read anyway.
    Write-Host ""
    Write-Host "Capabilities:"
    $mfPath = Join-Path $inner 'AppxManifest.xml'
    if (Test-Path $mfPath) {
        $caps = [regex]::Matches((Get-Content $mfPath -Raw), '<(?:uap:)?Capability\s+Name="([^"]+)"') |
            ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
        Write-Host ("  {0}" -f ($caps -join ', '))
        if ($caps -notcontains 'privateNetworkClientServer') {
            Write-Host "  privateNetworkClientServer ABSENT: private/LAN addresses are unreachable" -ForegroundColor Yellow
            Write-Host "  (a device run against the 192.168.x.x harness will time out on the first goto)" -ForegroundColor Yellow
        }
    }
    else {
        Write-Host "  AppxManifest.xml not found in the payload" -ForegroundColor Yellow
    }

    # --- verdict -------------------------------------------------------------------------------
    Write-Host ""
    if ($failures.Count -eq 0) {
        Write-Host "AUDIT PASSED" -ForegroundColor Green
        exit 0
    }
    Write-Host "AUDIT FAILED:" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}
finally {
    if ($Keep) {
        Write-Host ""
        Write-Host "Extracted: $work"
    } else {
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

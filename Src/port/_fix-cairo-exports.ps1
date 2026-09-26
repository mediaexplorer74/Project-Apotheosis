$ErrorActionPreference = 'Stop'
$dll = 'C:\Users\media\source\repos\Vibe\Apotheosis\build-x64-gpu\bin\WebCore.dll'
$lib = 'C:\Users\media\source\repos\Vibe\Apotheosis\build-x64-gpu\libs\cairo-complete-x64-uwp.lib'
$def = 'C:\Users\media\source\repos\Vibe\Apotheosis\Src\port\webcore-exports.def'

$nmLib = & 'C:\Program Files\LLVM\bin\llvm-nm.exe' $lib 2>&1
$needed = $nmLib | Select-String 'U __imp_cairo_' | ForEach-Object {
    'cairo_' + (($_.Line.Trim()) -split '\s+')[-1].Substring('__imp_cairo_'.Length)
} | Sort-Object -Unique

$out = & 'C:\Program Files\LLVM\bin\llvm-readobj.exe' --coff-exports $dll 2>&1
$exported = @()
foreach ($ln in ($out -join "`n" -split "`n")) {
    if ($ln -match '^\s*Name: (cairo_\w+)\s*$') { $exported += $Matches[1] }
}
$exported = $exported | Sort-Object -Unique

$missing = $needed | Where-Object { $_ -notin $exported }
Write-Host ("needed: {0}, exported: {1}, MISSING: {2}" -f $needed.Count, $exported.Count, $missing.Count)

# read current def
$defLines = Get-Content $def
$defNames = $defLines | Where-Object { $_ -match '^\s*cairo_' } | ForEach-Object { $_.Trim() }
$toAdd = $missing | Where-Object { $_ -notin $defNames }
Write-Host ("to add to def: {0}" -f $toAdd.Count)
$toAdd -join ', '

# append after the last cairo line (keep the file structure)
$lastCairoIdx = 0
for ($i = 0; $i -lt $defLines.Count; $i++) { if ($defLines[$i] -match '^\s*cairo_') { $lastCairoIdx = $i } }
$newLines = @()
for ($i = 0; $i -lt $defLines.Count; $i++) {
    $newLines += $defLines[$i]
    if ($i -eq $lastCairoIdx) { foreach ($a in $toAdd) { $newLines += $a } }
}
Set-Content -Path $def -Value $newLines -Encoding ascii
Write-Host 'def updated'

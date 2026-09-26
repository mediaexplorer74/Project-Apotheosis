# make-icu75-libs.ps1 — generate ICU-75 import libs from preserved working x64 DLLs.
# The prebuilt engine (WebCore.dll/JavaScriptCore.dll) imports icuuc75.dll/icuin75.dll
# (ICU 75). vcpkg's icu port builds ICU 78 (icuuc78.dll) which would load a SECOND ICU
# into the same process -> conflict. So we re-create icuuc.lib/icuin.lib/icudt.lib that
# bind to the ICU-75 DLLs preserved in the working AppPackages unpack.
$ErrorActionPreference = 'Stop'
$db  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe'
$lib = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\lib.exe'
$src = 'C:\Users\media\source\repos\Vibe\Apotheosis\Src\harness\AppPackages\Harness\Harness_0.1.8.18_x64_Unpacked'
$out = 'C:\icu-x64-uwp\lib'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$tmp = 'C:\Users\media\AppData\Local\Temp\opencode\icu75'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

foreach ($pair in @(@('icuuc75','icuuc.lib'), @('icuin75','icuin.lib'), @('icudt75','icudt.lib'))) {
    $dllName = $pair[0]; $libOut = $pair[1]
    $dll = "$src\$dllName.dll"
    if (-not (Test-Path $dll)) { Write-Host "MISSING $dll" -ForegroundColor Red; continue }

    $exports = & $db /EXPORTS $dll 2>&1
    $def = "$tmp\$dllName.def"
    $head = "LIBRARY `"$dllName.dll`"`r`nEXPORTS`r`n"
    $names = @()
    foreach ($ln in $exports) {
        # row: "ordinal hint RVA name"
        if ($ln -match '^\s+(\d+)\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$') {
            $ord = $matches[1]; $nm = $matches[2]
            $names += ("    {0} @{1}" -f $nm, $ord)
        }
    }
    $body = $head + ($names -join "`r`n")
    Set-Content -Path $def -Value $body -Encoding ASCII
    Write-Host "==> building import lib for $dllName ($($names.Count) exports)" -ForegroundColor Cyan
    & $lib /nologo /machine:X64 /def:$def /out:"$out\$libOut" | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Host "    OK -> $out\$libOut" -ForegroundColor Green } else { Write-Host "    lib.exe FAILED $LASTEXITCODE" -ForegroundColor Red }
}
Write-Host "=== result ===" -ForegroundColor Cyan
Get-ChildItem $out -Filter 'icu*.lib' | Select-Object Name,Length
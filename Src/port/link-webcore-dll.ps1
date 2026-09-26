param()

. "$PSScriptRoot\..\setenv.ps1"

$ninjaDir = Resolve-Path "$env:APOTHEOSIS_ROOT\build-x64-gpu"
$webcoreDir = "$ninjaDir\Source\WebCore\CMakeFiles\WebCore.dir"
$palDir = "$ninjaDir\Source\WebCore\PAL\pal\CMakeFiles\PAL.dir"

# Collect all .obj files
$allObjs = @()
$allObjs += Get-ChildItem "$webcoreDir\*.obj" -Recurse | ForEach-Object { $_.FullName }
$allObjs += Get-ChildItem "$palDir\*.obj" -Recurse | ForEach-Object { $_.FullName }
Write-Output "Total .obj files: $($allObjs.Count)"

# Libraries
$libs = @(
    "`"$env:ProgramFiles\LLVM\lib\clang\22\lib\windows\clang_rt.builtins-x86_64.lib`""
    "C:\icu-x64-uwp\lib\icuuc.lib"
    "C:\vcpkg\installed\x64-uwp\lib\libxml2.lib"
    "$env:APOTHEOSIS_ROOT\build-x64-gpu\libs\sqlite3.lib"
    "C:\vcpkg\installed\x64-uwp\lib\z.lib"
    "$env:APOTHEOSIS_ROOT\Src\angle\x64\libGLESv2.lib"
    "$env:APOTHEOSIS_ROOT\Src\angle\x64\libEGL.lib"
    "$env:APOTHEOSIS_ROOT\build-x64-gpu\libs\psl\psl.lib"
    "$env:APOTHEOSIS_ROOT\build-x64-gpu\libs\cairo-complete-x64-uwp.lib"
    "C:\vcpkg\installed\x64-uwp\lib\libcurl.lib"
    "C:\vcpkg\installed\x64-uwp\lib\libssl.lib"
    "C:\vcpkg\installed\x64-uwp\lib\jpeg.lib"
    "C:\vcpkg\installed\x64-uwp\lib\libpng16.lib"
    "C:\vcpkg\installed\x64-uwp\lib\libwebp.lib"
    "C:\vcpkg\installed\x64-uwp\lib\libwebpdemux.lib"
    "C:\vcpkg\installed\x64-uwp\lib\pixman-1.lib"
    "C:\vcpkg\installed\x64-uwp\lib\freetype.lib"
    "C:\icu-x64-uwp\lib\icudt.lib"
    "C:\icu-x64-uwp\lib\icuin.lib"
    "C:\vcpkg\installed\x64-uwp\lib\libcrypto.lib"
    "$ninjaDir\lib\JavaScriptCore.lib"
    "WindowsApp.lib"
)

$out = "$ninjaDir\bin\WebCore.dll"
$implib = "$ninjaDir\lib\WebCore.lib"
$pdb = "$ninjaDir\bin\WebCore.pdb"
$def = "$env:APOTHEOSIS_ROOT\Src\port\webcore-exports.def"

# Build response file content
$rspContent = @()
$rspContent += $allObjs
$rspContent += $libs
$rspContent += "/machine:x64 /INCREMENTAL:NO /APPCONTAINER /MANIFEST:NO /DEBUG /OPT:NOICF /OPT:REF /DEF:`"$def`" /out:`"$out`" /implib:`"$implib`" /pdb:`"$pdb`" /dll /version:1.0"

$rspFile = "$env:TEMP\webcore-link.rsp"
$rspContent -join "`r`n" | Set-Content -Path $rspFile -Encoding ASCII
Write-Output "Response file written: $rspFile ($((Get-Item $rspFile).Length) bytes)"

$lldLink = "${env:ProgramFiles}\LLVM\bin\lld-link.exe"
Write-Output "Running: lld-link /nologo @$rspFile"
Write-Output ""

# Run the linker directly (avoid Invoke-Expression which chokes on @)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $lldLink /nologo @$rspFile
$sw.Stop()
Write-Output "Link completed in $($sw.Elapsed.TotalSeconds)s"

if (Test-Path $out) {
    $dllSize = (Get-Item $out).Length
    Write-Output "WebCore.dll created: $dllSize bytes"
} else {
    Write-Error "WebCore.dll was NOT created!"
}

# build-harness.ps1 — build harness appx (v143, ARM or x64 depending on APOTHEOSIS_ARCH).
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$proj = "$env:APOTHEOSIS_HARNESS\Harness.vcxproj"
$log = "$env:APOTHEOSIS_ROOT\harness-build.log"

if (-not $env:APOTHEOSIS_ARCH) { $env:APOTHEOSIS_ARCH = 'x64' }
$arch = if ($env:APOTHEOSIS_ARCH -eq 'arm') { 'ARM' } else { 'x64' }
Write-Host "=== Target arch: $arch (APOTHEOSIS_ARCH=$env:APOTHEOSIS_ARCH) ===" -ForegroundColor Cyan

Write-Host "=== [1/2] Generate XAML .g.hpp (MarkupCompilePass1+2) ===" -ForegroundColor Cyan
& $msbuild $proj /p:Configuration=Release /p:Platform=$arch /t:MarkupCompilePass1`;MarkupCompilePass2 /v:minimal 2>&1 | Out-Null

Write-Host "=== [2/2] Full build + appx package ===" -ForegroundColor Cyan
& $msbuild $proj /p:Configuration=Release /p:Platform=$arch /m /v:minimal 2>&1 | Tee-Object $log
$code = $LASTEXITCODE
Write-Host "=== MSBuild exit code: $code ==="
exit $code

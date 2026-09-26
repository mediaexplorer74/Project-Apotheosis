$env:APOTHEOSIS_ROOT = "C:\Users\media\source\repos\Vibe\Apotheosis"
$ninja = "C:\ProgramData\chocolatey\bin\ninja.exe"
$log = "$env:APOTHEOSIS_ROOT\build-x64-gpu\ninja-build.log"
$Build = "$env:APOTHEOSIS_ROOT\build-x64-gpu"
& $ninja -j1 -C $Build WebCore 2>&1 | Out-File $log -Encoding utf8
Write-Output "Ninja finished with exit code: $LASTEXITCODE"

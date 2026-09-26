# Apotheosis: scratch run chain for the .60 measurement. Delete when done.
# Steps: build appx -> install in place (keeps LocalState) -> settle -> clear rh-trace.log -> navigate.
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\media\source\repos\Vibe\Apotheosis'
$ls = 'C:\Users\media\AppData\Local\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState'

$stamp = Get-Date -Format 'HH:mm:ss'
Write-Output "[$stamp] step 1: build appx"
& cmd.exe /c "$root\Src\tools\_build-appx-x64.bat" 2>&1 | Select-Object -Last 12
if ($LASTEXITCODE -ne 0) { Write-Output "APPX BUILD FAILED exit=$LASTEXITCODE"; exit 1 }

Write-Output ("[{0}] step 2: install in place + launch" -f (Get-Date -Format 'HH:mm:ss'))
& pwsh -NoProfile -File "$root\Src\tools\update-local-x64.ps1" 2>&1 | Select-Object -Last 15

Write-Output ("[{0}] step 3: settle 40 s (home page)" -f (Get-Date -Format 'HH:mm:ss'))
Start-Sleep -Seconds 40

Write-Output ("[{0}] step 4: clear rh-trace.log so the trace holds one navigation" -f (Get-Date -Format 'HH:mm:ss'))
Remove-Item (Join-Path $ls 'rh-trace.log') -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $ls 'rh-trace.txt'))) { Write-Output 'ARM FILE MISSING -- recreate rh-trace.txt'; exit 1 }

Write-Output ("[{0}] step 5: navigate to lenta.ru" -f (Get-Date -Format 'HH:mm:ss'))
& pwsh -NoProfile -File "$root\Src\tools\fresh-load.ps1" -NoRestart -Url 'https://lenta.ru/' -SettleSec 0 -RenderSec 45 -AfterSec 6 2>&1 | Select-Object -Last 20

Write-Output ("[{0}] step 6: settle 30 s, then read the trace" -f (Get-Date -Format 'HH:mm:ss'))
Start-Sleep -Seconds 30
$tr = Join-Path $ls 'rh-trace.log'
Write-Output ("rh-trace.log: {0} bytes" -f (Get-Item $tr -ErrorAction SilentlyContinue).Length)
Write-Output ("[{0}] done" -f (Get-Date -Format 'HH:mm:ss'))

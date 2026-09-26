# run-trace-tests.ps1 — trace-instrumented engine test (wt: glyph.log forensics).
# Runs the reference crash/hang/alive page set and dumps the wt:/fill: traces
# from glyph.log per run so the crash phase can be pinpointed.
param([int]$WaitSec = 22)
$ErrorActionPreference = 'Stop'
$pfn = 'EdgeHTMLReborn.Harness_edmb40rfkwsbg'
$aumid = "$pfn!App"
$ls = Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalState"
$box = [string][char]0x2500
$ell = [string][char]0x2026
$kana = [string][char]0x306E
$wan = [string][char]0x4E00
$euro = [string][char]0x20AC

function Write-Utf8NoBom([string]$path, [string]$text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $enc)
}

function Read-NewLines([string]$path, [long]$from) {
    if (-not (Test-Path $path)) { return @() }
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($from, 'Begin') | Out-Null
        return (New-Object System.IO.StreamReader($fs)).ReadToEnd() -split "`r?`n" | Where-Object { $_ }
    } finally { $fs.Close() }
}

function Test-One([string]$name, [string]$html) {
    $htmlPath = Join-Path $ls "$name.html"
    Write-Utf8NoBom $htmlPath $html
    $url = "file:///$($htmlPath -replace '\\','/')"
    Write-Utf8NoBom (Join-Path $ls 'testurl.txt') $url
    Get-Process Harness -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $logFile = Join-Path $ls 'log.txt'
    $glyphFile = Join-Path $ls 'glyph.log'
    # The harness Logger truncates log.txt at startup, so a delta read from a
    # pre-launch length would seek past EOF and capture nothing. Delete both files
    # before launch and read the whole file afterwards.
    Remove-Item $logFile, $glyphFile -ErrorAction SilentlyContinue
    Start-Process "shell:AppsFolder\$aumid"
    Start-Sleep -Seconds $WaitSec
    $alive = [bool](Get-Process Harness -ErrorAction SilentlyContinue)
    $newLog = Read-NewLines $logFile 0
    $newGlyph = Read-NewLines $glyphFile 0
    $afterLoad = ($newLog | Select-String -Pattern 'after-load url=file' -Quiet)
    $result = if ($alive -and $afterLoad) { 'ALIVE' } else { if ($alive) { 'HANG' } else { 'CRASH' } }
    $wt = @($newGlyph | Select-String '^wt:')
    $fills = @($newGlyph | Select-String '^fill:')
    $pInit = @($newGlyph | Select-String 'platformInit').Count
    Write-Output ("{0,-6} {1,-10} alive={2,-5} afterLoad={3,-5} pInit={4} wt={5} fill={6}" -f $result, $name, $alive, $afterLoad, $pInit, $wt.Count, $fills.Count)
    if ($wt.Count) {
        Write-Output "  --- wt: traces ($($wt.Count)) ---"
        $wt | ForEach-Object { Write-Output "  $($_.Line)" }
    }
    if ($fills.Count) {
        Write-Output "  --- fill: traces ($($fills.Count)) ---"
        $fills | Select-Object -Last 8 | ForEach-Object { Write-Output "  $($_.Line)" }
    }
    Write-Output "  --- last log lines ---"
    $newLog | Select-Object -Last 6 | ForEach-Object { Write-Output "  $_" }
    $newLog | Set-Content (Join-Path $env:TEMP "trace-$name.log.txt")
    $newGlyph | Set-Content (Join-Path $env:TEMP "trace-$name.glyph.txt")
    Write-Output ""
}

$head = '<html><head><meta charset="utf-8"></head><body>'
$foot = '</body></html>'
Test-One 'single'   "$head<p>$box</p>$foot"
Test-One 'mixed'    "$head<p>A$box B</p>$foot"
Test-One 'wan-meta' "$head<p>A$wan B</p>$foot"
Test-One 'kana-n'   "$head<p>A$kana B</p>$foot"
Test-One 'br-kana'  "$head<p>$kana</p>$foot"
Test-One 'mixed-e'  "$head<p>A$ell B</p>$foot"
Test-One 'br-euro'  "$head<p>$euro</p>$foot"
Test-One 'cjk-b'    "$head<p>A$wan B</p>$foot"
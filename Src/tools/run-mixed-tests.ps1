param([string]$Ip = "")
$ErrorActionPreference = 'Stop'
$pfn = 'EdgeHTMLReborn.Harness_edmb40rfkwsbg'
$aumid = "$pfn!App"
$ls = Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalState"
$box = [string][char]0x2500
$ell = [string][char]0x2026

function Write-Utf8NoBom([string]$path, [string]$text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $enc)
}

function Test-One([string]$name, [string]$html) {
    $htmlPath = Join-Path $ls "$name.html"
    Write-Utf8NoBom $htmlPath $html
    $url = "file:///$($htmlPath -replace '\\','/')"
    Write-Utf8NoBom (Join-Path $ls 'testurl.txt') $url
    Get-Process Harness -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $logFile = Join-Path $ls 'log.txt'
    $beforeLen = if (Test-Path $logFile) { (Get-Item $logFile).Length } else { 0 }
    $glyphFile = Join-Path $ls 'glyph.log'
    $gBeforeLen = if (Test-Path $glyphFile) { (Get-Item $glyphFile).Length } else { 0 }
    Start-Process "shell:AppsFolder\$aumid"
    Start-Sleep -Seconds 22
    $alive = [bool](Get-Process Harness -ErrorAction SilentlyContinue)
    $newLog = @()
    if (Test-Path $logFile) {
        $fs = [System.IO.File]::Open($logFile, 'Open', 'Read', 'ReadWrite')
        try {
            $fs.Seek($beforeLen, 'Begin') | Out-Null
            $newLog = (New-Object System.IO.StreamReader($fs)).ReadToEnd() -split "`r?`n" | Where-Object { $_ }
        } finally { $fs.Close() }
    }
    $afterLoad = ($newLog | Select-String -Pattern 'after-load url=file' -Quiet)
    $newGlyph = @()
    if (Test-Path $glyphFile) {
        $fs = [System.IO.File]::Open($glyphFile, 'Open', 'Read', 'ReadWrite')
        try {
            $fs.Seek($gBeforeLen, 'Begin') | Out-Null
            $newGlyph = (New-Object System.IO.StreamReader($fs)).ReadToEnd() -split "`r?`n" | Where-Object { $_ }
        } finally { $fs.Close() }
    }
    $result = if ($alive -and $afterLoad) { 'ALIVE' } else { if ($alive) { 'HANG' } else { 'CRASH' } }
    $pages = @($newGlyph | Select-String '^fill: MISS i=0' | ForEach-Object { $_.Line -replace '^fill: MISS i=0 cp=U\+','' })
    Write-Output ("{0,-7} {1,-12} alive={2,-5} afterLoad={3,-5} pInit={4} pages=[{5}]" -f $result, $name, $alive, $afterLoad, @($newGlyph | Select-String 'platformInit').Count, ($pages -join ','))
    $newLog | Set-Content (Join-Path $env:TEMP "pt2-$name.txt") -ErrorAction SilentlyContinue
}

$head = '<html><head><meta charset="utf-8"></head><body>'
$foot = '</body></html>'
Test-One 'single'   "$head<p>$box</p>$foot"
Test-One 'attr'     "$head<div title=`"A$box B`">C</div>$foot"
Test-One 'comment'  "$head<!-- A$box B --><p>C</p>$foot"
Test-One 'mixed'    "$head<p>A$box B</p>$foot"
Test-One 'bold'     "$head<p>A<b>$box</b> B</p>$foot"
Test-One 'mixed-e'  "$head<p>A$ell B</p>$foot"
Test-One 'bold-e'   "$head<p>A<b>$ell</b> B</p>$foot"
Test-One 'span-box' "$head<p>A<span>$box</span> B</p>$foot"
param([string]$Ip = "")
$ErrorActionPreference = 'Stop'
$pfn = 'EdgeHTMLReborn.Harness_edmb40rfkwsbg'
$aumid = "$pfn!App"
$ls = Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalState"
$out = Join-Path $env:TEMP 'char-tests2'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$ell = [string][char]0x2026
$kana = [string][char]0x306E
$box = [string][char]0x2500
$w = $box

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
    $result = if ($alive -and $afterLoad) { 'ALIVE' } else { 'CRASH' }
    $pages = @($newGlyph | Select-String '^fill: MISS i=0' | ForEach-Object { $_.Line -replace '^fill: MISS i=0 cp=U\+','' })
    Write-Output ("{0,-8} {1,-12} alive={2,-5} afterLoad={3,-5} pInit={4} pages=[{5}]" -f $result, $name, $alive, $afterLoad, @($newGlyph | Select-String 'platformInit').Count, ($pages -join ','))
}

$head = '<html><head><meta charset="utf-8"></head><body>'
$foot = '</body></html>'
Test-One 'hidden'  "$head<p style=`"display:none`">A$w B</p>$foot"
Test-One 'attr'    "$head<div title=`"A$w B`">C</div>$foot"
Test-One 'comment' "$head<!-- A$w B --><p>C</p>$foot"
Test-One 'single'  "$head<p>$w</p>$foot"
Test-One 'ncr'     "$head<p>A&#x2026;B</p>$foot"
Test-One 'kana-n'  "$head<p>A$kana B</p>$foot"
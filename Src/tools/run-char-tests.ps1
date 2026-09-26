param(
    [string[]]$Chars = @(),
    [string[]]$Names = @(),
    [int]$WaitSec = 22,
    [string]$Ip = ""
)
$ErrorActionPreference = 'Stop'
$pfn = 'EdgeHTMLReborn.Harness_edmb40rfkwsbg'
$aumid = "$pfn!App"
$ls = Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalState"
$out = Join-Path $env:TEMP 'char-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Write-Utf8NoBom([string]$path, [string]$text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $enc)
}

function Test-One([string]$name, [string]$char) {
    $html = "<html><head><meta charset=`"utf-8`"></head><body><p>A$char B</p></body></html>"
    $htmlPath = Join-Path $ls "$name.html"
    Write-Utf8NoBom $htmlPath $html
    $url = "file:///$($htmlPath -replace '\\','/')"
    Write-Utf8NoBom (Join-Path $ls 'testurl.txt') $url

    Get-Process Harness -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $logFile = Join-Path $ls 'log.txt'
    $beforeLen = if (Test-Path $logFile) { (Get-Item $logFile).Length } else { 0 }
    Start-Process "shell:AppsFolder\$aumid"
    Start-Sleep -Seconds $WaitSec

    $alive = [bool](Get-Process Harness -ErrorAction SilentlyContinue)
    $log = Get-Content $logFile -ErrorAction SilentlyContinue
    $newLog = @()
    if (Test-Path $logFile) {
        $fs = [System.IO.File]::Open($logFile, 'Open', 'Read', 'ReadWrite')
        try {
            $fs.Seek($beforeLen, 'Begin') | Out-Null
            $sr = New-Object System.IO.StreamReader($fs)
            $newLog = $sr.ReadToEnd() -split "`r?`n" | Where-Object { $_ }
        } finally { $fs.Close() }
    }
    $afterLoad = ($newLog | Select-String -Pattern 'after-load url=file' -Quiet)
    $beforeLoad = ($newLog | Select-String -Pattern 'before-load' -Quiet)
    $glyph = Get-Content (Join-Path $ls 'glyph.log') -ErrorAction SilentlyContinue
    $missLines = @($glyph | Select-String -Pattern 'MISS')
    $hasGlyph = ($glyph | Select-String -Pattern "cp=U\+[0-9A-F]{4}" -Quiet)

    $cp = ('U+{0:X4}' -f [int][char]$char)
    if ($char.Length -gt 1) {
        $high = [int][char]$char[0]
        $low = [int][char]$char[1]
        $cpFull = ('U+{0:X4} (surrogate {1:X4} {2:X4})' -f (0x10000 + (($high - 0xD800) -shl 10) + ($low - 0xDC00)), $high, $low)
        $cp = $cpFull
    }
    $logPath = Join-Path $out "$name.log.txt"
    $glyphPath = Join-Path $out "$name.glyph.txt"
    $newLog | Set-Content $logPath
    $glyph | Set-Content $glyphPath

    $result = if ($alive -and $afterLoad) { 'ALIVE' } else { 'CRASH' }
    Write-Output ("{0,-8} {1,-10} alive={2,-5} afterLoad={3,-5} miss={4,-3} logLines={5}" -f $result, $name, $alive, $afterLoad, $missLines.Count, $log.Count)
}

if ($Chars.Count -eq 0) {
    $tests = @(
        @{ n = 'kana';   c = [string][char]0x306E },
        @{ n = 'hangul'; c = [string][char]0xAC00 },
        @{ n = 'yisl';   c = [string][char]0xA000 },
        @{ n = 'repl';   c = [string][char]0xFFFD },
        @{ n = 'lincb';  c = [string][char]0xD800 + [string][char]0xDC00 }
    )
} else {
    $tests = @()
    for ($i = 0; $i -lt $Chars.Count; $i++) {
        $tests += @{ n = $Names[$i]; c = $Chars[$i] }
    }
}

foreach ($t in $tests) {
    Test-One $t.n $t.c
    if ($t.n -ne $tests[-1].n) { Start-Sleep -Seconds 3 }
}
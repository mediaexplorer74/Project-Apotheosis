param(
  [Parameter(Mandatory=$true)][string]$Original,
  [Parameter(Mandatory=$true)][string]$Current
)
# Proves that a comment cleanup touched comment text only.
# For every line that differs, the "code prefix" (everything before the first '//' that is outside a
# string literal) must be byte-identical, so no statement, string literal or preprocessor directive can
# have moved. Also checks that comment delimiters and quote counts are unchanged file-wide.
$ErrorActionPreference = 'Stop'

function Get-CodePrefix([string]$s) {
  $inStr = $false; $esc = $false; $i = 0
  while ($i -lt $s.Length) {
    $c = $s[$i]
    if ($esc) { $esc = $false }
    elseif ($inStr -and $c -eq '\') { $esc = $true }
    elseif ($c -eq '"') { $inStr = (-not $inStr) }
    elseif ((-not $inStr) -and $c -eq '/' -and ($i + 1) -lt $s.Length -and $s[$i + 1] -eq '/') { return $s.Substring(0, $i) }
    $i = $i + 1
  }
  return $s
}

function Get-Count([string]$s, [string]$needle) {
  $n = 0; $pos = 0
  while ($true) {
    $k = $s.IndexOf($needle, $pos)
    if ($k -lt 0) { break }
    $n = $n + 1
    $pos = $k + $needle.Length
  }
  return $n
}

function Get-CodeRegionQuotes([string[]]$lines) {
  $n = 0
  foreach ($l in $lines) { $n = $n + (Get-Count (Get-CodePrefix $l) '"') }
  return $n
}

function Read-Text([string]$p) {
  $b = [System.IO.File]::ReadAllBytes((Resolve-Path $p).Path)
  $t = [System.Text.Encoding]::UTF8.GetString($b)
  if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { $t = $t.Substring(1) }
  return $t
}

$origText = Read-Text $Original
$curText = Read-Text $Current
$o = $origText -split "\r\n|\n|\r"
$n = $curText -split "\r\n|\n|\r"

Write-Host ("original lines : " + $o.Length)
Write-Host ("current  lines : " + $n.Length)
if ($o.Length -ne $n.Length) { Write-Host "FAIL: line count changed -- the map was supposed to be line-preserving"; exit 1 }

$changed = 0
$bad = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $o.Length; $i++) {
  if ($o[$i] -ceq $n[$i]) { continue }
  $changed = $changed + 1
  $po = (Get-CodePrefix $o[$i]).TrimEnd()
  $pn = (Get-CodePrefix $n[$i]).TrimEnd()
  if ($po -cne $pn) {
    $bad.Add(("line " + ($i + 1) + ": CODE CHANGED`n    was: " + $o[$i] + "`n    now: " + $n[$i]))
  }
}

Write-Host ("lines changed  : " + $changed)
Write-Host ("block comment  : original /*=" + (Get-Count $origText '/*') + " */=" + (Get-Count $origText '*/') +
            "   current /*=" + (Get-Count $curText '/*') + " */=" + (Get-Count $curText '*/'))
Write-Host ("quotes in CODE : original=" + (Get-CodeRegionQuotes $o) + "   current=" + (Get-CodeRegionQuotes $n) +
            "     (quotes inside comments are not counted -- they may legitimately disappear)")
Write-Host ("backslash-EOL  : current=" + (Get-Count $curText "\\`r") )

if ($bad.Count -gt 0) {
  Write-Host ("FAIL: " + $bad.Count + " line(s) changed outside their comment")
  foreach ($b in $bad) { Write-Host $b }
  exit 1
}
if ((Get-Count $origText '/*') -ne (Get-Count $curText '/*') -or (Get-Count $origText '*/') -ne (Get-Count $curText '*/')) {
  Write-Host "FAIL: block-comment delimiters changed"
  exit 1
}
if ((Get-CodeRegionQuotes $o) -ne (Get-CodeRegionQuotes $n)) {
  Write-Host "FAIL: the number of double quotes in code changed -- a string literal may have been altered"
  exit 1
}
if ((Get-Count $curText "\\`r") -ne 0) {
  Write-Host "FAIL: a line now ends with a backslash (line continuation)"
  exit 1
}
Write-Host "PASS: code prefix identical on every changed line; delimiters, code quotes and continuations clean."

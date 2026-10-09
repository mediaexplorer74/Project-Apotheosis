param(
  [Parameter(Mandatory=$true)][string]$File,
  [ValidateSet('stats','mangled','cjk')][string]$Mode = 'stats',
  [int]$Limit = 400
)
$ErrorActionPreference = 'Stop'
$path = (Resolve-Path $File).Path
$text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
$lines = $text -split "\r\n|\n|\r"
$mLines = New-Object System.Collections.Generic.List[string]
$cLines = New-Object System.Collections.Generic.List[string]
$mCount = 0
$cCount = 0
$qTotal = 0
for ($i = 0; $i -lt $lines.Length; $i++) {
  $line = $lines[$i]
  $na = 0
  $maxRun = 0
  $run = 0
  foreach ($ch in $line.ToCharArray()) {
    $code = [int][char]$ch
    if ($code -gt 127) { $na = $na + 1 }
    if ($ch -eq '?') {
      $run = $run + 1
      if ($run -gt $maxRun) { $maxRun = $run }
    } else {
      $run = 0
    }
  }
  if ($maxRun -ge 2 -and $na -eq 0) {
    $mCount = $mCount + 1
    $qTotal = $qTotal + $maxRun
    $mLines.Add(("{0,6} run={1,3}| {2}" -f ($i + 1), $maxRun, $line))
  }
  if ($na -gt 0) {
    $cCount = $cCount + 1
    $safe = ""
    foreach ($ch in $line.ToCharArray()) {
      if ([int][char]$ch -gt 127) { $safe = $safe + "#" } else { $safe = $safe + $ch }
    }
    $cLines.Add(("{0,6} n={1,3}| {2}" -f ($i + 1), $na, $safe))
  }
}
$totalQ = $qTotal
switch ($Mode) {
  'stats' {
    Write-Host ("file    : " + $path)
    Write-Host ("lines   : " + $lines.Length)
    Write-Host ("MANGLED lines (>=2 literal '?' and zero non-ASCII on the line) : " + $mCount)
    Write-Host ("INTACT  lines (carry live non-ASCII: CJK / typographic dash)   : " + $cCount)
    Write-Host ("sum of longest '?' runs inside mangled lines                   : " + $totalQ)
  }
  'mangled' {
    $n = 0
    foreach ($s in $mLines) {
      Write-Host $s
      $n = $n + 1
      if ($n -ge $Limit) { Write-Host ("... truncated at " + $Limit); break }
    }
  }
  'cjk' {
    $n = 0
    foreach ($s in $cLines) {
      Write-Host $s
      $n = $n + 1
      if ($n -ge $Limit) { Write-Host ("... truncated at " + $Limit); break }
    }
  }
}

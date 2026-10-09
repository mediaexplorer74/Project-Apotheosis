param(
  [Parameter(Mandatory=$true)][string]$File,
  [int]$Skip = 0,
  [int]$Take = 100,
  [int]$MinRun = 2
)
# Byte-accurate candidate report for the mangled-comment cleanup.
# A candidate is any line carrying a run of MinRun or more literal '?' characters.
# Non-ASCII characters are printed as '#' so the console codepage cannot lie about what is live.
$ErrorActionPreference = 'Stop'
$path = (Resolve-Path $File).Path
$text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
$lines = $text -split "\r\n|\n|\r"

$rows = New-Object System.Collections.Generic.List[string]
$total = 0
$commentOnly = 0
$trailing = 0
$literal = 0
$withNonAscii = 0
$qChars = 0

for ($i = 0; $i -lt $lines.Length; $i++) {
  $line = $lines[$i]
  if ($null -eq $line) { continue }
  $maxRun = 0
  $run = 0
  $na = 0
  $qInString = $false
  $inStr = $false
  $esc = $false
  foreach ($ch in $line.ToCharArray()) {
    $code = [int][char]$ch
    if ($code -gt 127) { $na = $na + 1 }
    if ($esc) { $esc = $false }
    elseif ($inStr -and $ch -eq '\') { $esc = $true }
    elseif ($ch -eq '"') { $inStr = (-not $inStr) }
    elseif ($ch -eq '?') {
      if ($inStr) { $qInString = $true }
      $run = $run + 1
      if ($run -gt $maxRun) { $maxRun = $run }
    } else { $run = 0 }
  }
  if ($maxRun -lt $MinRun) { continue }

  $qChars = $qChars + $maxRun
  $total = $total + 1
  $trim = $line.TrimStart()
  if ($trim.StartsWith('//') -or $trim.StartsWith('*') -or $trim.StartsWith('/*')) {
    $kind = 'C'
    $commentOnly = $commentOnly + 1
  } else {
    $kind = 'T'
    $trailing = $trailing + 1
  }
  if ($qInString) {
    $kind = $kind + 'S'
    $literal = $literal + 1
  }
  if ($na -gt 0) {
    $kind = $kind + 'X'
    $withNonAscii = $withNonAscii + 1
  }

  $safe = ""
  foreach ($ch in $line.ToCharArray()) {
    if ([int][char]$ch -gt 127) { $safe = $safe + '#' } else { $safe = $safe + $ch }
  }
  $rows.Add(("{0,6} {1,4} {2,3}| {3}" -f ($i + 1), $maxRun, $kind, $safe))
}

Write-Host ("file      : " + $path + "   lines=" + $lines.Length)
Write-Host ("candidates: " + $total + "   comment-only=" + $commentOnly + "   trailing-after-code=" + $trailing +
            "   touches-string-literal=" + $literal + "   also-has-live-non-ASCII=" + $withNonAscii)
$end = [Math]::Min($Skip + $Take, $rows.Count)
Write-Host ("showing   : rows " + $Skip + ".." + ($end - 1) + " of " + $rows.Count)
for ($k = $Skip; $k -lt $end; $k++) { Write-Host $rows[$k] }

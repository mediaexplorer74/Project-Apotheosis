param(
  [Parameter(Mandatory=$true)][string]$File,
  [Parameter(Mandatory=$true)][string]$Map,
  [switch]$DryRun
)
# Applies a mangled-comment map to a source file in ONE pass, against the ORIGINAL line numbering.
#
# Map entries (TAB separated, blank lines and lines starting with '#' ignored):
#   <lineNo>\tD                      delete the whole line
#   <lineNo>\tC\t<new comment text>  replace the comment (first '//' outside a string) to EOL
#   <lineNo>\tW\t<new line text>     replace the whole line
#
# Safety: every touched line must still carry a run of two or more literal '?' characters, so a map
# that has drifted out of step with the file aborts instead of silently editing live code.
$ErrorActionPreference = 'Stop'

function Get-MaxQuestionRun([string]$s) {
  $max = 0; $run = 0
  foreach ($ch in $s.ToCharArray()) {
    if ($ch -eq '?') { $run = $run + 1; if ($run -gt $max) { $max = $run } } else { $run = 0 }
  }
  return $max
}

function Find-CommentStart([string]$s) {
  $inStr = $false; $esc = $false; $i = 0
  while ($i -lt $s.Length) {
    $c = $s[$i]
    if ($esc) { $esc = $false }
    elseif ($inStr -and $c -eq '\') { $esc = $true }
    elseif ($c -eq '"') { $inStr = (-not $inStr) }
    elseif ((-not $inStr) -and $c -eq '/' -and ($i + 1) -lt $s.Length -and $s[$i + 1] -eq '/') { return $i }
    $i = $i + 1
  }
  return -1
}

$path = (Resolve-Path $File).Path
$bytes = [System.IO.File]::ReadAllBytes($path)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)

$raw = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom) { $raw = $raw.Substring(1) }
$crlf = ($raw.IndexOf("`r`n") -ge 0)
$nl = "`n"
if ($crlf) { $nl = "`r`n" }
$lines = New-Object System.Collections.Generic.List[string]
foreach ($l in ($raw -split "`r`n|`n")) { $lines.Add($l) }
# A trailing newline yields one empty final element; remember it so it can be restored.
$trailing = ''
if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') {
  $trailing = $nl
  $lines.RemoveAt($lines.Count - 1)
}

$rewrite = @{}
$delete = New-Object System.Collections.Generic.List[int]
$mapPaths = @()
if ($Map -match '\*') {
  foreach ($f in (Get-ChildItem -Path $Map -File | Sort-Object Name)) { $mapPaths += $f.FullName }
} else {
  $mapPaths = @((Resolve-Path $Map).Path)
}
$mapEntries = New-Object System.Collections.Generic.List[string]
foreach ($mp in $mapPaths) {
  foreach ($e in [System.IO.File]::ReadAllLines($mp, [System.Text.Encoding]::UTF8)) { $mapEntries.Add($e) }
}
Write-Host ("map files   : " + $mapPaths.Count + "   entries: " + $mapEntries.Count)
foreach ($entry in $mapEntries) {
  if ([string]::IsNullOrWhiteSpace($entry)) { continue }
  if ($entry.StartsWith('#')) { continue }
  $parts = $entry -split "`t"
  if ($parts.Count -lt 2) { throw ("map line is not TAB separated: " + $entry) }
  $n = 0
  $minRun = 2
  $lineSpec = $parts[0].Trim()
  if ($lineSpec.StartsWith('*')) { $minRun = 1; $lineSpec = $lineSpec.Substring(1) }
  if (-not [int]::TryParse($lineSpec, [ref]$n)) { throw ("bad line number in map: " + $entry) }
  $action = $parts[1].Trim()
  $payload = ''
  if ($parts.Count -gt 2) { $payload = ($parts[2..($parts.Count - 1)] -join "`t") }
  if ($n -lt 1 -or $n -gt $lines.Count) { throw ("map line out of range: " + $entry) }
  if ($delete.Contains($n) -or $rewrite.ContainsKey($n)) { throw ("duplicate map entry for line " + $n) }
  $current = $lines[$n - 1]
  $run = Get-MaxQuestionRun $current
  if ($run -lt $minRun) { throw ("line " + $n + " no longer carries a '?' run >= " + $minRun + " - map is out of step: " + $current) }
  if ($action -eq 'D') { $delete.Add($n); continue }
  if ($action -eq 'C') {
    $ci = Find-CommentStart $current
    if ($ci -lt 0) { throw ("line " + $n + " has no '//' comment to splice: " + $current) }
    if ($payload -ne '-' -and -not $payload.StartsWith('//')) { throw ("C payload must start with '//' (or be '-') on line " + $n) }
    $rewrite[$n] = @('C', $ci, $payload)
    continue
  }
  if ($action -eq 'W') { $rewrite[$n] = @('W', 0, $payload); continue }
  throw ("unknown action '" + $action + "' for line " + $n)
}

$out = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $lines.Count; $i++) {
  $n = $i + 1
  if ($delete.Contains($n)) { continue }
  if ($rewrite.ContainsKey($n)) {
    $r = $rewrite[$n]
    if ($r[0] -eq 'C') {
      $prefix = $lines[$i].Substring(0, $r[1])
      $new = $prefix
      if ($r[2] -ne '-') { $new = $prefix + $r[2] }
      $new = $new.TrimEnd()
      $out.Add($new)
    } else {
      $out.Add($r[2])
    }
    continue
  }
  $out.Add($lines[$i])
}

$remaining = 0
foreach ($l in $out) { if ((Get-MaxQuestionRun $l) -ge 2) { $remaining = $remaining + 1 } }

Write-Host ("file        : " + $path)
$nlName = 'LF'
if ($crlf) { $nlName = 'CRLF' }
Write-Host ("bom         : " + $hasBom + "   newline: " + $nlName)
Write-Host ("input lines : " + $lines.Count + "   output lines: " + $out.Count)
Write-Host ("rewritten   : " + $rewrite.Count + "   deleted: " + $delete.Count)
Write-Host ("candidates left after this map (need a '?' run >= 2) : " + $remaining)

if ($DryRun) { Write-Host "DRY RUN - nothing written"; exit 0 }

$joined = [string]::Join($nl, $out.ToArray()) + $trailing
$enc = New-Object System.Text.UTF8Encoding($hasBom)
[System.IO.File]::WriteAllText($path, $joined, $enc)
Write-Host "written."

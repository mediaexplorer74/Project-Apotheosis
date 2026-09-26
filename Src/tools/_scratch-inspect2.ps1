# Apotheosis: scratch, part 2 -- ordered view of the .60 trace. Delete when done.
$ls = 'C:\Users\media\AppData\Local\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState'
$tr = Get-Content (Join-Path $ls 'rh-trace.log')

Write-Output "=== FL: lines in FILE ORDER (index in the trace) ==="
for ($i = 0; $i -lt $tr.Count; $i++) {
    if ($tr[$i] -like 'FL:*') { Write-Output ("  [{0,4}] {1}" -f $i, $tr[$i]) }
}

Write-Output "`n=== last 30 trace lines in file order ==="
$tr | Select-Object -Last 30 | ForEach-Object { Write-Output ("  " + $_) }

$line = (Get-Content (Join-Path $ls 'log.txt') | Select-String -SimpleMatch 'latediag[5/5]' | Select-Object -First 1).Line
if (-not $line) { Write-Output "`nNO latediag[5/5]"; exit }
$inner = $line.Substring($line.IndexOf(' res:[') + 6)
$inner = $inner.Substring(0, $inner.IndexOf(']'))
$entries = @($inner -split '\s+' | Where-Object { $_ -match '\(s\d\)$' })
$pending = @($entries | Where-Object { $_ -match '\(s1\)$' } | ForEach-Object { $_ -replace '\(s1\)$','' })
Write-Output ("`n=== the {0} Pending leaves of latediag[5/5] and their whole trace history ===" -f $pending.Count)
$byLeaf = @{}
for ($i = 0; $i -lt $tr.Count; $i++) {
    $p = $tr[$i] -split ' '
    if ($p.Count -lt 3) { continue }
    $leaf = $p[2]
    if (-not $byLeaf.ContainsKey($leaf)) { $byLeaf[$leaf] = @() }
    $byLeaf[$leaf] += ("{0}[{1}]" -f (($p[0] + $p[1]) -replace ':$',''), $i)
}
$histo = @{}
$silent = 0
foreach ($leaf in $pending) {
    $k = $leaf.Substring(0, [Math]::Min(44, $leaf.Length))
    if (-not $byLeaf.ContainsKey($k)) { $silent++; $tag = '<NO TRACE LINE AT ALL>'; $histo[$tag] = 1 + ($histo[$tag] | ForEach-Object { $_ }) }
    else { $tag = ($byLeaf[$k] -join ' ') }
    if (-not $histo.ContainsKey($tag)) { $histo[$tag] = 0 }
    $histo[$tag]++
    Write-Output ("  " + $k.PadRight(46) + " " + $tag)
}
Write-Output "`n=== verdict summary ==="
$histo.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Write-Output ("  " + $_.Value.ToString().PadLeft(4) + "  " + $_.Key) }
Write-Output ("`n  Pending leaves with no trace line at all: {0}" -f $silent)

Write-Output "`n=== RH:/SL:/RC: line counts per prefix, and totals ==="
$tot = @{}
foreach ($l in $tr) {
    $p = $l -split ' '
    if ($p.Count -ge 2 -and $p[0] -match ':$') { $k = $p[0] + $p[1]; if ($k -match '^RC:') { $k = 'RC: (all)' } ; if (-not $tot.ContainsKey($k)) { $tot[$k] = 0 }; $tot[$k]++ }
}
$tot.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Output ("  " + $_.Name.PadRight(18) + " " + $_.Value) }

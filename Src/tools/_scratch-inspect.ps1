# Apotheosis: scratch analysis for the .60 loader-gate instrument. Delete when done.
$ls = 'C:\Users\media\AppData\Local\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState'
$trPath = Join-Path $ls 'rh-trace.log'
if (-not (Test-Path $trPath)) { Write-Output "NO rh-trace.log -- the arm file is missing or the build is not armed"; exit 1 }
$tr = Get-Content $trPath
Write-Output ("trace = {0} lines, {1} bytes" -f $tr.Count, (Get-Item $trPath).Length)

Write-Output "`n=== prefix census ==="
$tr | ForEach-Object {
    $p = $_ -split ' '
    if ($p.Count -ge 2 -and $p[0] -match ':$') { ($p[0] + $p[1]) } else { $p[0] }
} | Group-Object | Sort-Object Count -Descending | ForEach-Object { Write-Output ("  " + $_.Name.PadRight(20) + " x" + $_.Count) }

Write-Output "`n=== FL: lines (the gate), in order ==="
$fl = @($tr | Where-Object { $_ -like 'FL:*' })
Write-Output ("  total FL lines: {0}" -f $fl.Count)
$fl | Group-Object | Sort-Object Count -Descending | Select-Object -First 12 | ForEach-Object { Write-Output ("  x" + $_.Count.ToString().PadLeft(4) + "  " + $_.Name) }
Write-Output "  --- last 15 FL lines in file order ---"
$fl | Select-Object -Last 15 | ForEach-Object { Write-Output ("    " + $_) }

Write-Output "`n=== RC: balance ==="
$rc = @($tr | Where-Object { $_ -like 'RC:*' })
Write-Output ("  total RC lines: {0}" -f $rc.Count)
$last = $rc | Select-Object -Last 1
if ($last) { Write-Output ("  last line: " + $last) }
$plus = @{}; $minus = @{}
foreach ($l in $rc) {
    $p = $l -split ' '
    if ($p.Count -lt 3) { continue }
    $leaf = $p[2]
    if ($p[1].StartsWith('+')) { if (-not $plus.ContainsKey($leaf)) { $plus[$leaf] = 0 }; $plus[$leaf]++ }
    elseif ($p[1].StartsWith('-')) { if (-not $minus.ContainsKey($leaf)) { $minus[$leaf] = 0 }; $minus[$leaf]++ }
}
Write-Output ("  distinct leaves: +{0}  -{1}" -f $plus.Count, $minus.Count)
$leak = @($plus.Keys | Where-Object { -not $minus.ContainsKey($_) -or $minus[$_] -ne $plus[$_] })
Write-Output ("  leaves whose + and - do not match: {0}" -f $leak.Count)
foreach ($l in $leak) {
    $m = if ($minus.ContainsKey($l)) { $minus[$l] } else { 0 }
    Write-Output ("    UNBALANCED  +{0} -{1}  {2}" -f $plus[$l], $m, $l)
}
Write-Output "  --- any NEGATIVE line ---"
$neg = @($rc | Where-Object { $_ -like '*NEGATIVE*' })
if ($neg.Count -eq 0) { Write-Output '    none' } else { $neg | Select-Object -First 10 | ForEach-Object { Write-Output ("    " + $_) } }

Write-Output "`n=== latediag: resource census + the fields that matter ==="
foreach ($n in 1..5) {
    $line = (Get-Content (Join-Path $ls 'log.txt') | Select-String -SimpleMatch ("latediag[" + $n + "/5]") | Select-Object -First 1).Line
    if (-not $line) { Write-Output ("  latediag[$n/5]: absent"); continue }
    $i = $line.IndexOf(' res:[')
    $inner = if ($i -ge 0) { $line.Substring($i + 6) } else { '' }
    $entries = @()
    if ($inner) { $inner = $inner.Substring(0, $inner.IndexOf(']')); $entries = @($inner -split '\s+' | Where-Object { $_ -match '\(s\d\)$' }) }
    $census = (0..4 | ForEach-Object { $s = $_; $c = @($entries | Where-Object { $_ -match "\(s$s\)$" }).Count; if ($c) { "s$s=$c" } }) -join ' '
    $fields = ''
    foreach ($f in @('body=', 'pending=', 'nonwhite=', 'rs=', 'contents=')) {
        $m = [regex]::Match($line, [regex]::Escape($f) + '(\S+?)[ /]')
        if ($m.Success) { $fields += ($f + $m.Groups[1].Value + '  ') }
    }
    Write-Output ("  latediag[$n/5]: entries={0}  {1}" -f $entries.Count, $census)
    Write-Output ("                {0}" -f $fields.Trim())
    Write-Output ("                scr={0}" -f ([regex]::Match($line, 'scr=(\S+)').Groups[1].Value))
}

#Requires -Version 7.0
<#
  analyze-device-run.ps1 -- turn a device acceptance run's autocmd-log.txt into one row per site.

  Why this exists. On the bench Src\tools\run-smoke.ps1 writes the rows itself, because it drives
  the app from outside and can poll stage.txt between sites. On the Lumia the list runs inside the
  app (Src\tools\device-accept\device-run.txt, run by MainPage.TryStartPackagedBatch) and the only
  record is autocmd-log.txt: one line per command, with the engine's own diag on the line the
  `diag` verb wrote. This script reads that file and prints the same shape of row, so a device run
  and a bench run are compared as measurements rather than narrated separately.

  The row answers, per site, in the order the list asks:
    doc        the DOCUMENT the engine loaded (`diag ... | url=`), not the requested URL. This is
               the field that names dzen's SSO page and reddit's challenge page.
    nonwhite   painted fraction, from the same diag (`nonwhite=N/T`)
    body/rs    whether a body exists and the parser state
    swipe      the real touch path's total ms (`swipe end totalMs=`), the requirement-6 number
    back/fwd   the host-stack result (`history back rc=`) -- -99 is the documented fallback
    restored   the second diag, taken after Back then Forward: proof the row came back
    dead       set when the log ends before the batch does, which is the only death marker there is

  It also prints the packaged-batch header, so a run that never started (no batch, wrong version,
  marker already set) is distinguishable from one that started and failed.

  Usage:
    pwsh -File Src\tools\analyze-device-run.ps1 -Log Src\tools\device-logs\<stamp>\autocmd-log.txt
    pwsh -File Src\tools\analyze-device-run.ps1 -Log <file> -Csv <out.csv>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Log,
    [string]$Csv
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Log)) { Write-Host "no such log: $Log" -ForegroundColor Red; exit 1 }

$rxStamp = '^\d{2}:\d{2}:\d{2}\.\d{3}\s+'

function Get-Field([string]$s, [string]$pattern) {
    $m = [regex]::Match($s, $pattern)
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}

$rows = New-Object System.Collections.Generic.List[object]
$cur = $null
$batchSeen = $false
$batchDone = $false
$afterShellCheck = $false
$batchHeader = ''

# autocmd-log.txt APPENDS for the life of the install -- every deploy keeps writing to the same
# file, so a second run's log opens with the previous run's sites. Only the LAST packaged batch is
# this run; everything above its header belongs to an older one and would otherwise be reported as
# if it had just happened (measured 2026-10-20: the .41 probe opened with all 13 rows of .40).
$all = @(Get-Content -LiteralPath $Log)
$start = 0
for ($i = 0; $i -lt $all.Count; $i++) {
    if ($all[$i] -match '---- packaged batch ') { $start = $i }
}
$runLines = if ($start -gt 0) { $all[$start..($all.Count - 1)] } else { $all }

foreach ($raw in $runLines) {
    $line = [regex]::Replace($raw, $rxStamp, '')

    if ($line -match '^---- packaged batch ') { $batchHeader = $line; $batchSeen = $true; continue }

    # A new site begins at its goto -- but NOT at the shell's `ui goto ... done` echo, which names
    # the same URL and would otherwise double every row.
    if ($line -match '^goto (\S+)$') {
        $cur = [ordered]@{
            site = $Matches[1]; doc = ''; pct = ''; body = ''; rs = ''
            swipe = ''; back = ''; fwd = ''; hostidx = ''; restored = ''; restoredPct = ''
            h1 = ''; h2 = ''
            diags = 0
        }
        $rows.Add($cur)
        continue
    }
    if ($null -eq $cur) { continue }

    if ($line -match '^diag ') {
        # Two url= fields share the line: the shell's requested URL, then the engine's diag after
        # the ` | ` separator. Only the second names the loaded document -- and only if it is
        # ANCHORED, because `lasterr=[... url=...]` further down the same line carries another url=
        # that is the failed REQUEST. Anchoring matters most on the error page: its synthetic
        # document has an empty own url, and an unanchored match silently reported the lasterr url
        # as the document (measured 2026-10-20 on the .41 probe).
        $eng = if ($line -match '\| (url=.*)$') { $Matches[1] } else { $line }
        $cur.diags++
        $d = Get-Field $eng '^url=(\S*)'
        if ($d -eq '' -and $eng -match 'lasterr=\[[^\]]+\]') { $d = '(error page)' }
        $n = [regex]::Match($eng, 'nonwhite=(\d+)/(\d+)')
        $pct = if ($n.Success) {
            $t = [double]$n.Groups[2].Value
            if ($t -gt 0) { [string][math]::Round(100.0 * [double]$n.Groups[1].Value / $t, 2) } else { 'n/a' }
        } else { '' }
        if ($cur.diags -eq 1) {
            $cur.doc = $d; $cur.pct = $pct
            $cur.body = Get-Field $eng 'body=(-?\d+)'
            $cur.rs = Get-Field $eng 'rs=([^ ]+)'
        }
        else {
            $cur.restored = $d; $cur.restoredPct = $pct
        }
        continue
    }

    if ($line -match '^swipe end totalMs=(\d+)') { $cur.swipe = $Matches[1]; continue }
    if ($line -match '^history back rc=(-?\d+)') { $cur.back = $Matches[1]; continue }
    if ($line -match '^history fwd skipped') { $cur.fwd = 'skipped'; continue }
    if ($line -match '^history fwd rc=(-?\d+)') { $cur.fwd = $Matches[1]; continue }
    if ($line -match '^hist engine back=\d+ fwd=\d+ cached=\S+ host idx=(\d+)/(\d+)') {
        $cur.hostidx = ('{0}/{1}' -f $Matches[1], $Matches[2]); continue
    }

    if ($line -match '^ui settings-close') { $afterShellCheck = $true; continue }

    if ($line -match '^hash (0x[0-9a-fA-F]+)') {
        # Two hashes per site since 2026-10-20: one before the swipe and one after it. Equal
        # hashes across a swipe on a page taller than the viewport mean the gesture ran and the
        # frame did not move -- which `swipe end totalMs=` alone cannot tell you.
        if ($cur.h1 -eq '') { $cur.h1 = $Matches[1] }
        elseif ($cur.h2 -eq '') { $cur.h2 = $Matches[1] }
        # The list's last command is `hash` after `ui settings-close`; nothing after it belongs
        # to the batch, so reaching it is how this script knows the run finished. Anchoring on
        # the site URL alone would not do: site 13's own `hash` happens before its swipe.
        if ($afterShellCheck) { $batchDone = $true }
        continue
    }
}

if (-not $batchSeen) {
    Write-Host 'NO PACKAGED BATCH in this log -- the run never started (no batch in the appx, or the' -ForegroundColor Yellow
    Write-Host 'device-run.done marker already matched this version). Nothing below is a measurement.' -ForegroundColor Yellow
    Write-Host ''
}
else {
    Write-Host ("batch: {0}" -f $batchHeader) -ForegroundColor Cyan
    if (-not $batchDone) {
        Write-Host 'THE BATCH DID NOT REACH ITS LAST COMMAND -- the process died or was suspended mid-list.' -ForegroundColor Red
        Write-Host ("last row: {0}" -f $rows[$rows.Count - 1].site) -ForegroundColor Red
    }
    Write-Host ''
}

$fmt = '{0,-42} {1,-3} {2,7} {3,5} {4,-22} {5,6} {6,6} {7,7} {8,7} {9,-6} {10}'
$hdr = $fmt -f 'requested', 'n', 'pct', 'body', 'document', 'swipe', 'back', 'fwd', 'idx', 'moved', 'restored'
Write-Host $hdr -ForegroundColor Cyan
foreach ($r in $rows) {
    $docShort = $r.doc -replace '^https?://', ''
    if ($docShort.Length -gt 22) { $docShort = $docShort.Substring(0, 22) }
    # `moved` compares the frame hash taken before the swipe with the one after it.
    $moved = if ($r.h1 -eq '' -or $r.h2 -eq '') { '-' }
             elseif ($r.h1 -ne $r.h2) { 'yes' } else { 'NO' }
    Write-Host ($fmt -f $r.site, $r.diags, $r.pct, $r.body, $docShort, $r.swipe, $r.back, $r.fwd,
                       $r.hostidx, $moved, ($r.restoredPct + ' ' + ($r.restored -replace '^https?://', '')))
}

if ($Csv) {
    $rows | ForEach-Object { [pscustomobject]$_ } | Export-Csv -LiteralPath $Csv -NoTypeInformation -Encoding UTF8
    Write-Host ''
    Write-Host ("csv: {0}" -f $Csv)
}

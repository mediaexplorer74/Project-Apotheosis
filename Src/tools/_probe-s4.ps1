# _probe-s4.ps1 -- S4 empirical probe on the x64 bench (0.2.3.61), v2.
#
# S4 claims the address box shows destroy bytes (literal 0x3F) when the URL is
# unknown. A static audit of the whole C# line found NO path that can produce a
# literal 0x3F in UrlBox (UTF-8 marshalling everywhere, UTF-8 file reads, no
# destroy literals in tables/XAML/home/error pages). So the claim has to be
# measured, not argued.
#
# v1 drove the navs with `goto` from about:home -- and on this bench the home
# page re-renders ~10x/s, so every goto was QUEUED then superseded inside one
# frame (measured: 07:41:07.144 -> .252, 86-147 ms). The box stayed on the
# placeholder and the run measured nothing (memory: fitwindow-home-adopt-loop).
# v2 uses the documented way past it: the FIRST load comes from testurl.txt
# (drive-csharp -Url), which is read at startup, not posted against a busy
# shell. That puts the app on the error page (engine URL unknown) before any
# verb runs.
#
#   1. dead host  -- first load from testurl.txt; the port renders the error
#                    page via writer.begin(URL()), i.e. document.url() EMPTY;
#                    then tap the page (the path that reads WebCoreGetUrl into
#                    the box) and read the diag.
#   2. real page  -- -KeepState goto example.com (allowed: the error page does
#                    not re-render like home), then tap a non-link spot: the
#                    .58/.59 "empty url= after tap" case.
#
# Verdicts read from: the two PNGs (the box itself), autocmd-log.txt, stage.txt.
# A '?' RUN in the box = S4 confirmed and the source hunt resumes; empty box /
# previous URL = S4 not reproducible in the C# line, close with this evidence.
$ErrorActionPreference = 'Continue'
$root = 'C:\Users\media\source\repos\Vibe\Apotheosis'
$shotDir = Join-Path $root 'Src\tools\shots'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null

$drive = Join-Path $root 'Src\tools\drive-csharp.ps1'

# Batch 1: fresh launch; the dead host arrives as the first load via testurl.txt.
& $drive -Url 'https://nonexistent.invalid/' -BootMs 12000 `
    -Cmds 'wait 6000;diag;tap 400 400;wait 3000;diag' `
    -Shot (Join-Path $shotDir 's4-1-errorpage.png') -FitWindow
Write-Host '--- batch 1 done (error page + tap) ---'

# Batch 2: same process; a real page (the error page settles, so goto lands),
# then a tap on a non-link spot.
& $drive -KeepState -NoLaunch -AfterCmdsMs 7000 `
    -Cmds 'goto https://example.com/;wait 6000;diag;tap 60 700;wait 2000;diag' `
    -Shot (Join-Path $shotDir 's4-2-tap-nonlink.png') -FitWindow
Write-Host '--- batch 2 done (real page + non-link tap) ---'

$ls = Join-Path $env:LOCALAPPDATA 'Packages\ME.Apotheosis_cj4nrm0qt9s1y\LocalState'
Write-Host '--- autocmd tail ---'
Get-Content (Join-Path $ls 'autocmd-log.txt') -Tail 40 -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host $_ }

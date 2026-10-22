# _probe-s3.ps1 -- S3 empirical probe on the x64 bench (0.2.3.64).
#
# S3: the address bar must follow the ENGINE, not the shell's request.
#
# What the earlier attempts measured, and why they are not the right case:
#   - a server redirect (http:// -> https://) never reaches WebCore on this port:
#     curl follows it inside the fetcher and the port plants the REQUESTED url in
#     the DocumentLoader, so the engine diag reads `url=<requested>` even when the
#     document arrived from elsewhere (measured: example.com, w3.org, wikipedia).
#     "Redirects lie in the chrome" therefore cannot be reproduced through
#     document.url() -- the engine itself believes it is at the requested url.
#   - `taplink` goes through HandleTapAt, which reads the engine url right after
#     the click and writes it into the box itself; it cannot tell whether the
#     LIVE TICK follows anything.
#
# The honest instrument is `clicklink <n>` (added .64): WebCoreClickAt called
# directly on the engine thread, nothing written to the shell -- exactly the
# shape of an engine-initiated hop (an SPA route change, history.replaceState).
# The tick must notice the new document url and leave
#   url follows engine: <new url>
# in autocmd-log; the diag then names both urls, and the screenshot shows the box.
$ErrorActionPreference = 'Continue'
$root = 'C:\Users\media\source\repos\Vibe\Apotheosis'
$shotDir = Join-Path $root 'Src\tools\shots'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
$drive = Join-Path $root 'Src\tools\drive-csharp.ps1'

& $drive -Url 'http://example.com/' -BootMs 12000 `
    -Cmds 'wait 6000;links;clicklink 0;wait 8000;diag' `
    -Shot (Join-Path $shotDir 's3-engine-hop.png') -FitWindow

$ls = Join-Path $env:LOCALAPPDATA 'Packages\ME.Apotheosis_cj4nrm0qt9s1y\LocalState'
Write-Host '--- this run ---'
Get-Content (Join-Path $ls 'autocmd-log.txt') -Tail 40 -ErrorAction SilentlyContinue |
    Where-Object { $_ -match 'clicklink|follows engine|links|diag nav=' } |
    ForEach-Object { Write-Host $_.Substring(0, [Math]::Min(300, $_.Length)) }

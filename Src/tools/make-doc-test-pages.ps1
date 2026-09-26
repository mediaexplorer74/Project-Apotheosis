# make-doc-test-pages.ps1 -- discriminate "the port leaks Documents" from "the page's own JS caches
# Documents", with one respect changed at a time.
#
# Apotheosis 2026-09-24. Why it exists: `SL: docs after teardown total=` grows ~+4 documents per
# navigation of https://dzen.ru/ without bound, and it does NOT stop when the cache levers are armed.
# Observable in the same log: two documents per load with an empty url, one to two `about:blank`, and
# one whose url is `https://dzen.ru/` -- a whole page document, while `Page::nonUtilityPageCount()` is 0,
# so no non-utility Page owns it.
#
# The open question is the one the port's own comment names: does this port fail to drop a Document that
# nothing references any more, or does dzen.ru's own JavaScript keep parsed documents alive (which it
# would do in any browser, and which is not this project's defect)? Reading the port's sources answered
# nothing -- every `RefPtr<Document>` there is a scoped local -- so the question needs a page that
# differs in exactly the retention respect and in nothing else.
#
# The three arms, and the only difference between them is what the script does with the documents it
# creates: drop them, or keep them in `window.__keep`:
#   d0.html -- creates none                                   (control: the floor of the instrument)
#   d1.html -- creates 5 and keeps no reference to any       (THE QUESTION: dropped documents)
#   d2.html -- creates 5 and stores them in window.__keep    (positive control: MUST accumulate)
#
# d2 is not optional. Without it a flat d1 reading cannot be told apart from an instrument that does not
# work, which is the failure mode [[diagnostics-that-guess-will-lie]] is about.
#
# EVERY ARM ALSO APPENDS A MARKER ELEMENT PER DOCUMENT IT CREATES, and that is not decoration: the first
# version of these pages did not, d2 came back flat, and a flat positive control proves nothing about its
# own validity -- "the script never ran", "createHTMLDocument threw" and "the port collects documents
# promptly" all produce the same flat line. The markers make the page's own arithmetic readable out of
# the existing `diag:` line's `bodyKids=`, so execution and creation count are observed, not assumed.
# Expected `bodyKids`: d0 = 1, d1 = 6, d2 = 6 (one marker for the start, one per created document).
#
# Read the answer out of `SL: docs after teardown total=` in gpuinit-steps.txt, one line per navigation:
#   d0 flat, d1 flat, d2 grows by 5 per load -> documents are collected when JS drops them; the port is
#                                              not at fault, and dzen.ru's growth is the page's own.
#   d1 grows too                            -> this port fails to release dropped Documents, and the
#                                              leak is ours to fix.
#   d0 flat and d1 flat and BOTH d2 and bodyKids flat
#                                           -> the arm never ran; fix the page before reading anything
#                                              into the other two lines.
#
# One more reading to take from the same run, because it decides what `document_count` even means:
# `Document::allDocumentsMap()` is a HashMap keyed by `Document::identifier()`
# (`Document.cpp:632-637`), so two documents sharing an identifier occupy ONE slot. The first reading of
# this test left that open, so if d2's `bodyKids` proves six elements were appended while
# `SL: docs total` does not grow, the cause to check is a null or shared identifier, not collection.
param(
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'

if (-not $OutDir) {
    $pkg = Get-AppxPackage -Name EdgeHTMLReborn.Harness -ErrorAction SilentlyContinue
    if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
    $OutDir = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState\po"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Write-Page {
    param([string]$Name, [string]$Body)
    $html = @"
<!doctype html>
<html><head><meta charset="utf-8"><title>$Name</title></head>
<body>
<h1>$Name</h1>
<p>Document-retention arm. See Src\tools\make-doc-test-pages.ps1 for what this page is for.</p>
<script>
$Body
</script>
</body></html>
"@
    # ASCII only: the harness reads these through the engine's own loader, and an encoding accident on a
    # test page would be misread as an engine defect.
    $path = Join-Path $OutDir "$Name.html"
    [System.IO.File]::WriteAllText($path, $html, [System.Text.Encoding]::ASCII)
    Write-Host "wrote $path"
}

# Nothing created, nothing retained. One marker proves the script ran at all -- without it a flat
# document count on this arm cannot be told apart from a page whose script never executed.
$d0 = @'
window.__apoDocArm = 'd0-none';
function apoMark(t) { var i = document.createElement('i'); i.textContent = t; document.body.appendChild(i); }
apoMark('d0');
'@

# Five documents created and dropped: no reference survives the loop body.
$d1 = @'
window.__apoDocArm = 'd1-dropped';
function apoMark(t) { var i = document.createElement('i'); i.textContent = t; document.body.appendChild(i); }
apoMark('start');
for (var i = 0; i < 5; i++) {
    var d = document.implementation.createHTMLDocument('dropped-' + i);
    d.body.innerHTML = '<p>dropped ' + i + '</p>';
    apoMark('c' + i);
}
'@

# Five documents created and kept: the positive control. If THIS one does not accumulate, the probe or
# the arm is broken and the d1 reading means nothing.
# The marker is appended AFTER the push, so bodyKids=6 certifies both that the loop ran five times and
# that the references were stored.
$d2 = @'
window.__apoDocArm = 'd2-kept';
function apoMark(t) { var i = document.createElement('i'); i.textContent = t; document.body.appendChild(i); }
window.__keep = [];
apoMark('start');
for (var i = 0; i < 5; i++) {
    var d = document.implementation.createHTMLDocument('kept-' + i);
    d.body.innerHTML = '<p>kept ' + i + '</p>';
    window.__keep.push(d);
    apoMark('c' + i);
}
'@

Write-Page -Name 'd0' -Body $d0
Write-Page -Name 'd1' -Body $d1
Write-Page -Name 'd2' -Body $d2

Write-Host ''
Write-Host 'Drive with, e.g.:'
Write-Host ('  pwsh -File Src\tools\rss-loop.ps1 -Url "file:///<po>\d0.html" -Count 5')
Write-Host 'then read SL: docs after teardown total= in gpuinit-steps.txt.'

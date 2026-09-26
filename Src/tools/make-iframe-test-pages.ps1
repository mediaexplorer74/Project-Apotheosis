# make-iframe-test-pages.ps1 -- controlled pages for the retained-Document experiment.
#
# Apotheosis 2026-09-24. The question: `SL: docs` and `SL: mem` on 0.1.10.44 show dzen.ru retaining
# ~+3-4 whole Documents per load across a session teardown, of which several carry the page's own URL
# (`https://dzen.ru/`) rather than being blank -- i.e. candidates for retained same-origin subframe
# documents. document_count grows 3 -> 5 -> 11 while nonUtilityPageCount stays 1, so they are NOT the
# session Page and not the hidden SVG Pages (those are URL-less, and there are fewer of them).
#
# This script isolates the iframe from everything else, the same way make-svg-test-pages.ps1 isolated the
# SVG: the pages differ in exactly one respect, the number of same-origin iframes, and the children
# carry NO JavaScript at all. That last point is the important one -- it is what separates "the engine
# does not drop subframe documents it should" from "dzen.ru's own JS caches documents", and only the
# first is a port defect. A JS-free retained iframe document cannot be blamed on a script.
#
# Distinct child files rather than four references to one: an iframe is a separate Frame whatever its URL,
# so this is not required for correctness, but it keeps the byte content of the children different and
# removes any chance of a same-url shortcut in the frame or document caches.
#
# Writes into LocalState\po, beside the svg pages, and is driven by the existing runner:
#   pwsh -File Src\tools\svg-page-test.ps1 -Urls i0.html,i4.html,i0.html
param(
    [string]$LocalState = (Join-Path $env:LOCALAPPDATA "Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState")
)
$ErrorActionPreference = 'Stop'
$po = Join-Path $LocalState 'po'
New-Item -ItemType Directory -Force -Path $po | Out-Null

function Write-Child {
    param([string]$Name, [string]$Title)
    # Static markup only, and enough of it that the document has a real DOM and render tree: a retained
    # document of two nodes would not show up in RSS even if the retention is real, and the point of the
    # experiment is to be able to price the retention as well as count it.
    $body = (1..40 | ForEach-Object { "<p>child $Title paragraph $_ with some text to lay out</p>" }) -join ''
    $html = "<!doctype html><meta charset=utf-8><title>$Title</title><body><h1>$Title</h1>$body</body>"
    Set-Content -Path (Join-Path $po $Name) -Value $html -Encoding UTF8
}

1..4 | ForEach-Object { Write-Child "f$_.html" "child$_" }
Write-Child 'f.html' 'child'

function Write-Page {
    param([string]$Name, [string]$Title, [string]$Body)
    $html = "<!doctype html><meta charset=utf-8><title>$Title</title><body><h1>$Title</h1>$Body</body>"
    Set-Content -Path (Join-Path $po $Name) -Value $html -Encoding UTF8
}

Write-Page 'i0.html' 'i0 no iframe' '<p>plain page, zero iframes</p>'

# The control pair. i4 nests four children, i4d the same four distinct children -- so a difference between
# these two would mean the urls matter, which no part of the hypothesis predicts.
$four  = (1..4 | ForEach-Object { "<iframe src=`"f$_.html`" width=`"300`" height=`"200`"></iframe>" }) -join ''
$fourS = (1..4 | ForEach-Object { "<iframe src=`"f.html`" width=`"300`" height=`"200`"></iframe>" }) -join ''
Write-Page 'i4.html'  'i4 four iframes' $four
Write-Page 'i4s.html' 'i4s same url x4' $fourS

# The nested case: a child that itself nests a grandchild. If retention is per-frame rather than per-
# document, this separates "four frames" from "five frames one level deeper".
$deep = (1..2 | ForEach-Object { "<iframe src=`"f$_.html`" width=`"200`" height=`"150`"></iframe>" }) -join ''
$child2 = "<!doctype html><meta charset=utf-8><title>nest</title><body><h1>nest</h1>$deep</body>"
Set-Content -Path (Join-Path $po 'f9.html') -Value $child2 -Encoding UTF8
Write-Page 'i9.html' 'i9 nested iframes' '<iframe src="f9.html" width="400" height="300"></iframe>'

Get-ChildItem $po -Filter 'i*.html' | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "pages written to $po"
Write-Host "load with: pwsh -File Src\tools\svg-page-test.ps1 -Urls i0.html,i4.html,i0.html"

# make-svg-test-pages.ps1 -- build the controlled SVG pages for the live-Page experiment.
#
# Apotheosis 2026-09-24. The question this serves: the harness leaks one hidden whole Page per
# navigation on SVG-heavy pages (`SL: live pages` in gpuinit-steps.txt shows 2-3 anonymous Pages added
# per dzen.ru load, each surviving teardown). The leading hypothesis is SVGImage::m_page
# (WebCore/svg/graphics/SVGImage.cpp:497 -- a whole Page per SVG image, whose own upstream FIXME says
# the Cache code cannot see the cycle). This script produces pages that differ in exactly one respect,
# the number of EXTERNAL svg files, so the count can be read off directly.
#
# External files, not inline <svg>: an inline SVG element is part of the host document and creates no
# Page at all, so a test built that way would "refute" the hypothesis for a reason that has nothing to
# do with it. Ten DISTINCT urls, not one repeated ten times: identical urls collapse to a single
# CachedImage and would yield one Page, not ten.
#
# Writes into LocalState (the harness reads file:/// urls from there). Load them with nav.txt:
#   file:///C:/Users/<you>/AppData/Local/Packages/<pkg>/LocalState/po/t0.html
param(
    [string]$LocalState = (Join-Path $env:LOCALAPPDATA "Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState")
)
$ErrorActionPreference = 'Stop'
$po = Join-Path $LocalState 'po'
New-Item -ItemType Directory -Force -Path $po | Out-Null

$svg = '<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40"><circle cx="20" cy="20" r="18" fill="#3a7"/></svg>'

function Write-Page {
    param([string]$Name, [string]$Title, [string]$Body)
    $html = "<!doctype html><meta charset=utf-8><title>$Title</title><body><h1>$Title</h1>$Body</body>"
    Set-Content -Path (Join-Path $po $Name) -Value $html -Encoding UTF8
}

Write-Page 'icon.svg' 'icon' $svg
1..10 | ForEach-Object { Set-Content -Path (Join-Path $po "icon$_.svg") -Value $svg -Encoding UTF8 }

Write-Page 't0.html'  't0 no svg'  '<p>plain page, zero external svg</p>'
Write-Page 't1.html'  't1 one svg' '<img src="icon.svg" width="40" height="40">'
$imgs = (1..10 | ForEach-Object { "<img src=`"icon$_.svg`" width=`"40`" height=`"40`">" }) -join ''
Write-Page 't10.html' 't10 ten svg' $imgs

# The pair that tests the OTHER half: the same ten svg, differing only in whether page JS keeps a
# reference to the image elements. Measured on 0.1.10.41, the ten-svg page's hidden Pages were gone by
# the next teardown, while dzen.ru's - the one page here with heavy JS - kept every one of them through
# every following navigation. So the retention is not the image cache as such: something outside it holds
# the CachedImage, and a JS-reachable HTMLImageElement is the obvious candidate. tjs holds, tjs2 does
# not, and the two are otherwise byte-identical apart from the script.
Write-Page 'tjs2.html' 'tjs2 svg no js hold' $imgs
Write-Page 'tjs.html'  'tjs svg + js hold'  ($imgs + '<script>window.__keep = [].slice.call(document.images);</script>')

# Two more loaders for the same ten files, because "<img> svg" is only one of the ways a page reaches an
# svg, and dzen.ru reaches them mostly from CSS. A CSS-loaded svg is held by a StyleImage/CSSImageValue in
# the style system rather than by an HTMLImageElement, i.e. by a structure with a different lifetime - and
# the earlier test already killed the HTMLImageElement explanation (tjs survives exactly like tjs2).
$divs = (1..10 | ForEach-Object { "<div style=`"width:40px;height:40px;background-image:url(icon$_.svg)`"></div>" }) -join ''
Write-Page 'tcss.html' 'tcss svg via css' $divs

# And the nested case, which is the one upstream's own FIXME names: an svg that loads another svg. Each of
# the ten files here references a distinct child, so a hidden Page that serves an image inside itself is
# distinguishable from a flat one.
1..10 | ForEach-Object {
    $child = if ($_ -lt 10) { $_ + 1 } else { 1 }
    $nested = "<svg xmlns=`"http://www.w3.org/2000/svg`" xmlns:xlink=`"http://www.w3.org/1999/xlink`" width=`"40`" height=`"40`"><image xlink:href=`"nest$child.svg`" width=`"40`" height=`"40`"/></svg>"
    Set-Content -Path (Join-Path $po "nest$_.svg") -Value $nested -Encoding UTF8
}
$nests = (1..10 | ForEach-Object { "<img src=`"nest$_.svg`" width=`"40`" height=`"40`">" }) -join ''
Write-Page 'tnest.html' 'tnest nested svg' $nests

Get-ChildItem $po | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "pages written to $po"
Write-Host "load with: file:///$($po -replace '\\','/')/t0.html"

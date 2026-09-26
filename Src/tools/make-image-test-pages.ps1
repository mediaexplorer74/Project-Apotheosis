# make-image-test-pages.ps1 -- controlled pages that price the DECODED image store.
#
# Apotheosis 2026-09-24. The question this serves. Everything else visible has been priced or saturated:
# the JSC heap freezes at 113 MB, document_count freezes at 11, the hidden per-SVG Pages cost ~0.4 MB
# each, and MemoryCache's own accounting is 1.4 MB of images on a dzen.ru load. Yet RSS grows ~141 MB per
# dzen.ru navigation, and the ONE lever that halves it is MemoryCache::evictResources() -- which destroys
# the CachedImage objects themselves rather than freeing any bytes the cache counts. Combined with
# `decodedSize = 0` for images on every load (the cache is never told about decoded bitmap data, so those
# images never enter m_liveDecodedResources and no release path can reach them), the standing hypothesis is
# that decoded image data is resident and invisible to every instrument in this port.
#
# The design. A grid that fits inside the viewport, so the whole thing is painted and therefore decoded:
#   g0.html    -- the grid, no images at all          (control: the page itself)
#   gbig.html  -- 20 files of 1200x1200, drawn 200x100 (decoded ~5.76 MB each, ~115 MB on screen)
#   gtiny.html -- 20 files of 32x32,     drawn 200x100 (decoded ~4 KB each)   (control)
#
# gbig and gtiny differ in EXACTLY ONE respect: the intrinsic pixel count of the referenced files. Same
# DOM shape, same element count, same 20 requests, same layout boxes, same painted area, same CSS. That is
# the discipline this project keeps having to relearn -- a page that merely *fits* a hypothesis has been
# wrong four times here.
#
# PNG, not BMP, and the gradients are deliberate: a 1200x1200 gradient compresses to tens of kilobytes, so
# the ENCODED bytes stay trivial while the DECODED bytes stay enormous. With BMP the two would move
# together and the test could not tell "encoded bytes are held" from "decoded pixels are held".
#
# 20 distinct urls, not one repeated: identical urls collapse to a single CachedImage and would decode once.
#
# Writes into LocalState (the harness reads file:/// urls). Load with nav.txt, e.g.
#   file:///C:/Users/<you>/AppData/Local/Packages/<pkg>/LocalState/po/gbig.html
param(
    [string]$LocalState = (Join-Path $env:LOCALAPPDATA "Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState"),
    [int]$Count = 20
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$po = Join-Path $LocalState 'po'
New-Item -ItemType Directory -Force -Path $po | Out-Null

# A diagonal gradient. Cheap for a PNG encoder to compress (every row is a smooth ramp), expensive to
# decode (1200*1200*4 bytes of ARGB), and the hue varies per file so no two files are byte-identical.
function Write-GradientPng {
    param([string]$Path, [int]$Size, [int]$Hue)
    $bmp = New-Object System.Drawing.Bitmap $Size, $Size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $Size, $Size
    $c1 = [System.Drawing.Color]::FromArgb(255, (($Hue * 37) % 256), (($Hue * 71) % 256), (($Hue * 113) % 256))
    $c2 = [System.Drawing.Color]::FromArgb(255, (($Hue * 149) % 256), (($Hue * 211) % 256), (($Hue * 53) % 256))
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $c1, $c2, 45.0
    $g.FillRectangle($brush, $rect)
    $brush.Dispose(); $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

Write-GradientPng (Join-Path $po 'tiny.png') 32 1
for ($i = 1; $i -le $Count; $i++) { Write-GradientPng (Join-Path $po "tiny$i.png") 32 ($i + 1) }
for ($i = 1; $i -le $Count; $i++) { Write-GradientPng (Join-Path $po "big$i.png") 1200 ($i + 100) }

function Write-Page {
    param([string]$Name, [string]$Title, [string]$Body)
    $html = "<!doctype html><meta charset=utf-8><title>$Title</title><body style=`"margin:0`"><h1 style=`"font-size:14px;margin:2px`">$Title</h1>$Body</body>"
    Set-Content -Path (Join-Path $po $Name) -Value $html -Encoding UTF8
}

# One cell per image: 5 across x 4 down, 200x100 each = 1000x400, which fits the bench viewport on both the
# software and the GPU line, so every image is inside the painted area and therefore decoded.
function Grid-Body {
    param([string]$Prefix)
    $cells = (1..$Count | ForEach-Object { "<div style=`"display:inline-block;width:200px;height:100px`"><img src=`"$Prefix$_.png`" style=`"width:200px;height:100px`"></div>" }) -join ''
    return "<div style=`"width:1000px`">$cells</div>"
}

Write-Page 'g0.html'    'g0 grid, no images' (Grid-Body 'x-nothing-')
Write-Page 'gbig.html'  'gbig 20 x 1200x1200 drawn 200x100' (Grid-Body 'big')
Write-Page 'gtiny.html' 'gtiny 20 x 32x32 drawn 200x100'     (Grid-Body 'tiny')

$enc = (Get-ChildItem $po -Filter 'big*.png' | Measure-Object -Property Length -Sum).Sum
Write-Host ("big files: {0} x 1200x1200, encoded total {1:N0} bytes ({2:N1} KB each)" -f $Count, $enc, ($enc / $Count / 1KB))
Write-Host ("  decoded if fully decoded: {0:N1} MB" -f ($Count * 1200 * 1200 * 4 / 1MB))
$tenc = (Get-ChildItem $po -Filter 'tiny*.png' | Measure-Object -Property Length -Sum).Sum
Write-Host ("tiny files: {0}, encoded total {1:N0} bytes; decoded {2:N0} bytes" -f $Count, $tenc, ($Count * 32 * 32 * 4))
Write-Host "pages written to $po"
Write-Host "load with: file:///$($po -replace '\\','/')/gbig.html"

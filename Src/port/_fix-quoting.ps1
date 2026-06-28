# _fix-quoting.ps1 — заменить одиночные кавычки на двойные вокруг $env:APOTHEOSIS_*
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir "..\..")

$files = @()
$files += Get-ChildItem "$Root\Src\port\*.ps1" -File | Select-Object -ExpandProperty FullName
$files += Get-ChildItem "$Root\Src\tools\*.ps1" -File | Select-Object -ExpandProperty FullName

$totalFixed = 0
# Шаблон: 'слеш$env:APOTHEOSIS_ИМЯ\возможный\путь'
$re = [regex]@'
'(\$env:APOTHEOSIS_\w+(?:\\[^'"]*)?)'
'@

foreach ($file in $files) {
    $content = Get-Content -LiteralPath $file -Raw
    $original = $content
    $content = $re.Replace($content, '"$1"')
    if ($content -ne $original) {
        $count = ($re.Matches($original) | Measure-Object).Count
        $totalFixed += $count
        Set-Content -LiteralPath $file -Value $content -NoNewline
        Write-Host "FIXED ($count): $file" -ForegroundColor Green
    }
}

Write-Host "`nDone! Total quoting fixes: $totalFixed" -ForegroundColor Cyan

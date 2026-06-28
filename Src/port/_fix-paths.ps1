# _fix-paths.ps1 — заменить $env:APOTHEOSIS_ROOT\ на $env:APOTHEOSIS_* во всех .ps1 и .bat
# Однократный запуск после создания Src/setenv.ps1

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir "..\..")

# Словарь замен: подстрока → замена (сохраняет кавычки и контекст)
# Порядок важен: более специфичные раньше
$replacements = @(
    @{pattern="$env:APOTHEOSIS_PORT\";     replacement="$env:APOTHEOSIS_PORT\"}
    @{pattern="$env:APOTHEOSIS_HARNESS\";  replacement="$env:APOTHEOSIS_HARNESS\"}
    @{pattern="$env:APOTHEOSIS_TOOLS\";    replacement="$env:APOTHEOSIS_TOOLS\"}
    @{pattern="$env:APOTHEOSIS_ANGLE\";    replacement="$env:APOTHEOSIS_ANGLE\"}
    @{pattern="$env:APOTHEOSIS_CRASH\";    replacement="$env:APOTHEOSIS_CRASH\"}
    @{pattern="$env:APOTHEOSIS_ROOT\";          replacement="$env:APOTHEOSIS_ROOT\"}
)

# Файлы для обработки
$files = @()
$files += Get-ChildItem "$Root\Src\port\*.ps1" -File | Select-Object -ExpandProperty FullName
$files += Get-ChildItem "$Root\Src\tools\*.ps1" -File | Select-Object -ExpandProperty FullName
$files += Get-ChildItem "$Root\Src\port\*.bat" -File | Select-Object -ExpandProperty FullName

$totalReplaced = 0
foreach ($file in $files) {
    $content = Get-Content -LiteralPath $file -Raw
    $original = $content
    foreach ($r in $replacements) {
        $content = $content -replace [regex]::Escape($r.pattern), $r.replacement
    }
    if ($content -ne $original) {
        $replaced = (Compare-Object ([regex]::Split($original, "`n")) ([regex]::Split($content, "`n")) | 
                     Where-Object { $_.SideIndicator -eq '<=' }).Count
        $totalReplaced += $replaced
        Set-Content -LiteralPath $file -Value $content -NoNewline
        Write-Host "FIXED ($replaced): $file" -ForegroundColor Green
    } else {
        Write-Host "SKIP (no changes): $file" -ForegroundColor DarkGray
    }
}

Write-Host "`nDone! Total replacements: $totalReplaced" -ForegroundColor Cyan

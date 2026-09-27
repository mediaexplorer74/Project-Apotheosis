param(
    # Сайт для проверки. Будет записан в LocalState\testurl.txt.
    [Parameter(Mandatory = $true)][string]$Url,
    # Сколько секунд ждать отрисовки перед снимком.
    [int]$WaitSeconds = 22,
    # Куда положить PNG. По умолчанию — во временный каталог.
    [string]$Shot = "$env:TEMP\shot.png"
)

# Прогон одного сайта: перезапуск приложения с testurl.txt -> ожидание -> снимок экрана.
#
# В C#-линии нет наблюдателя за nav.txt (он остался в старом C++ harness,
# см. Doc/Archive/BLANK-SECOND-LOAD.md), зато есть testurl.txt: MainPage читает
# его один раз при старте и уходит на NavigateTo. Поэтому каждая проверка
# требует перезапуска -- это медленнее, но совершенно воспроизводимо.
#
# Диагностику для сравнения смотрите в LocalState: diag.txt, layertree.txt,
# stage.txt, port-trace.txt.

$ErrorActionPreference = 'Stop'
$pkg = Get-AppxPackage -Name '*Apotheosis*'
if (-not $pkg) { throw 'Apotheosis не установлен' }
$ls = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState')

Get-Process -Name 'Apotheosis' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# Старый testurl.txt сработал бы на запуске повторно, поэтому затираем его историю:
# удаляем прочие файлы состояния, чтобы в логах не смешались прошлые прогоны.
Get-ChildItem $ls -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne 'testurl.txt' } |
    Remove-Item -Force -ErrorAction SilentlyContinue

Set-Content -LiteralPath (Join-Path $ls 'testurl.txt') -Value $Url -Encoding ASCII
"nav -> $Url"

$app = ($pkg | Get-AppxPackageManifest).Package.Applications.Application | Select-Object -First 1
Start-Process explorer.exe -ArgumentList "shell:AppsFolder\$($pkg.PackageFamilyName)!$($app.Id)"

Start-Sleep -Seconds $WaitSeconds
$proc = Get-Process -Name 'Apotheosis' -ErrorAction SilentlyContinue
if ($proc) { "жив: PID=$($proc.Id) RAM=$([math]::Round($proc.WorkingSet64/1MB,0)) МБ" }
else { 'ВНИМАНИЕ: процесс не найден' }

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object Drawing.Bitmap $b.Width, $b.Height
$g = [Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
$bmp.Save($Shot, [Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"снимок: $Shot"

# Что приложение успело записать -- это дешевле и точнее, чем разглядывать снимок.
foreach ($fn in 'stage.txt', 'diag.txt', 'layertree.txt', 'port-trace.txt') {
    $p = Join-Path $ls $fn
    if (Test-Path $p) {
        "---------- $fn ----------"
        Get-Content $p -Tail 25
    }
}
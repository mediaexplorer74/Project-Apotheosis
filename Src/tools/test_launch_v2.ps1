param(
    [string]$Aumid = "EdgeHTMLReborn.Harness_edmb40rfkwsbg!App"
)

# Force re-register the app to ensure it's properly activated
$package = Get-AppxPackage -Name "EdgeHTMLReborn.Harness"
Write-Output "Package: $($package.PackageFullName)"
Write-Output "InstallLocation: $($package.InstallLocation)"

# Check if exe exists
$exe = Join-Path $package.InstallLocation "Harness.exe"
Write-Output "Exe exists: $(Test-Path $exe)"
Write-Output "Exe size: $((Get-Item $exe).Length / 1KB) KB"

# Check WebCore.dll in install dir
$wcdll = Join-Path $package.InstallLocation "WebCore.dll"
Write-Output "WebCore.dll exists: $(Test-Path $wcdll)"
Write-Output "WebCore.dll size: $((Get-Item $wcdll).Length / 1KB) KB"

# Also check JavaScriptCore.dll 
$jscdll = Join-Path $package.InstallLocation "JavaScriptCore.dll"
Write-Output "JavaScriptCore.dll exists: $(Test-Path $jscdll)"

# Try using shell:AppsFolder explicitly 
Write-Output "`n--- Launching via explorer shell:AppsFolder ---"
$startTime = Get-Date
explorer.exe "shell:AppsFolder\$Aumid"

Start-Sleep -Seconds 20

$proc = Get-Process -Name "Harness" -ErrorAction SilentlyContinue
if ($proc) {
    Write-Output "Harness.exe process found! PID=$($proc.Id) Title='$($proc.MainWindowTitle)' WS=$($proc.WorkingSet64 / 1MB)MB"
} else {
    Write-Output "No Harness.exe process found after 20s"
}

Write-Output "`n=== AppModel events since $($startTime.ToString('HH:mm:ss')) ==="
Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-AppModel-Runtime/Admin'; Id=39,41} -MaxEvents 10 2>&1 | Where-Object { $_.TimeCreated -gt $startTime } | Format-Table TimeCreated, Id, Message -Wrap | Out-String -Width 120

# Check for application errors
Write-Output "`n=== Windows Error Reporting events ==="
Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000,1001,1026} -MaxEvents 20 2>&1 | Where-Object { $_.TimeCreated -gt $startTime } | ForEach-Object {
    Write-Output "Time: $($_.TimeCreated) Id=$($_.Id)"
    $_.Message -split "`n" | Select-Object -First 3
}

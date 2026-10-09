<#
.SYNOPSIS
    Launch the installed Apotheosis app on the Lumia over Windows Device Portal.

.DESCRIPTION
    Nothing in the repo could start the *remote* package: drive-csharp.ps1 launches through the local
    shell (shell:AppsFolder\...), which only works for the x64 line installed on the bench, and
    deploy-launch.ps1 starts the app but has the harness's package name and publisher baked in as
    constants. This resolves the package over WDP instead, so it keeps working across a version change
    -- the same reason pull-device-logs.ps1 asks the device rather than taking -Ver and -Pub.

    The launch itself is one POST. The endpoint takes both identifiers BASE64-encoded:
        POST /api/taskmanager/app?appid=<b64(PackageRelativeId)>&package=<b64(PackageFullName)>
    where PackageRelativeId is the app user model id and already carries its `!App` suffix.

    IMPORTANT -- the screen lock. An app started over Device Portal while the phone is locked never
    gets foreground focus; the OS suspends it within seconds and then reaps it, so the run writes few
    or no markers and looks exactly like a hang or a startup crash. Confirm the phone is unlocked
    before reading anything into a quiet log. (The harness records the same situation as
    `CRASHVERDICT: clean-exit`, because a suspend-then-terminate is a clean shutdown.)

.PARAMETER Ip
    Device IP. Defaults to 192.168.1.35, the Lumia 950 this repo is developed against.

.PARAMETER Scheme / Port
    Default https / 443. Windows 10 Mobile also exposes http / 80, which has no certificate to ignore.

.PARAMETER NameMatch
    Substring that identifies the package. Defaults to ME.Apotheosis.

.PARAMETER Credential
    WDP Basic credentials, if the device has them set. This one does not.

.PARAMETER WaitSec
    Seconds to wait after the launch before reporting, so the caller's next step reads a log that has
    something in it. Zero to return immediately.

.EXAMPLE
    pwsh -File Src/tools/device-launch.ps1
    pwsh -File Src/tools/device-launch.ps1 -Ip 192.168.1.35 -WaitSec 20
#>
[CmdletBinding()]
param(
    [string]$Ip = '192.168.1.35',
    [ValidateSet('https', 'http')][string]$Scheme = 'https',
    [int]$Port = 443,
    [string]$NameMatch = 'ME.Apotheosis',
    [pscredential]$Credential,
    [int]$WaitSec = 15
)

$ErrorActionPreference = 'Stop'
if ($Scheme -eq 'http' -and -not $PSBoundParameters.ContainsKey('Port')) { $Port = 80 }
$BaseUri = "${Scheme}://${Ip}:${Port}"

# WDP needs a CSRF token on state-changing requests: it arrives in a Set-Cookie on any GET and is
# echoed back in the X-CSRF-Token header. PowerShell's cookie jar does not keep WDP's Set-Cookie
# intact, so the token is lifted from the response headers directly.
$script:Session = $null
$script:Csrf = $null

function Update-Csrf($resp) {
    $sc = $resp.Headers['Set-Cookie']
    if (-not $sc) { return }
    foreach ($line in @($sc)) {
        if ($line -match 'CSRF-Token=([^;,\s]+)') {
            $script:Csrf = $Matches[1]
            if ($script:Session) {
                try { $script:Session.Cookies.Add([System.Net.Cookie]::new('CSRF-Token', $script:Csrf, '/', $Ip)) } catch {}
            }
        }
    }
}

function Invoke-Wdp {
    param([string]$Method = 'GET', [string]$Path)
    $params = @{
        Uri                      = "$BaseUri$Path"
        Method                   = $Method
        SkipCertificateCheck     = $true
        ConnectionTimeoutSeconds = 30
    }
    if ($script:Session) { $params.WebSession = $script:Session } else { $params.SessionVariable = 'newSession' }
    if ($Credential) { $params.Authentication = 'Basic'; $params.Credential = $Credential; $params.AllowUnencryptedAuthentication = ($Scheme -eq 'http') }
    if ($Method -in 'POST', 'DELETE') {
        if (-not $script:Csrf -and $script:Session) {
            try { Invoke-Wdp -Method GET -Path '/api/os/info' | Out-Null } catch {}
        }
        if ($script:Csrf) { $params.Headers = @{ 'X-CSRF-Token' = $script:Csrf } }
    }
    $resp = Invoke-WebRequest @params
    if ($newSession) { $script:Session = $newSession }
    Update-Csrf $resp
    if ($resp.Content) { try { return $resp.Content | ConvertFrom-Json } catch { return $resp.Content } }
    return $resp
}

$info = Invoke-Wdp -Method GET -Path '/api/os/info'
Write-Host "Device : $($info.ComputerName)  OS $($info.OsVersion)" -ForegroundColor Green

# Resolve the package rather than trusting a constant: a reinstall changes the version segment of the
# full name, and the caller should not have to know it.
#
# Match on all three identifiers, because `Name` is NOT the package family name: WDP reports the
# manifest's *display* name there, which for this app is plain `Apotheosis` while the family is
# `ME.Apotheosis`. Filtering on Name alone found nothing on the first real device run even though the
# package was installed -- measured 2026-10-03.
$pkgs = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
$hit = @($pkgs.InstalledPackages | Where-Object {
    $_.Name -like "*$NameMatch*" -or
    $_.PackageFamilyName -like "*$NameMatch*" -or
    $_.PackageFullName -like "*$NameMatch*"
})
if ($hit.Count -eq 0) { throw "no installed package matches '$NameMatch'" }
if ($hit.Count -gt 1) {
    Write-Host "  $($hit.Count) packages match; using the first" -ForegroundColor Yellow
}
$p = $hit[0]
Write-Host "Package: $($p.PackageFullName)"

# The taskmanager endpoint wants base64, not URL-escaped text, for both identifiers.
#
# appid is the *application user model id*, i.e. `<PackageRelativeId>!App`. WDP already reports that
# whole string in `PackageRelativeId`, `!App` included -- so it is passed as-is. Do NOT prefix the
# family name: `PackageFamilyName + '!' + PackageRelativeId` double-counts the suffix and the device
# rejects it with -2147024809 E_INVALIDARG "Failed to start the app" (measured 2026-10-03). The
# `!App` fallback only covers a device that reports the bare relative id.
$appUserId = if ($p.PackageRelativeId -like '*!*') { $p.PackageRelativeId } else { "$($p.PackageRelativeId)!App" }
$appId = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($appUserId))
$full  = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($p.PackageFullName))
$path  = "/api/taskmanager/app?appid=$([uri]::EscapeDataString($appId))&package=$([uri]::EscapeDataString($full))"
$r = Invoke-Wdp -Method POST -Path $path
$code = if ($r -is [int]) { $r } elseif ($r.StatusCode) { [int]$r.StatusCode } else { 'sent' }
Write-Host "Launch : HTTP $code" -ForegroundColor Green
Write-Host "  The phone must be UNLOCKED: a launch onto a locked screen is suspended and reaped, and the" -ForegroundColor DarkYellow
Write-Host "  run then writes no markers -- indistinguishable from a hang. Check the log, not this exit." -ForegroundColor DarkYellow

if ($WaitSec -gt 0) {
    Write-Host "Waiting ${WaitSec}s for startup markers..."
    Start-Sleep -Seconds $WaitSec
}

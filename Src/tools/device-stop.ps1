<#
.SYNOPSIS
    Stop the installed Apotheosis app on the Lumia over Windows Device Portal.

.DESCRIPTION
    The counterpart to device-launch.ps1: launch is a POST to /api/taskmanager/app, terminate is a
    DELETE on the same endpoint. Two details cost real time to find, both measured 2026-10-20:

      * The `package` parameter must be the BASE64 of the package FULL name. The plain family name
        returns HTTP 500; a stale CSRF token returns 403.
      * WDP requires the CSRF token in the X-CSRF-Token header for DELETE. It arrives in a
        Set-Cookie on any GET response and must be lifted from the headers -- PowerShell's cookie
        jar does not keep it.

    Why a script and not `Stop-Process` on a debugger: killing a tool kills what it holds. On the
    bench that rule is about cdb (use `.detach`); here it is about the app itself -- a WDP DELETE is
    an orderly terminate, and it is also the only way to make LocalState files readable in full,
    because the process holds them open while it runs (see pull-device-logs.ps1).

.PARAMETER Ip
    Device IP. Defaults to 192.168.1.35, the Lumia 950 this repo is developed against.

.PARAMETER NameMatch
    Substring that identifies the package. Defaults to ME.Apotheosis.

.EXAMPLE
    pwsh -File Src\tools\device-stop.ps1
    pwsh -File Src\tools\device-stop.ps1 -Ip 192.168.1.35
#>
[CmdletBinding()]
param(
    [string]$Ip = '192.168.1.35',
    [ValidateSet('https', 'http')][string]$Scheme = 'https',
    [int]$Port = 443,
    [string]$NameMatch = 'ME.Apotheosis',
    [pscredential]$Credential
)

$ErrorActionPreference = 'Stop'
if ($Scheme -eq 'http' -and -not $PSBoundParameters.ContainsKey('Port')) { $Port = 80 }
$BaseUri = "${Scheme}://${Ip}:${Port}"

# See device-launch.ps1 for why the token is lifted from the headers rather than a cookie jar.
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

# Resolve the installed package instead of trusting a constant: the version segment of the full
# name changes with every deploy.
$pkgs = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
$hit = @($pkgs.InstalledPackages | Where-Object {
    $_.Name -like "*$NameMatch*" -or
    $_.PackageFamilyName -like "*$NameMatch*" -or
    $_.PackageFullName -like "*$NameMatch*"
})
if ($hit.Count -eq 0) { throw "no installed package matches '$NameMatch'" }
if ($hit.Count -gt 1) { Write-Host "  $($hit.Count) packages match; using the first" -ForegroundColor Yellow }
$p = $hit[0]
Write-Host "Package: $($p.PackageFullName)"

# A plain name returns HTTP 500; the endpoint wants base64 of the full name.
$full = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($p.PackageFullName))
$path = "/api/taskmanager/app?package=$([uri]::EscapeDataString($full))"
try {
    $r = Invoke-Wdp -Method DELETE -Path $path
    $code = if ($r -is [int]) { $r } elseif ($r.StatusCode) { [int]$r.StatusCode } else { 'sent' }
    Write-Host "Stop   : HTTP $code" -ForegroundColor Green
}
catch {
    $st = $_.Exception.Response.StatusCode.value__
    throw "stop failed (HTTP $st): $($_.Exception.Message)"
}

# Confirm the process really went away, and read the field WDP actually reports it under. The
# processes JSON names it `ImageName`; filtering on `Name` matches nothing and would "prove" the
# app is gone even when it is not (measured 2026-10-20).
Start-Sleep -Seconds 3
$procs = Invoke-Wdp -Method GET -Path '/api/resourcemanager/processes'
$alive = @($procs.Processes | Where-Object { $_.ImageName -like '*Apotheosis*' })
if ($alive.Count -eq 0) {
    Write-Host 'Process: gone' -ForegroundColor Green
}
else {
    $ids = ($alive | ForEach-Object { "$($_.ImageName) pid=$($_.ProcessId)" }) -join ', '
    Write-Host "Process: STILL RUNNING -- $ids" -ForegroundColor Red
    exit 1
}

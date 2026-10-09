#Requires -Version 7.0
<#
.SYNOPSIS
    通过 Windows Device Portal (WDP) 把 appx/msix 推送并安装到 Windows 10 Mobile 设备。

.DESCRIPTION
    针对 Lumia 950 (Win10M) 的开发部署工具。封装 WDP 的 REST API:
      - 连通性/设备信息: GET /api/os/info
      - 安装:           POST /api/app/packagemanager/package?package=<file>   (multipart)
      - 安装状态:        GET  /api/app/packagemanager/state
      - 已装列表:        GET  /api/app/packagemanager/packages
      - 卸载:           DELETE /api/app/packagemanager/package?package=<FullName>
    自动处理 CSRF token(WDP 用 CSRF-Token cookie + X-CSRF-Token 头)与自签名证书。

.PARAMETER Appx
    要安装的主包路径 (.appx / .appxbundle / .msix / .msixbundle)。

.PARAMETER Dependencies
    依赖包路径数组(如 Microsoft.VCLibs ARM)。可选。

.PARAMETER Certificate
    侧载签名证书 (.cer)。首次侧载某证书时需要。可选。

.PARAMETER DeviceIp
    设备内网 IP。默认 192.168.3.51。

.PARAMETER Scheme / Port
    默认 https / 443。Win10M 也常用 http / 80(无证书烦恼但明文)。

.PARAMETER Credential
    WDP 若设了用户名/密码,用 -Credential 传 Basic 认证。

.PARAMETER List
    只列出设备上已安装的包,不部署。

.PARAMETER Uninstall
    传入 PackageFullName 执行卸载,不部署。

.EXAMPLE
    .\Wdp-Deploy.ps1 -Appx .\harness\AppPackages\Harness_ARM.appxbundle `
                     -Dependencies .\harness\AppPackages\Microsoft.VCLibs.ARM.appx

.EXAMPLE
    .\Wdp-Deploy.ps1 -List
    .\Wdp-Deploy.ps1 -Uninstall EdgeHTMLReborn.Harness_1.0.0.0_arm__abc123
#>
[CmdletBinding(DefaultParameterSetName = 'Deploy')]
param(
    [Parameter(ParameterSetName = 'Deploy', Mandatory, Position = 0)]
    [string]$Appx,

    [Parameter(ParameterSetName = 'Deploy')]
    [string[]]$Dependencies = @(),

    [Parameter(ParameterSetName = 'Deploy')]
    [string]$Certificate,

    [Parameter(ParameterSetName = 'List')]
    [switch]$List,

    [Parameter(ParameterSetName = 'Uninstall', Mandatory)]
    [string]$Uninstall,

    [string]$DeviceIp = '192.168.3.51',
    [ValidateSet('https', 'http')]
    [string]$Scheme = 'https',
    [int]$Port = 443,
    [pscredential]$Credential,
    # How long the whole package upload may take. The phone's WiFi is slow and variable, and the
    # appx is tens of MB, so the default is generous: this is a safety net for a dead link, not a
    # schedule. It is separate from -TimeoutSec, which bounds only the install-state polling that
    # follows the upload.
    [int]$UploadTimeoutSec = 1800,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = 'Stop'

# http 默认走 80(除非用户显式给了 -Port)
if ($Scheme -eq 'http' -and -not $PSBoundParameters.ContainsKey('Port')) { $Port = 80 }

$BaseUri = "${Scheme}://${DeviceIp}:${Port}"

# ---------------------------------------------------------------------------
# 公共请求参数:跳过自签名证书校验 + 共享会话(带 cookie)+ 可选 Basic 认证
# ---------------------------------------------------------------------------
$script:Session = $null
$script:Csrf = $null   # WDP CSRF-Token:PS 的 cookie 容器收不下 WDP 的 Set-Cookie,直接从响应头抠

function Update-Csrf($resp) {
    $sc = $resp.Headers['Set-Cookie']
    if ($sc) {
        foreach ($line in @($sc)) {
            if ($line -match 'CSRF-Token=([^;,\s]+)') {
                $script:Csrf = $Matches[1]
                if ($script:Session) {
                    try { $script:Session.Cookies.Add([System.Net.Cookie]::new('CSRF-Token', $script:Csrf, '/', $DeviceIp)) } catch {}
                }
            }
        }
    }
}

function Invoke-Wdp {
    param(
        [string]$Method = 'GET',
        [string]$Path,
        [switch]$Raw
    )
    # Only GET/DELETE go through here now, so the bound stays short: a dead host must fail fast.
    #
    # The UPLOAD deliberately does not use Invoke-WebRequest -- see Send-WdpUpload. Two measured
    # reasons, both found on 2026-10-03 while pushing a 45 MB ARM bundle:
    #   * On PowerShell 7.6, -TimeoutSec is an ALIAS of -ConnectionTimeoutSeconds (Get-Command proves
    #     it), and that value becomes HttpClient.Timeout, i.e. it bounds the WHOLE request, not just
    #     the connect. At 15, the upload died after the connect had already succeeded; and
    #     -OperationTimeoutSeconds does NOT extend it -- setting it while the other stays 15 still
    #     fails with the same 15-second message.
    #   * Even with the timeout fixed, Invoke-WebRequest -Form's multipart encoding is rejected by this
    #     device's WDP with `{ "Reason": "Missing .appx (or .eappx) file in uploaded files" }`. A raw
    #     HttpClient multipart of the same five files is accepted with HTTP 202.
    $args = @{
        Uri                      = "$BaseUri$Path"
        Method                   = $Method
        SkipCertificateCheck     = $true
        ConnectionTimeoutSeconds = 15
    }
    if ($script:Session) { $args.WebSession = $script:Session }
    else { $args.SessionVariable = 'newSession' }
    if ($Credential) { $args.Authentication = 'Basic'; $args.Credential = $Credential; $args.AllowUnencryptedAuthentication = ($Scheme -eq 'http') }

    # 改状态的请求 (POST/DELETE) 需要带 CSRF token
    if ($Method -in 'POST', 'DELETE') {
        $token = Get-CsrfToken
        if ($token) { $args.Headers = @{ 'X-CSRF-Token' = $token } }
    }

    $resp = Invoke-WebRequest @args
    if ($newSession) { $script:Session = $newSession }  # 首次建会话后保存
    Update-Csrf $resp

    if ($Raw) { return $resp }
    if ($resp.Content) { try { return $resp.Content | ConvertFrom-Json } catch { return $resp.Content } }
    return $resp
}

function Get-CsrfToken {
    # 先用一次 GET 让 WDP 下发 CSRF-Token(Update-Csrf 从响应头抠并存 $script:Csrf)
    if (-not $script:Csrf) {
        try { Invoke-Wdp -Method GET -Path '/api/os/info' -Raw | Out-Null } catch {}
    }
    return $script:Csrf
}

function Send-WdpUpload {
    # The only upload method this device's WDP accepts. A raw HttpClient multipart, with an explicit
    # ContentDisposition (Name AND FileName) and Content-Type: application/octet-stream per part --
    # modelled on the harness's deploy-launch.ps1, which has pushed a 48 MB package this way for
    # months. Invoke-WebRequest -Form produces a multipart this WDP rejects outright.
    param(
        [Parameter(Mandatory)][string]$MainFile,
        [string[]]$ExtraFiles = @()
    )

    Add-Type -AssemblyName System.Net.Http

    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.ServerCertificateCustomValidationCallback =
        [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
    $handler.CookieContainer = [System.Net.CookieContainer]::new()
    if ($Credential) {
        $handler.Credentials = $Credential.GetNetworkCredential()
        $handler.PreAuthenticate = $true
    }
    $cli = [System.Net.Http.HttpClient]::new($handler)
    $cli.Timeout = [TimeSpan]::FromSeconds($UploadTimeoutSec)

    # The token has to come from the same cookie jar the upload will use, so it is re-fetched here
    # rather than reused from the Invoke-Wdp session.
    $token = $null
    try {
        $warm = $cli.GetAsync("$BaseUri/api/os/info").GetAwaiter().GetResult()
        $sc = $null
        if ($warm.Headers.TryGetValues('Set-Cookie', [ref]$sc)) {
            foreach ($line in $sc) {
                if ($line -match 'CSRF-Token=([^;,\s]+)') {
                    $token = $Matches[1]
                    try { $handler.CookieContainer.Add([Uri]$BaseUri, [System.Net.Cookie]::new('CSRF-Token', $token, '/', $DeviceIp)) } catch {}
                }
            }
        }
    } catch {
        throw "cannot reach WDP at $BaseUri : $($_.Exception.Message)"
    }

    $mp = [System.Net.Http.MultipartFormDataContent]::new()
    foreach ($f in @($MainFile) + $ExtraFiles) {
        $full  = (Resolve-Path -LiteralPath $f).Path
        $name  = [IO.Path]::GetFileName($full)
        $bytes = [IO.File]::ReadAllBytes($full)
        $bc = [System.Net.Http.ByteArrayContent]::new($bytes)
        $bc.Headers.ContentDisposition = [System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
        $bc.Headers.ContentDisposition.Name     = "`"$name`""
        $bc.Headers.ContentDisposition.FileName = "`"$name`""
        $bc.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
        $mp.Add($bc)
        Write-Host ("    part: {0} ({1:N1} MB)" -f $name, ($bytes.Length / 1MB)) -ForegroundColor DarkGray
    }

    $mainName = [IO.Path]::GetFileName($MainFile)
    $path = "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($mainName))"
    $req = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, "$BaseUri$path")
    $req.Content = $mp
    if ($token) { $req.Headers.Add('X-CSRF-Token', $token) }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $resp = $cli.SendAsync($req).GetAwaiter().GetResult()
        $sw.Stop()
        $body = $resp.Content.ReadAsStringAsync().Result
    } finally {
        $cli.Dispose()
    }
    Write-Host ("    HTTP {0} in {1:N0}s" -f [int]$resp.StatusCode, $sw.Elapsed.TotalSeconds) -ForegroundColor DarkGray
    if (-not $resp.IsSuccessStatusCode) {
        throw "upload rejected: HTTP $([int]$resp.StatusCode) $body"
    }
    return $body
}

function Get-InstalledNames {
    # PackageFullName is the only unambiguous identifier in this list: `Name` is the manifest's
    # *display* name, which this app now carries a version inside ("Apotheosis 0.2.3.21"), and
    # `PackageFamilyName` does not move when the version does. Two snapshots of these full names are
    # how an install gets confirmed -- see Wait-InstallComplete.
    $pkgs = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    return @($pkgs.InstalledPackages | ForEach-Object { $_.PackageFullName })
}

function Wait-InstallComplete {
    param([string[]]$Before)
    # The verdict is the package list, NOT /api/app/packagemanager/state. Measured 2026-10-03: right
    # after the upload POST that endpoint answers 204/empty *before the install has registered*, so
    # reading "empty" as "finished" reported success for an install that never happened, and the run
    # went on to launch a package that was not there. The state channel is now used only for progress
    # and for a hard failure verdict; an empty answer there just means "not yet" and the loop keeps
    # polling. Success is a PackageFullName that was absent from the -Before snapshot.
    if (-not $PSBoundParameters.ContainsKey('Before')) {
        throw 'Wait-InstallComplete requires -Before: without a baseline every installed package looks new'
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    Write-Host "==> 等待安装完成..." -ForegroundColor Cyan
    $lastState = '(no state reported)'
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2

        $failMsg = $null
        try {
            $resp = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/state' -Raw
            if ($resp.StatusCode -ne 204 -and $resp.Content) {
                $state = $resp.Content | ConvertFrom-Json
                $pct = if ($null -ne $state.PercentComplete) { " $($state.PercentComplete)%" } else { '' }
                $lastState = "[$($state.CodeText)]$pct $($state.Reason)"
                Write-Host "    $lastState" -ForegroundColor DarkGray
                if ($state.Code -and $state.Code -ne 0 -and $state.Success -eq $false -and
                    $state.CodeText -notmatch 'InProgress|Installing|Ready') {
                    $failMsg = "安装失败: Code=$($state.Code) $($state.CodeText) - $($state.Reason)"
                }
            }
        } catch {
            Write-Host "    (状态查询暂不可用, 继续等包列表)" -ForegroundColor DarkGray
        }
        if ($failMsg) { throw $failMsg }

        try {
            $now = Get-InstalledNames
        } catch {
            Write-Host "    (包列表暂不可用: $($_.Exception.Message))" -ForegroundColor DarkGray
            continue
        }
        $new = @($now | Where-Object { $Before -notcontains $_ })
        if ($new.Count -gt 0) {
            Write-Host "==> 已确认安装: $($new -join ', ')" -ForegroundColor Green
            return $true
        }
        Write-Host "    (包列表里还没有新条目)" -ForegroundColor DarkGray
    }
    throw "安装超时 (${TimeoutSec}s): 包列表未出现新条目。最后一次状态: $lastState"
}

# ===========================================================================
# 入口
# ===========================================================================

Write-Host "WDP 目标: $BaseUri" -ForegroundColor Yellow

# --- 连通性 + 设备信息 ---
try {
    $info = Invoke-Wdp -Method GET -Path '/api/os/info'
    Write-Host "==> 已连接: $($info.ComputerName)  OS $($info.OsVersion)  $($info.Platform)" -ForegroundColor Green
} catch {
    Write-Error "无法连接 WDP ($BaseUri): $($_.Exception.Message)`n提示: 确认设备已开 Device Portal,IP/端口/scheme 正确(Win10M 常用 http:80 或 https:443),如设了密码用 -Credential。"
    exit 1
}

# --- List 模式 ---
if ($List) {
    $pkgs = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    Write-Host "`n已安装应用:" -ForegroundColor Cyan
    $pkgs.InstalledPackages | Sort-Object Name | ForEach-Object {
        "{0,-45} {1}" -f $_.Name, $_.PackageFullName
    }
    exit 0
}

# --- Uninstall 模式 ---
if ($Uninstall) {
    Write-Host "==> 卸载: $Uninstall" -ForegroundColor Cyan
    Invoke-Wdp -Method DELETE -Path "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($Uninstall))" | Out-Null
    Write-Host "==> 卸载请求已发送。" -ForegroundColor Green
    exit 0
}

# --- Deploy 模式 ---
$appxItem = Get-Item -LiteralPath $Appx
$pkgName  = $appxItem.Name

# Dependencies first, then the certificate: WDP wants them in the same multipart as the main package.
$extra = @()
foreach ($dep in $Dependencies) {
    $d = Get-Item -LiteralPath $dep
    $extra += $d.FullName
    Write-Host "    + 依赖: $($d.Name)" -ForegroundColor DarkGray
}
if ($Certificate) {
    $c = Get-Item -LiteralPath $Certificate
    $extra += $c.FullName
    Write-Host "    + 证书: $($c.Name)" -ForegroundColor DarkGray
}

Write-Host "==> 上传并安装: $pkgName ($([math]::Round($appxItem.Length/1MB,1)) MB)" -ForegroundColor Cyan
# The baseline has to be read *before* the POST: the install is confirmed by this list gaining an
# entry, so a snapshot taken afterwards could already contain the thing it is meant to detect.
$installedBefore = Get-InstalledNames
Write-Host "    (基线: $($installedBefore.Count) 个已安装包)" -ForegroundColor DarkGray
Send-WdpUpload -MainFile $appxItem.FullName -ExtraFiles $extra | Out-Null

Wait-InstallComplete -Before $installedBefore | Out-Null
Write-Host "==> 完成: $pkgName 已部署到 $DeviceIp" -ForegroundColor Green

#Requires -Version 7.0
<#  deploy-launch.ps1 — 卸载旧→装新→启动,不轮询(交互测用)。 #>
[CmdletBinding()]
param(
    [string]$Ip='192.168.3.51',
    [string]$Ver='0.1.7.23',
    [string]$Pub='x0rqga78m2mgc',
    # Apotheosis: launching from the desk is opt-in. See the comment at the launch step for why the
    # default changed -- a run that never gets foreground focus is not a test, and its log says
    # "clean-exit" anyway.
    [switch]$Launch,
    # Apotheosis: -Clean uninstalls first, which also wipes LocalState. Off by default because the
    # destructive order turns a dropped upload into a device with no browser, and because wiping
    # LocalState has already destroyed evidence someone had just reproduced by hand.
    [switch]$Clean
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"
$FN="EdgeHTMLReborn.Harness_${Ver}_arm__${Pub}"
$Praid="EdgeHTMLReborn.Harness_${Pub}!App"
$appxDir="$env:APOTHEOSIS_HARNESS\AppPackages\Harness\Harness_${Ver}_ARM_Test"
$appx="$appxDir\Harness_${Ver}_ARM.appx"; $cer="$appxDir\Harness_${Ver}_ARM.cer"
if(-not (Test-Path $appx)){ throw "appx 不存在: $appx" }
$h=[System.Net.Http.HttpClientHandler]::new()
$h.ServerCertificateCustomValidationCallback=[System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer=[System.Net.CookieContainer]::new()
# Apotheosis: 30 minutes, not 8. The appx is about 48 MB and the phone's WiFi is both slow and
# variable -- 2026-08-19 a deploy of 0.1.9.19 died on the old 8-minute limit mid-upload, and because
# the script uninstalls before it uploads, that left the device with no application at all. The
# timeout is a safety net for a dead connection, not a schedule: raising it costs nothing when the
# link is healthy.
$cli=[System.Net.Http.HttpClient]::new($h); $cli.Timeout=[TimeSpan]::FromMinutes(30)
$script:Csrf=$null
function Sync-Csrf($resp){ $sc=$null; if($resp.Headers.TryGetValues('Set-Cookie',[ref]$sc)){ foreach($l in $sc){ if($l -match 'CSRF-Token=([^;,\s]+)'){ $script:Csrf=$Matches[1]; try{$h.CookieContainer.Add([Uri]$base,[System.Net.Cookie]::new('CSRF-Token',$script:Csrf,'/',$Ip))}catch{} } } } }
function Wdp([string]$m,[string]$p,[System.Net.Http.HttpContent]$c){ $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($m),"$base$p"); if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}; if($c){$req.Content=$c}; $r=$cli.SendAsync($req).GetAwaiter().GetResult(); Sync-Csrf $r; return $r }
function Txt($r){ return $r.Content.ReadAsStringAsync().Result }

Sync-Csrf ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
Write-Host "设备: $(Txt (Wdp GET '/api/os/info' $null))" -ForegroundColor Green

# Apotheosis: uninstalling first is now opt-in, because it is the destructive order and the failure it
# produces is the expensive one. A 45 MB upload over this phone's WiFi takes about twenty minutes and does
# drop -- on 2026-08-21 it died with "Error while copying content to a stream" *after* the old package had
# already been removed, leaving the device with no browser at all while nobody was next to it.
# Not uninstalling costs nothing: package identity is Name plus publisher, not version, so installing a
# higher version is an in-place update and the app list still shows exactly one Apotheosis. Verified the
# same day -- Deploy-Robust.ps1 deletes nothing and the device ended up with one package,
# EdgeHTMLReborn.Harness_0.1.9.29_arm__edmb40rfkwsbg.
# What uninstalling does buy is a clean LocalState, since logs and settings go with the package. That is
# occasionally what you want and more often actively harmful: it has already destroyed a reproduction the
# maintainer had just performed by hand. So pass -Clean when you specifically want the slate wiped.
if ($Clean) {
  foreach($p in ((Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -like '*EdgeHTMLReborn.Harness*' })){
    $u=Wdp DELETE "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($p.PackageFullName))" $null
    Write-Host "卸载 $($p.PackageFullName): $([int]$u.StatusCode)"
    Start-Sleep 2
  }
} else {
  Write-Host "keeping the installed package: a higher version updates it in place, and a dropped upload then leaves the previous build working. Pass -Clean to wipe LocalState instead." -ForegroundColor DarkGray
}

# 装新
$mp=[System.Net.Http.MultipartFormDataContent]::new()
foreach($f in @($appx,$cer)){ $b=[System.IO.File]::ReadAllBytes($f); $n=[System.IO.Path]::GetFileName($f); $bc=[System.Net.Http.ByteArrayContent]::new($b); $bc.Headers.ContentDisposition=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data'); $bc.Headers.ContentDisposition.Name="`"$n`""; $bc.Headers.ContentDisposition.FileName="`"$n`""; $bc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream'); $mp.Add($bc) }
$ins=Wdp POST "/api/app/packagemanager/package?package=$([uri]::EscapeDataString([System.IO.Path]::GetFileName($appx)))" $mp
Write-Host "安装请求: $([int]$ins.StatusCode)"
$ok=$false
for($i=0;$i -lt 40;$i++){ Start-Sleep 3; if((Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -eq $FN }){ $ok=$true; break }; Write-Host "  …装中($i)" }
if(-not $ok){ Write-Host "❌ 安装未确认,终止" -ForegroundColor Red; exit 1 }
Write-Host "✅ 已安装 $FN" -ForegroundColor Green

# Apotheosis: arm crash-dump collection right here, because it is a PER-PACKAGE setting and every
# reinstall silently resets it. That cost two reproductions on 2026-08-19: dumps were armed for
# 0.1.9.19, the next build was deployed as 0.1.9.20, and the crash the maintainer then reproduced left
# a CRASHVERDICT: CRASHED line in the log but no dump at all -- so the stack that would have named the
# faulting function was simply not collected. Arming it as a separate step you have to remember is a
# trap; arming it as part of deployment is not. One POST, and it applies from the very first launch,
# which matters because the interesting crash happens seconds after start.
$cc=Wdp POST "/api/debug/dump/usermode/crashcontrol?packageFullName=$([uri]::EscapeDataString($FN))" $null
Write-Host "✅ 崩溃 dump 收集已开启: $([int]$cc.StatusCode) (per-package, resets on every reinstall)" -ForegroundColor Green

# 启动
# Apotheosis: launching from here is now opt-in, because doing it by default manufactures misleading
# evidence. An app started over Device Portal while the phone is locked never gets foreground focus, so
# the OS suspends it within seconds and then reaps it -- and the harness, correctly, records that as
# `CRASHVERDICT: clean-exit`, because a suspend-then-terminate *is* a clean shutdown. The result is a log
# that looks like a successful run of a browser that never drew a pixel. That reading cost real time
# twice on 2026-08-20, once sending the investigation toward a memory-quota theory that measurement then
# refuted. Worse, this script uninstalls first, so an unattended deploy would silently overwrite the
# LocalState evidence of whatever the maintainer had just reproduced by hand.
# Pass -Launch when you are holding the unlocked phone and want it started for you.
if ($Launch) {
    $aid=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Praid)); $pkg=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FN))
    $lr=Wdp POST "/api/taskmanager/app?appid=$([uri]::EscapeDataString($aid))&package=$([uri]::EscapeDataString($pkg))" $null
    Write-Host "✅ 启动: $([int]$lr.StatusCode) — 设备上应已起来,可以开始测了" -ForegroundColor Green
    Write-Host "   note: if the screen was locked, this run will be suspended and reaped -- not a valid test" -ForegroundColor DarkYellow
} else {
    Write-Host ""
    Write-Host "installed but NOT launched, on purpose." -ForegroundColor Cyan
    Write-Host "  Unlock the phone and start Apotheosis from the tile or the app list, so the run gets" -ForegroundColor Cyan
    Write-Host "  foreground focus and the log describes a real test. Re-run with -Launch to start it" -ForegroundColor Cyan
    Write-Host "  from here anyway." -ForegroundColor Cyan
}

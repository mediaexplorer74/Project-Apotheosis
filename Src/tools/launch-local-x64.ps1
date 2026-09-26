# Apotheosis: launch the installed x64 harness without touching LocalState.
$pkg = Get-AppxPackage EdgeHTMLReborn.Harness | Select-Object -First 1
if (-not $pkg) { throw 'EdgeHTMLReborn.Harness is not installed' }
$aid = (Get-AppxPackageManifest $pkg.PackageFullName).Package.Applications.Application.Id
$family = $pkg.PackageFamilyName
$target = "shell:AppsFolder\{0}!{1}" -f $family, $aid
Write-Host "launch: $target"
Start-Process $target

# setenv.ps1 — set APOTHEOSIS_ROOT and sub-variables for the Apotheosis project
# Source this script: . .\Src\setenv.ps1
# All build scripts use $env:APOTHEOSIS_* instead of hardcoded E:\Apotheosis\

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir "..")
$env:APOTHEOSIS_ROOT = $Root

# Sub-variables for the subdirectories
$env:APOTHEOSIS_PORT    = Join-Path $Root "Src\port"
$env:APOTHEOSIS_HARNESS = Join-Path $Root "Src\harness"
$env:APOTHEOSIS_TOOLS   = Join-Path $Root "Src\tools"
$env:APOTHEOSIS_ANGLE   = Join-Path $Root "Src\angle"
$env:APOTHEOSIS_CRASH   = Join-Path $Root "crash"

# Default platform (arm / x64)
if (-not $env:APOTHEOSIS_ARCH) { $env:APOTHEOSIS_ARCH = "x64" }

# VS + SDK paths
$env:APOTHEOSIS_VS = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$env:APOTHEOSIS_MSVC = "$env:APOTHEOSIS_VS\VC\Tools\MSVC\14.44.35207"
$env:APOTHEOSIS_SDK = 'C:\Program Files (x86)\Windows Kits\10'
$env:APOTHEOSIS_SDK_VER = '10.0.26100.0'

# Vcpkg + ICU (external dependencies, arch-specific)
$env:APOTHEOSIS_VCPKG = 'C:\vcpkg'
$archSuffix = if ($env:APOTHEOSIS_ARCH -eq 'arm') { 'arm-uwp' } else { 'x64-uwp' }
$env:APOTHEOSIS_VCPKG_TRIPLET = $archSuffix
# Apotheosis 2026-08-21: ICU lives in the vcpkg prefix on both architectures now. This used to be
# "C:\icu-$archSuffix", which named the hand-built x64 tree and, for arm, a directory that has never
# existed on this machine. Nothing reads this variable today -- it is exported for orientation -- but
# a wrong value in the one place a newcomer looks first is worse than none.
$env:APOTHEOSIS_ICU   = "C:\vcpkg\installed\$archSuffix"
$env:APOTHEOSIS_ANGLE_ARCH = if ($env:APOTHEOSIS_ARCH -eq 'arm') { 'arm' } else { 'x64' }

# Perl (vcpkg downloads) — needed by Python codegen scripts that run `perl` via subprocess
$perlBin = "C:\vcpkg\downloads\tools\perl\5.42.2.1\perl\bin"
if (Test-Path "$perlBin\perl.exe") {
    $env:PATH = "$perlBin;$env:PATH"
}

# Apotheosis: Ruby, needed by WebKit's own code generators.
#
# Added 2026-08-21 after a from-scratch CMake configure failed with
#     CMake Error at Source/cmake/WebKitCommon.cmake:221 (message):
#       Ruby 2.5 or higher is required.
# Ruby 3.4.10 was installed all along at C:\tools\ruby34; it simply was not on PATH, and every
# configure since had reused the cached Ruby_EXECUTABLE from an earlier run made in a shell that
# happened to have it. So "reconfigure works" was true only for an incremental reconfigure, and
# deleting CMakeCache.txt -- the ordinary way to make CMake re-search for a moved dependency --
# broke the build directory. Exported here so both are true.
$rubyBin = "C:\tools\ruby34\bin"
if (Test-Path "$rubyBin\ruby.exe") {
    $env:PATH = "$rubyBin;$env:PATH"
}

write-host "==> APOTHEOSIS_ROOT  = $Root" -ForegroundColor Cyan
write-host "==> APOTHEOSIS_PORT  = $env:APOTHEOSIS_PORT" -ForegroundColor Cyan
write-host "==> APOTHEOSIS_TOOLS = $env:APOTHEOSIS_TOOLS" -ForegroundColor Cyan
write-host "==> APOTHEOSIS_ARCH  = $env:APOTHEOSIS_ARCH" -ForegroundColor Cyan
write-host "==> ICU path         = $env:APOTHEOSIS_ICU" -ForegroundColor Cyan
write-host "==> VCPKG triplet    = $env:APOTHEOSIS_VCPKG_TRIPLET" -ForegroundColor Cyan

# Validate the critical paths
$checks = @(
    "$env:APOTHEOSIS_PORT\WebCoreDriver.cpp",
    "$env:APOTHEOSIS_HARNESS\MainPage.xaml.cpp",
    "$env:APOTHEOSIS_MSVC\bin\Hostx64\x64\cl.exe"
)
foreach ($c in $checks) {
    if (-not (Test-Path $c)) { write-host "WARNING: not found: $c" -ForegroundColor Yellow }
}

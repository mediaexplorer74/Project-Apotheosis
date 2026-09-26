# compile-driver-gpu-x64.ps1 — compile port .cpp using Ninja's own include paths.
# Extracts DEFINES + FLAGS + INCLUDES from build-x64-gpu\build.ninja and
# substitutes the source file. Run after Src\setenv.ps1.
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'
# Apotheosis: this script inherits INCLUDE / LIB / PATH from the session, so refuse to run in a
# shell that arm32-uwp-env.ps1 has already rewritten. See assert-target.ps1.
& "$PSScriptRoot\assert-target.ps1" -Expect x64
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$BuildDir = Join-Path $Root 'build-x64-gpu'

# Read the first WebCore build entry from build.ninja to use as template
$ninja = Join-Path $BuildDir 'build.ninja'
$template = Select-String -Path $ninja -Pattern "^build Source\\WebCore\\CMakeFiles\\WebCore\.dir\\platform\\graphics\\cairo\\CairoUtilities\.cpp\.obj:" -Context 0,20
if (-not $template) { throw "Cannot find template build rule in $ninja" }

# Extract DEFINES, FLAGS, INCLUDES from the template
$defines = ""
$flags = ""
$includes = ""
foreach ($line in $template[0].Context.PostContext) {
    if ($line -match '^\s+DEFINES = (.+)$') { $defines = $matches[1] }
    if ($line -match '^\s+FLAGS = (.+)$') { $flags = $matches[1] }
    if ($line -match '^\s+INCLUDES = (.+)$') { $includes = $matches[1] }
}

# Apotheosis: the DEFINES above are lifted from a WebCore-internal build rule, so they
# carry -DBUILDING_WebCore / -DWebCore_EXPORTS. Those make WEBCORE_EXPORT expand to
# __declspec(dllexport) in port code, so the port emits *direct* references to WebCore
# globals (e.g. HTMLNames::aTag) instead of __imp_ indirections. The linker then binds
# them to the static archive's own never-initialized copies, and reading one faults
# (0xC0000005 in HTMLCollection traversal via is<HTMLAnchorElement>). The port is a
# *consumer* of WebCore.dll, so strip them and let WEBCORE_EXPORT mean dllimport.
$defines = ($defines -replace '(^|\s)[-/]DBUILDING_WebCore(?=\s|$)', ' ') -replace '(^|\s)[-/]DWebCore_EXPORTS(?=\s|$)', ' '

# No PCH — PCH has define mismatch with GL_GLEXT_PROTOTYPES
$flags = $flags -replace '/Yu[^ ]*', '' -replace '/FI[^ ]*', '' -replace '/Fp[^ ]*', ''
# /GS- to suppress stack cookie checks in port layer (avoid 0xc0000409)
$flags = $flags -replace '/GS ', '/GS- '
$flags = $flags -replace '/MD ', '/MT '
# Apotheosis: do NOT add -DUSE_FREETYPE=1 here. WebCore itself is built without it
# (see build-x64-gpu\cmakeconfig.h), so defining it only for port objects changes the
# FontPlatformData layout (adds RefPtr<FcPattern> m_pattern) and pulls in fontconfig
# headers that this port deliberately does not ship. FreeType is still usable directly
# via <ft2build.h> + cairo-ft.h, which is how the font stubs get an FT_Face.

# Add port-specific include paths
$extra = "-I$Root\WebKit\Source\WebKitLegacy -I$Root\WebKit\Source\WebKitLegacy\WebCoreSupport -I$Root\Src\angle\include -IC:\Strawberry\c\include -I$Root\WebKit\Source\WebCore\platform\graphics\freetype"
$includes = "$includes $extra"

$clang = "C:\Program Files\LLVM\bin\clang-cl.exe"
$target = "--target=x86_64-unknown-windows-msvc"
$SrcAbs = (Resolve-Path $Src).Path
$ObjAbs = (Resolve-Path (Split-Path -Parent $Obj)).Path + '\' + (Split-Path -Leaf $Obj)
$cmd = "`"$clang`" $target $defines $includes $flags /Fo`"$ObjAbs`" -c -- `"$SrcAbs`""

$ObjDir = Resolve-Path (Split-Path -Parent $Obj)
$log = Join-Path $ObjDir 'compile-x64-gpu.log'
# Use response file to avoid cmd.exe 8191-char limit
$responseFile = Join-Path $ObjDir '_driver_x64_compile.rsp'
# Strip the leading clang-cl path from the command for the response file
$cmdForRsp = $cmd
$clangPrefix = "`"$clang`""
if ($cmdForRsp.StartsWith($clangPrefix)) { $cmdForRsp = $cmdForRsp.Substring($clangPrefix.Length).TrimStart() }
Set-Content -Path $responseFile -Value $cmdForRsp -Encoding ASCII
Set-Location $BuildDir
    $prevEAP = $ErrorActionPreference; $ErrorActionPreference = 'Continue'; & $clang "@$responseFile" 1> $log 2>&1; $ErrorActionPreference = $prevEAP
$exit = $LASTEXITCODE
Set-Location $Root
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver-gpu-x64] EXIT=$exit  errors=$err  obj=$Obj  log=$log"
if ($exit -ne 0) { Get-Content $log -Tail 30 }
# Apotheosis: propagate clang failure to callers; an old object is not a successful build.
exit $exit

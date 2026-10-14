# ============================================================================
# build-driver-dll-arm32.ps1 — Build WebCoreDriver.dll for ARM32 or x64
#
# Usage:
#   pwsh -File build-driver-dll-arm32.ps1 -Arch arm       # ARM32 DLL
#   pwsh -File build-driver-dll-arm32.ps1 -Arch x64       # x64 DLL
#   pwsh -File build-driver-dll-arm32.ps1 -Arch arm -Clean # clean build
#
# Produces:
#   Src/port/WebCoreDriver-arm.dll  (ARM32, for Lumia 950)
#   Src/port/WebCoreDriver-x64.dll  (x64, for PC dev)
#
# Requires: LLVM/clang-cl, lld-link, pre-built WebKit + deps
# ============================================================================
param(
    [ValidateSet('arm','x64')]
    [string]$Arch = 'arm',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
# Apotheosis 2026-09-26: this was `Split-Path -Parent $PSScriptRoot`, i.e. <repo>\Src, and
# every $Root-based path below it therefore resolved one level too deep (Src\WebKit\...,
# Src\Src\angle\...). Nothing that uses this script could compile; the x64 driver DLL that
# the C# app ships was produced by hand instead.
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
if (-not $env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT = $Root }
$PortDir = $PSScriptRoot

# ---------------------------------------------------------------------------
# 1. Environment setup
# ---------------------------------------------------------------------------
if ($Arch -eq 'arm') {
    # Apotheosis 2026-09-26: was 'build-clang-gpu', a tree this repo does not have. The ARM
    # engine build tree is build-arm32-gpu (same name link-driver-gpu-arm32.ps1 uses), and its
    # WebCore/JavaScriptCore/WTF/PAL .lib files are what the link step below consumes -- so
    # this line is what keeps a driver DLL rebuild off the half-day WebKit rebuild.
    $BuildDir = Join-Path $Root 'build-arm32-gpu'
    $EnvScript = Join-Path $PortDir 'arm32-uwp-env.ps1'
    $Target = 'thumbv7-unknown-windows-msvc'
    $Machine = 'ARM'
    $DllName = 'WebCoreDriver-arm.dll'
    $LibName = 'WebCoreDriver-arm.lib'
    # Apotheosis 2026-09-26: was '.arm32-soft.obj'. The per-arch compile scripts write
    # <name>.arm32.obj, and the object suffix is what the [cached] check compares against --
    # a suffix nothing ever produces means "never cached", i.e. all 13 TUs recompiled.
    $ObjSuffix = '.arm32.obj'
    $ANGLELibDir = Join-Path $Root 'Src\angle\arm'
    $VcpkgLibDir = 'C:\vcpkg\installed\arm-uwp\lib'
    # Apotheosis 2026-08-21: was 'C:\icu-arm-uwp\lib', a directory that does not exist on this machine
    # and never did -- the ARM line has always taken ICU from vcpkg. Harmless only because lld-link
    # ignores a missing /libpath, which is exactly why it survived unnoticed. Both branches now point
    # at the vcpkg prefix, which for x64 became true on the same day (see
    # Doc/HARFBUZZ-ICU-DIVERGENCE.md); the variable is kept rather than folded into $VcpkgLibDir so
    # that a hand-built ICU can still be aimed at for an experiment.
    $IcuLibDir = 'C:\vcpkg\installed\arm-uwp\lib'
} else {
    $BuildDir = Join-Path $Root 'build-x64-gpu'
    # Apotheosis 2026-09-26: was arm32-uwp-env.ps1 "x64 env setup if needed". That set
    # APOTHEOSIS_TARGET=arm32 and put the arm32 toolchain on INCLUDE/LIB/PATH, which made the
    # x64 compile step abort in assert-target.ps1 and link.exe miss WindowsApp.lib -- the script
    # poisoned its own environment. The correct env per architecture is exactly what the compile
    # scripts source, so use it here too.
    $EnvScript = Join-Path $PortDir 'x64-uwp-env.ps1'
    $Target = 'x86_64-unknown-windows-msvc'
    $Machine = 'X64'
    $DllName = 'WebCoreDriver-x64.dll'
    $LibName = 'WebCoreDriver-x64.lib'
    $ObjSuffix = '.x64.obj'
    $ANGLELibDir = Join-Path $Root 'Src\angle\x64'
    $VcpkgLibDir = 'C:\vcpkg\installed\x64-uwp\lib'
    $IcuLibDir = 'C:\vcpkg\installed\x64-uwp\lib'
}

# Source environment
if ($EnvScript -and (Test-Path $EnvScript)) {
    . $EnvScript *> $null
}

# ---------------------------------------------------------------------------
# 2. Port layer sources (12 files → all get compiled)
# ---------------------------------------------------------------------------
$Sources = @(
    'WebCoreDriver',
    'PortChromeClient',
    'LoadingFrameLoaderClient',
    'PortPlatformStrategies',
    'PortNetworkStorageSession',
    'PortStorage',
    'PortWebSocket',
    'PortBackForward',
    'webcore-driver-stubs',
    'stubs-crypto',
    'stubs-pasteboard',
    'stubs-network',
    'stubs-ax',
    'stubs-other',
    'stubs-loader',
    # Apotheosis 2026-10-09: stubs-font-uwp.cpp is a WebCore translation unit -- CMake compiles it
    # into WebCoreFull-arm32.lib (build-arm32-gpu\Source\WebCore\CMakeFiles\WebCore.dir\...), and this
    # list is the ONLY set of sources the ARM relink ever rebuilds. Until today the phone therefore
    # linked a stubs-font-uwp.cpp.obj built 2026-08-22 09:50, whose
    # FontCache::systemFallbackForCharacterCluster carries neither the `size > 0` guard nor the
    # `scaledFont()` check: a zero-size FontDescription handed FontDataCacheKeyTraits::emptyValue()
    # (FontPlatformData(0,false,false)) to fontForPlatformData, whose HashMap::ensure release-asserts
    # -- RELEASE_ASSERT at wtf/HashTable.h:534, the `.30` device death, measured by disassembling the
    # shipped DLL against that stale object. Listing the source here compiles it with the same flags
    # as the other stubs; objects precede $WebKitLibs on the link line, so these definitions win over
    # the archive member, and /FORCE:MULTIPLE (ARM only, below) tolerates any overlap. Verify a rebuild
    # took without touching the device: the new DLL must contain "FONT fallback" (the old one does not).
    'stubs-font-uwp'
)

# Apotheosis 2026-09-26 (ARM only): Cairo's win32 backend references desktop GDI through __imp_,
# and UWP/AppContainer has no GDI. The x64 line receives these from gdi-stubs.cpp through the harness
# vcxproj, so the standalone driver link has to compile it in explicitly. The relative entry keeps
# the "$s.cpp" / "$s$ObjSuffix" naming convention in the compile loop below intact.
if ($Arch -eq 'arm') { $Sources += '..\harness\gdi-stubs' }

# ---------------------------------------------------------------------------
# 3. Clean if requested
# ---------------------------------------------------------------------------
if ($Clean) {
    foreach ($s in $Sources) {
        $obj = Join-Path $PortDir "$s$ObjSuffix"
        if (Test-Path $obj) { Remove-Item $obj -Force }
    }
    $dll = Join-Path $PortDir $DllName
    if (Test-Path $dll) { Remove-Item $dll -Force }
    Write-Host "==> Cleaned $Arch build artifacts" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 4. Compile each port source, per architecture
# ---------------------------------------------------------------------------
# Apotheosis 2026-09-26: this block used to parse -I paths and -D defines out of
# harness-cmd.bat and invoke clang-cl itself. harness-cmd.bat is an ARM-only dump of the
# *previous* tree layout (build-arm32-webcore, E:/Apotheosis), so the x64 run inherited ARM
# include paths and every source died on "no such file or directory" -- i.e. neither
# architecture could be built by this script. Both now go through the compilers that are kept
# in step with the real trees: compile-driver-gpu-x64.ps1 reads build-x64-gpu\build.ninja,
# compile-driver-gpu-arm32.ps1 reads build-arm32-gpu\build.ninja (and rewrites harness-cmd.bat's
# stale paths itself). Each of those also sources the right toolchain env, so the env script
# below is no longer needed for the compile step.
$CompileScript = if ($Arch -eq 'x64') { Join-Path $PortDir 'compile-driver-gpu-x64.ps1' }
                 else { Join-Path $PortDir 'compile-driver-gpu-arm32.ps1' }
$CompileLog = Join-Path $PortDir "compile-$Arch-dll.log"
$LogContent = @()

# ---------------------------------------------------------------------------
# 5. Compile loop
# ---------------------------------------------------------------------------
$Compiled = @()

Write-Host "==> Compiling $($Sources.Count) port sources for $Arch" -ForegroundColor Cyan

foreach ($s in $Sources) {
    $srcFile = Join-Path $PortDir "$s.cpp"
    $objFile = Join-Path $PortDir "$s$ObjSuffix"

    if (-not (Test-Path $srcFile)) {
        Write-Host "  SKIP (no source): $s.cpp" -ForegroundColor Yellow
        continue
    }

    # Skip recompile if obj is newer than source
    if ((Test-Path $objFile) -and
        (Get-Item $objFile).LastWriteTime -gt (Get-Item $srcFile).LastWriteTime) {
        Write-Host "  [cached] $s.cpp" -ForegroundColor DarkGray
        $Compiled += $objFile
        continue
    }

    Write-Host "  compile $s.cpp ..." -NoNewline
    $started = [DateTime]::UtcNow
    & pwsh -NoProfile -File $CompileScript $srcFile $objFile
    $exit = $LASTEXITCODE

    # Apotheosis: fail closed. A zero exit code with a missing or stale object would otherwise
    # be linked into the DLL and surface as an unresolved symbol much later.
    if ($exit -ne 0 -or -not (Test-Path $objFile) -or
        (Get-Item $objFile).LastWriteTimeUtc -lt $started) {
        Write-Host " FAILED (exit $exit)" -ForegroundColor Red
        $LogContent += "[$s] compile FAILED (exit $exit)"
        $perArchLog = Join-Path $PortDir "compile-$Arch-gpu.log"
        if (Test-Path $perArchLog) {
            Get-Content $perArchLog -Tail 20 | ForEach-Object { $LogContent += "[$s] $_" }
        }
        continue
    }

    $Compiled += $objFile
    Write-Host " OK" -ForegroundColor Green
}

# Save compile log
$LogContent | Out-File $CompileLog -Encoding UTF8

if ($Compiled.Count -eq 0) {
    Write-Host "!! No objects compiled. Aborting." -ForegroundColor Red
    exit 1
}

Write-Host "==> Compiled $($Compiled.Count)/$($Sources.Count) objects" -ForegroundColor Green

# ---------------------------------------------------------------------------
# 6. Link into DLL
# ---------------------------------------------------------------------------
$LldLink = 'C:\Program Files\LLVM\bin\lld-link.exe'
$DefFile = Join-Path $PortDir 'WebCoreDriver.exports.def'
$DllOut = Join-Path $PortDir $DllName
$LibOut = Join-Path $PortDir $LibName

# WebKit static libs
# Apotheosis 2026-09-26: the old list mixed the two architectures' models and pointed x64 at the
# wrong archive. build-x64-gpu\lib\WebCore.lib is the *import* library of build-x64-gpu\bin\
# WebCore.dll (33718 __imp_ symbols, every archive member literally named "WebCore.dll"), and that
# DLL is the current ICU-78 build. The only full static archive around, WebCoreFull.lib, is from
# 16.08 and wants u_toupper_75 -- ICU 75 no longer exists on this machine, so linking against it
# cannot work. The payload's WebCoreDriver.dll (26.09) was static and therefore came from a
# 78-era full archive that is no longer in the tree.
#
# Apotheosis 2026-09-26: BOTH architectures link WebCore statically now, through a WebCoreFull
# archive, and WebCore.dll is therefore dead weight in the payload. The note that used to sit here
# argued for import mode on both: "as __imp_ references into WebCore.dll they resolve at load time".
# That is true only for EXPORTED names, and the symbols stubs-other.cpp actually needs are not
# exported -- RenderTheme::RenderTheme() and SystemFontDatabase::SystemFontDatabase() are protected
# constructors, and WebKitFontFamilyNames::standardFamily is a data global. There is no __imp_ thunk
# for any of them, so nothing can resolve them at load time; the link simply fails.
$WebKitLibs = @(
    # does. stubs-other*.obj references WebCore internals that the DLL does not export -- the
    # protected ctors RenderTheme::RenderTheme() and SystemFontDatabase::SystemFontDatabase(), the
    # data global WebKitFontFamilyNames::standardFamily, and the RenderTheme virtuals. An __imp_
    # thunk exists only for an EXPORTED name, so linking WebCore.lib (import) cannot satisfy them
    # and every one shows up as LNK2001/LNK2019.
    # The earlier note here said the full archive "cannot work because it wants u_toupper_75".
    # That is no longer true: WebCoreFull-x64-19041.lib was rebuilt on 2026-09-26 from the 25.09
    # objects, which are ICU 78, and the shipped driver DLL imports icuuc78.dll / icuin78.dll. ICU
    # import libs for 78 come from vcpkg x64-uwp; C:\icu-x64-uwp holds ICU 75 and must NOT be used.
    (Join-Path $BuildDir 'lib\WebCoreFull-x64-19041.lib')
    # On x64 JavaScriptCore really is a DLL behind its import lib (the shipped driver imports
    # JavaScriptCore.dll), so it is listed once and needs no PAL/WTF companions.
    (Join-Path $BuildDir 'lib\JavaScriptCore.lib')
    # PAL is a separate archive on x64 too -- WebCore calls PAL::TextEncoding, PAL::UTF8Encoding
    # and PAL::SessionID, which WebCoreFull alone does not provide (144 unresolved without it).
    (Join-Path $BuildDir 'lib\PAL-x64-19041.lib')
)
if ($Arch -eq 'arm') {
    # Apotheosis 2026-09-26: ARM was switched from WebCore.lib (import) to the static archive,
    # because stubs-other.arm32.obj references WebCore INTERNALS that WebCore.dll does not export:
    # the protected ctors RenderTheme::RenderTheme() and SystemFontDatabase::SystemFontDatabase(),
    # the data global WebKitFontFamilyNames::standardFamily, and the RenderTheme virtuals
    # (purgeCaches, baselinePosition, supportsFocusRing, isControlStyled). An __imp_ thunk only
    # exists for an EXPORTED symbol, so "it will resolve at load time" was wrong for all of these --
    # that reasoning only holds for exported names. x64 already links WebCore statically for the
    # same reason, and its shipped WebCoreDriver.dll does not import WebCore.dll.
    # WebCoreFull-arm32.lib is built from the 963 objects in build-arm32-gpu\Source\WebCore with
    # MSVC bin\Hostx64\arm\lib.exe (an x64 lib.exe records the wrong machine).
    $WebKitLibs = @(
        (Join-Path $BuildDir 'lib\WebCoreFull-arm32.lib')
        (Join-Path $BuildDir 'lib\JavaScriptCore.lib')
        # JSC twice, and WTF twice: the circular JSC<->WTF dependency. JavaScriptCore.lib on ARM is
        # a real object archive (16k defined symbols) that already carries the WTF objects, hence
        # /FORCE:MULTIPLE above.
        (Join-Path $BuildDir 'lib\JavaScriptCore.lib')
        (Join-Path $BuildDir 'lib\PAL.lib')
        (Join-Path $BuildDir 'lib\WTF.lib')
        (Join-Path $BuildDir 'lib\WTF.lib')
    )
}

# Third-party libs -- per architecture.
# Apotheosis 2026-09-26: this was ONE list, written for lib.exe, which does not resolve externals:
# a name that does not exist is simply never pulled in, so cairo.lib / fontconfig.lib / expat.lib /
# sqlite3.lib / zlib.lib / libjpeg-turbo.lib sat in the x64 command for months without anyone
# noticing (link-driver-gpu-x64.ps1 is the origin of those names). lld-link *does* resolve, and
# failed on six of them. The x64 set below is what the shipped payload DLL actually imports
# (checked with llvm-readobj --coff-imports on native\x64\WebCoreDriver.dll): WebCore is static in
# the DLL, JavaScriptCore is a DLL behind its import lib, cairo is the hand-built "complete" one,
# and ICU comes from C:\icu-x64-uwp rather than from vcpkg. The ARM set keeps the vcpkg names the
# ARM line was written against.
$ThirdPartyLibs = if ($Arch -eq 'x64') {
    @(
        "$env:ProgramFiles\LLVM\lib\clang\22\lib\windows\clang_rt.builtins-x86_64.lib"
        (Join-Path $BuildDir 'libs\cairo-complete-x64-uwp.lib')
        (Join-Path $BuildDir 'libs\sqlite3.lib')
        (Join-Path $BuildDir 'libs\psl\psl.lib')
        (Join-Path $ANGLELibDir 'libEGL.lib')
        (Join-Path $ANGLELibDir 'libGLESv2.lib')
        (Join-Path $IcuLibDir 'icuuc.lib')
        (Join-Path $IcuLibDir 'icuin.lib')
        (Join-Path $IcuLibDir 'icudt.lib')
        (Join-Path $VcpkgLibDir 'freetype.lib')
        (Join-Path $VcpkgLibDir 'harfbuzz.lib')
        (Join-Path $VcpkgLibDir 'harfbuzz-icu.lib')
        (Join-Path $VcpkgLibDir 'libcurl.lib')
        (Join-Path $VcpkgLibDir 'libssl.lib')
        (Join-Path $VcpkgLibDir 'libcrypto.lib')
        (Join-Path $VcpkgLibDir 'pixman-1.lib')
        (Join-Path $VcpkgLibDir 'libexpat.lib')
        (Join-Path $VcpkgLibDir 'libxml2.lib')
        (Join-Path $VcpkgLibDir 'jpeg.lib')
        (Join-Path $VcpkgLibDir 'libpng16.lib')
        (Join-Path $VcpkgLibDir 'libwebp.lib')
        (Join-Path $VcpkgLibDir 'libwebpdemux.lib')
        (Join-Path $VcpkgLibDir 'z.lib')
        (Join-Path $VcpkgLibDir 'bz2.lib')
        (Join-Path $VcpkgLibDir 'brotlicommon.lib')
        (Join-Path $VcpkgLibDir 'brotlidec.lib')
        (Join-Path $VcpkgLibDir 'libsharpyuv.lib')
        # Apotheosis 2026-09-26: vcpkg's ARM cairo.lib is built with the win32 GDI backend, so it needs
        # gdi32 for __imp_SaveDC / GetGlyphOutlineW / GetFontData / DeleteObject / SetTextColor ...
        # (the x64 line gets the same symbols from Src/harness/gdi-stubs.cpp instead). Name-only,
        # resolved through the SDK um\arm LIBPATH that the ARM env script sets.
        # Apotheosis 2026-09-26: gdi32.lib is deliberately NOT linked. vcpkg's ARM cairo.lib is built
        # with the win32 GDI backend, and UWP has no GDI: linking the SDK 10.0.19041.0 um\arm\gdi32.lib
        # still leaves __imp_SaveDC / GetGlyphOutlineW / GetFontData / GetTextMetricsW unresolved.
        # Those come from Src/harness/gdi-stubs.cpp, added to $Sources for ARM above.
        'WindowsApp.lib'
    )
} else {
    @(
        (Join-Path $ANGLELibDir 'libEGL.lib')
        (Join-Path $ANGLELibDir 'libGLESv2.lib')
        (Join-Path $VcpkgLibDir 'cairo.lib')
        (Join-Path $VcpkgLibDir 'pixman-1.lib')
        (Join-Path $VcpkgLibDir 'freetype.lib')
        (Join-Path $VcpkgLibDir 'harfbuzz.lib')
        # Apotheosis 2026-09-26: WebCore/LocaleICU.cpp calls hb_icu_script_to_script, which lives in
        # harfbuzz-icu, not harfbuzz. Without it the static WebCore link leaves it unresolved.
        (Join-Path $VcpkgLibDir 'harfbuzz-icu.lib')
        (Join-Path $VcpkgLibDir 'fontconfig.lib')
        (Join-Path $VcpkgLibDir 'libexpat.lib')
        (Join-Path $VcpkgLibDir 'libcurl.lib')
        (Join-Path $VcpkgLibDir 'libssl.lib')
        (Join-Path $VcpkgLibDir 'libcrypto.lib')
        (Join-Path $VcpkgLibDir 'libxml2.lib')
        (Join-Path $VcpkgLibDir 'sqlite3.lib')
        (Join-Path $VcpkgLibDir 'z.lib')
        (Join-Path $VcpkgLibDir 'bz2.lib')
        (Join-Path $VcpkgLibDir 'brotlicommon.lib')
        (Join-Path $VcpkgLibDir 'brotlidec.lib')
        (Join-Path $VcpkgLibDir 'jpeg.lib')
        (Join-Path $VcpkgLibDir 'libpng16.lib')
        (Join-Path $VcpkgLibDir 'libwebp.lib')
        (Join-Path $VcpkgLibDir 'libwebpdemux.lib')
        (Join-Path $VcpkgLibDir 'libsharpyuv.lib')
        (Join-Path $IcuLibDir 'icuuc.lib')
        (Join-Path $IcuLibDir 'icuin.lib')
        (Join-Path $IcuLibDir 'icudt.lib')
        'WindowsApp.lib'
    )
}

# Apotheosis: fail closed on a missing library, and say which one. lld-link's own
# "could not open 'cairo.lib': no such file or directory" does not say which search path was used.
# Only full-path entries are checked here; name-only ones (WindowsApp.lib) are resolved by lld
# through /LIBPATH, exactly as before.
$MissingLibs = @($ThirdPartyLibs | Where-Object { $_ -match '[\\/]' -and -not (Test-Path $_) })
if ($MissingLibs.Count -gt 0) {
    Write-Host "!! $($MissingLibs.Count) link library/libraries missing:" -ForegroundColor Red
    $MissingLibs | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    exit 1
}

# Build link command
# Apotheosis 2026-09-26: lld-link replaced with MSVC link.exe, the linker the native harness
# already uses for these exact port objects (Harness.vcxproj links WebCoreDriver-gpu-x64.lib,
# which is just these 13 objects archived). Two reasons lld-link could not do this link:
#   * cairo: WebCore.dll exports 219 cairo_* symbols, so its import library contains a thunk
#     member for each; linked next to cairo-complete-x64-uwp.lib, lld-link reports every one of
#     them as "duplicate symbol". link.exe resolves the same input without complaint (that is
#     literally the harness's AdditionalDependencies line).
#   * the protected RenderTheme virtuals in stubs-other.cpp's vtable, which is why the ARM DLL
#     step was removed in the first place; as import references they resolve at load time.
$LinkExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe'
if (-not (Test-Path $LinkExe)) {
    Write-Host "!! link.exe not found at $LinkExe" -ForegroundColor Red
    exit 1
}
$LinkArgs = @(
    "/DLL"
    "/MACHINE:$Machine"
    "/NOLOGO"
    "/OUT:$DllOut"
    "/DEF:$DefFile"
    "/APPCONTAINER"
    "/MANIFEST:NO"
    "/OPT:REF"
    "/INCREMENTAL:NO"
    # Apotheosis 2026-09-26: the crash we are chasing is reported as a raw module offset
    # (WebCoreDriver.dll+0x22121C) with no symbols, because nothing was being emitted. /DEBUG
    # gives the loader a PDB for the native stack, and /MAP gives a flat text of public
    # symbols + addresses, which is what actually turns that offset into a function name.
    # /MAP is the cheap one: it needs no debug info in the objects to be useful.
    "/DEBUG"
    "/PDB:$PortDir\$([IO.Path]::GetFileNameWithoutExtension($DllName)).pdb"
    "/MAP:$PortDir\$([IO.Path]::GetFileNameWithoutExtension($DllName)).map"
    # /FORCE:MULTIPLE for both architectures now: the x64 driver links WebCore statically AND
    # cairo-complete-x64-uwp.lib, so the Cairo public API exists twice (WebCore exports + static
    # code). The ARM reason is documented separately below.
)

# Apotheosis 2026-09-26 (ARM only): JavaScriptCore.lib is NOT an import library on this target --
# llvm-nm counts 16094 defined symbols and its members are named after the DLL, so it already
# carries the WTF objects. Linking it together with WTF.lib (which the ARM list does, twice each,
# for the JSC<->WTF circular dependency) therefore makes link.exe report LNK2005 for every shared
# WTF symbol (StringImpl::destroy, WTF::fastMalloc, String::utf8, ...). Those are duplicates, not
# missing symbols, so FORCE:MULTIPLE keeps the first definition and lets the link finish.
# Deliberately NOT applied to x64: that is the line currently under investigation and it does
# not need it.
if ($Arch -eq 'arm') { $LinkArgs += '/FORCE:MULTIPLE' }

# Library search paths
$LibPaths = @(
    "$BuildDir\lib"
    $ANGLELibDir
    $VcpkgLibDir
    $IcuLibDir
)
foreach ($d in $LibPaths) {
    if (Test-Path $d) { $LinkArgs += "/LIBPATH:$d" }
}

# Object files
foreach ($o in $Compiled) {
    if (Test-Path $o) { $LinkArgs += $o }
}

# WebKit static libs
foreach ($l in $WebKitLibs) {
    if (Test-Path $l) { $LinkArgs += $l }
}

# Third-party libs
foreach ($l in $ThirdPartyLibs) {
    $LinkArgs += $l
}

Write-Host "==> Linking $DllName" -ForegroundColor Cyan
$LinkLog = Join-Path $PortDir "link-$Arch-dll.log"

# Apotheosis 2026-09-26: on x64 the driver DLL must link the DESKTOP MSVC CRT, not the store (UWP)
# one. The env script puts ...\MSVC\14.44.35207\lib\x64\store FIRST in $env:LIB, and the
# msvcprt.lib in there binds to MSVCP140_APP.dll / VCRUNTIME140_APP.dll -- the AppContainer CRT,
# which exists nowhere on this machine (not System32, not the Kits, not vcpkg). The payload, and the
# driver that actually ran and reached Frame::init(), use the desktop names. Scoped to the link step
# only: the compile step still wants the store includes. Verified with llvm-nm on both msvcprt.lib --
# lib\x64\store -> MSVCP140_APP.dll, lib\x64 -> MSVCP140.dll.
#
# x64 ONLY, deliberately. ARM's cairo comes from vcpkg's arm-uwp and was built against the STORE CRT;
# removing store from ARM's LIB leaves every cairo object with an unresolved __security_cookie, which
# is the guard-check variable the store CRT defines. The two architectures really do differ here: x64
# uses the hand-built cairo-complete-x64-uwp.lib, ARM uses vcpkg's cairo.lib. So ARM keeps the store
# CRT, and whether a Lumia under W10M can resolve the _APP DLLs remains an open question.
$LibRestore = $env:LIB
try {
    if ($Arch -eq 'x64' -and $env:LIB) {
        $DesktopMsvc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\lib\x64\'
        $parts = @($env:LIB -split ';' | Where-Object { $_ -and $_.Trim() })
        $rest = @($parts | Where-Object { $_ -notmatch '\\store\s*$' })
        $env:LIB = (@($DesktopMsvc) + $rest) -join ';'
    }
    & $LinkExe @LinkArgs *> $LinkLog
    $linkExit = $LASTEXITCODE
} finally {
    $env:LIB = $LibRestore
}

$unresolved = @(Select-String -Path $LinkLog -Pattern 'unresolved external symbol')
$duplicate = @(Select-String -Path $LinkLog -Pattern 'duplicate symbol|already defined|LNK2005')

Write-Host "[link] EXIT=$linkExit  unresolved=$($unresolved.Count)  duplicate=$($duplicate.Count)"

if ($linkExit -eq 0) {
    # Also create import lib for linking
    # Apotheosis 2026-10-03: llvm-lib cannot infer the target machine from a .def alone -- MS
    # lib.exe reads it off the .obj files, but here there are none, so the call needs /MACHINE.
    # Without it llvm-lib writes "/def option requires /machine to be specified" to stderr, which
    # under $ErrorActionPreference='Stop' terminated the script right here -- *before* the export
    # count and, more importantly, before the payload staging below. The symptom is the one the
    # staging comment warns about: the link is clean, native\<arch> keeps the previous DLL, and
    # the fix reads as "did nothing". Verified by hand: with /MACHINE the same call produces a
    # 10 KB import library, without it, nothing.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'   # llvm-lib reports progress on stderr; that is not a failure
    & 'C:\Program Files\LLVM\bin\llvm-lib.exe' /OUT:$LibOut /DEF:$DefFile /MACHINE:$Machine | Out-Null
    $libExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($libExit -ne 0) { throw "llvm-lib failed (exit $libExit) writing $LibOut" }

    $dllSize = if (Test-Path $DllOut) { [math]::Round((Get-Item $DllOut).Length / 1KB, 1) } else { 0 }
    Write-Host "==> $DllName built successfully (${dllSize} KB)" -ForegroundColor Green
    # Apotheosis 2026-10-03: was the literal "30 functions", which had been wrong for a while --
    # the .def listed 37 before the viewport pair was added. A count that is typed by hand goes
    # stale silently and then reads as an answer to "is my export in the list?", which is the one
    # question it must not be trusted for. Count the list instead.
    $exportCount = @(Select-String -Path $DefFile -Pattern '^\s+WebCore[A-Za-z0-9_]+$').Count
    Write-Host "    Exports: $exportCount functions (via .def)" -ForegroundColor Green
    Write-Host "    Import lib: $LibName" -ForegroundColor Green

    # Apotheosis 2026-09-26: stage the fresh DLL into the appx payload, under the bare name the
    # DllImport declarations use. This used to be a manual step, and a driver that was built but
    # not staged is the single most common cause of "my fix did nothing" -- native\x64 keeps
    # serving the previous DLL, the hash in the appx matches the OLD one, and the change looks
    # like it was ignored. Doing it here makes that failure mode impossible.
    # WebCore.dll is deliberately NOT staged: the driver links WebCore statically.
    $payloadDir = Join-Path (Split-Path $PortDir -Parent) "Apotheosis\native\$($Arch.ToLower())"
    if (Test-Path $payloadDir) {
        $stage = Join-Path $payloadDir 'WebCoreDriver.dll'
        # Backups live OUTSIDE the payload directory. They are large (the x64 driver is 40 MB) and
        # the payload folder should contain only what goes into the package, so that "what is in
        # native\<arch>" is a question with an obvious answer. A .bak would not match the
        # *.dll glob anyway, so this is about clarity and disk, not correctness.
        $bakDir = Join-Path $PortDir '_driver-backups'
        if (Test-Path $stage) {
            New-Item -ItemType Directory -Path $bakDir -Force | Out-Null
            Copy-Item $stage (Join-Path $bakDir "WebCoreDriver-$Arch.dll.$((Get-Date).ToString('yyyyMMdd-HHmmss')).bak") -Force
        }
        Copy-Item $DllOut $stage -Force
        $h1 = (Get-FileHash $DllOut -Algorithm SHA256).Hash
        $h2 = (Get-FileHash $stage -Algorithm SHA256).Hash
        Write-Host "    Staged: $stage" -ForegroundColor Green
        if ($h1 -eq $h2) {
            Write-Host "    SHA256 matches build output: $($h1.Substring(0,16))..." -ForegroundColor Green
        } else {
            Write-Host "    !! STAGED COPY DIFFERS FROM THE BUILD OUTPUT !!" -ForegroundColor Red
        }
    } else {
        Write-Host "    (payload dir $payloadDir does not exist; skipped staging)" -ForegroundColor DarkYellow
    }
} else {
    Write-Host "==> LINK FAILED" -ForegroundColor Red
    if ($duplicate.Count -gt 0) {
        Write-Host "--- Duplicate symbols ---" -ForegroundColor Yellow
        $duplicate | Select-Object -First 10 | ForEach-Object {
            Write-Host "  $($_.Line.Trim())" -ForegroundColor Yellow
        }
    }
    if ($unresolved.Count -gt 0) {
        Write-Host "--- Unresolved symbols ---" -ForegroundColor Yellow
        $unresolved | Select-Object -First 20 | ForEach-Object {
            $sym = $_.Line -replace '.*error: undefined symbol: ', ''
            Write-Host "  $sym" -ForegroundColor Yellow
        }
    }
    exit 1
}

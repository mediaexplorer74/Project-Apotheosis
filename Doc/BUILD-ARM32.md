# ARM32 build line — Lumia 950 / Windows 10 Mobile

> **Current (rewritten 2026-09-26).** The target is no longer "make the native harness run on
> ARM" — it is "ship the **C# UWP app** on a real Lumia 950 under W10M". See [INDEX.md](INDEX.md) for
> the phase framing and [MANAGED-HOST.md](MANAGED-HOST.md) for what the managed host requires from
> the driver. The C++/CX harness build record that used to follow this file now lives in
> [Archive/2026-08-12-arm32-harness-build.md](Archive/2026-08-12-arm32-harness-build.md).

The single most expensive fact about this project is that rebuilding WebKit for ARM32 costs half a
day on this machine. The driver line does **not** require it, because the engine archives are

## Status measured 2026-09-26

Before planning an ARM rebuild, the actual state of `build-arm32-gpu` was measured rather than assumed.

| Item | State |
|---|---|
| `build.ninja` | 2026-08-22 08:52 — **~5 weeks stale** |
| Objects on disk | 1516 `.obj` (3.4 GB); 1512 / 1548 `.obj` edges (97.7%) present |
| `.ninja_log` | **absent** — see [LESSONS.md](LESSONS.md); treat the tree as needing a full ninja run |
| `.ninja_deps` | present, 2026-08-23 20:51 |
| Engine archives | `lib\WebCore.lib` (5 MB, import), `lib\JavaScriptCore.lib`, `lib\WTF.lib`, `lib\PAL.lib` |
| Engine DLLs | `bin\WebCore.dll` (33.2 MB), `bin\JavaScriptCore.dll` (10.4 MB) + PDBs |
| `build-arm-gpu` | empty — do not use |
| `native\ARM` | **empty (0 files)** — the payload has never been staged |

**The ARM line needs its own `WebCoreFull-arm32.lib` after all.** (An earlier note in this file said
otherwise; that was wrong, and the link log proves it.) Linking ARM in import mode leaves
`stubs-other.arm32.obj` with unresolvable WebCore *internals*:

```
LNK2019: WebCore::RenderTheme::RenderTheme()            (protected ctor)
LNK2019: WebCore::SystemFontDatabase::SystemFontDatabase()
LNK2019: WebCore::WebKitFontFamilyNames::standardFamily
LNK2001: WebCore::RenderTheme::purgeCaches / baselinePosition / supportsFocusRing / isControlStyled
LNK2019: __imp_cairo_scaled_font_status / _extents / _text_extents
```

The tempting argument for import mode — "as `__imp_` references they resolve at load time" — is only
true for **exported** symbols. `RenderTheme::RenderTheme()` is `protected` and is not in
`WebCore.dll`'s export table, so no `__imp_` thunk exists for it and there is nothing to resolve at
load time. x64 avoids this precisely because it links WebCore **statically**. The same
`WebCoreFull` idea is therefore required on ARM, built from the 963 WebCore objects in
`build-arm32-gpu\Source\WebCore`:

```powershell
$objs = Get-ChildItem build-arm32-gpu\Source\WebCore -Recurse -Filter *.obj
'"' + $_.FullName + '"'   | Set-Content build-arm32-gpu\WebCoreFull-arm32.rsp
& "...\MSVC\14.44.35207\bin\Hostx64\arm\lib.exe" /NOLOGO `
    /OUT:build-arm32-gpu\lib\WebCoreFull-arm32.lib "@build-arm32-gpu\WebCoreFull-arm32.rsp"
```

Note the path: `bin\Hostx64\arm\lib.exe`, not `Hostx64\x64` — a static library records its target
machine, so an x64 `lib.exe` would produce a library that `link.exe /MACHINE:ARM` cannot use.

**This also casts doubt on the x64 script change of 2026-09-26.** `build-driver-dll-arm32.ps1` was
switched to import mode (`WebCore.lib`) for both architectures on the reasoning that the full archive
wanted ICU 75. But `WebCoreFull-x64-19041.lib` has just been rebuilt from the 25.09 objects, which are
ICU 78, and the shipped x64 `WebCoreDriver.dll` demonstrably does **not** import `WebCore.dll` — it is
static. If the x64 branch is re-run as it stands, it should fail with the same internal-symbol errors.
That is the line under active investigation, so verify it deliberately rather than discovering it.


**The old "no ARM ICU exists" blocker is stale.** `bin\WebCore.dll` imports **`icuin78.dll` /
`icuuc78.dll`**, i.e. the ARM engine was built against ICU 78 from `C:\vcpkg\installed\arm-uwp`,
not ICU 75. The script's `$IcuLibDir` for ARM points at the same vcpkg tree, so link and import agree
and ICU is *not* a blocker. `C:\icu-arm-uwp` does not exist and is not needed.

### Blocker found 2026-09-26: the SEH guard does not compile for ARM32

The first ARM32 driver-DLL build attempt fails in the **compile** step, not the link step:

```
WebCoreDriver.cpp(5872,5): error: SEH '__try' is not supported on this target
5 warnings and 15 errors generated.
```

`clang-cl` targets `thumbv7a-windows-msvc` for this device and does not implement `__try` there. Because
the whole TU fails, **no** driver objects are produced and the link then reports
`LNK2001: unresolved external symbol` for *every* export (`WebCoreClickAt`, `WebCoreCloseSession`, …),
which looks like a missing import library and is not. The other 12 port sources compile fine.

This is a clang limitation, not an OS one — MSVC's ARM32 backend does support `__try/__except`, and
clang does support SEH on AArch64. The wrappers in `Src/port/WebCoreDriver.cpp` now go through
`APO_SEH_RETURN_INT` / `APO_SEH_CALL_VOID`:

- **x64** expands to the original `__try/__except`. Behaviour is byte-identical to the build that
  fixed WER `0x1007`.
- **ARM32** forwards straight to the `*Impl` body and therefore has **no native guard**. A native fault
  there still reaches the CLR's `FailFast(7)`. `-DAPOTHEOSIS_REQUIRE_SEH` turns this into a build error.

A real guard on ARM32 requires compiling `WebCoreDriver.cpp` with MSVC for that target; it is not
achievable with a preprocessor switch. That is a decision worth taking deliberately, because ARM32 is
where a crash is most likely and where the managed host can least afford to die.

## Result 2026-09-26: the ARM32 driver DLL builds

The chain below took the ARM line from *no driver DLL at all* to a working one:

```
==> Compiled 14/14 objects
==> Linking WebCoreDriver-arm.dll
[link] EXIT=0  unresolved=0  duplicate=142
==> WebCoreDriver-arm.dll built successfully (31349.5 KB)
    Exports: 30 functions (via .def)
```

`Src/port/WebCoreDriver-arm.dll` = **30.6 MB**, 30 exported entry points, zero unresolved symbols. Its
imports are 22 DLLs and **`WebCore.dll` is not among them** — WebCore is linked statically, exactly as
on x64. So `build-arm32-gpu\bin\WebCore.dll` does **not** need to ship in the payload after all.

The six things that had to be fixed, in the order they surfaced:

1. **`__try` is not available for ARM32** — 15 compile errors, see the blocker section below. Fixed with
   `APO_SEH_RETURN_INT` / `APO_SEH_CALL_VOID`.
2. **`GetModuleHandleExW` undeclared** on the ARM32 include path — 1 error. Declared explicitly behind
   `#if !defined(_LIBLOADERAPI_H_)`.
3. **`LNK2005` duplicates** between `WTF.lib` and `JavaScriptCore.lib` (the latter is a real object
   archive on ARM, not an import lib, and already contains the WTF objects). Fixed with
   `/FORCE:MULTIPLE` for ARM only.
4. **WebCore internals unresolved** in `stubs-other.arm32.obj` — fixed by building and linking
   `WebCoreFull-arm32.lib` instead of the import library. This was the big one.
5. **GDI symbols unresolved** — vcpkg's ARM `cairo.lib` uses the win32 GDI backend, and UWP has no GDI.
   `gdi32.lib` does **not** help: linking the SDK `10.0.19041.0 um\arm\gdi32.lib` still leaves
   `__imp_SaveDC` / `GetGlyphOutlineW` / `GetFontData` / `GetTextMetricsW` unresolved. The fix is
   `Src/harness/gdi-stubs.cpp`, which the x64 line gets through the harness vcxproj and the standalone
   driver link has to compile in itself.
6. **`hb_icu_script_to_script` unresolved** from `LocaleICU.cpp.obj` — it lives in `harfbuzz-icu.lib`,
   not `harfbuzz.lib`.

`duplicate=142` is expected and benign: those are the WTF symbols defined in both archives, and
`/FORCE:MULTIPLE` keeps the first definition.

### Remaining runtime dependency: the UWP CRT

The driver's imports include `MSVCP140_APP.dll`, `MSVCP140_2_APP.dll` and `VCRUNTIME140_APP.dll`.
No ARM copies exist on this machine (the only `*_APP.dll` files anywhere are x64 debug symbols under
`Windows Kits\10\Debuggers\x64\sym`). On x64 this never surfaced, because there the CRT is named
`MSVCP140.dll` / `VCRUNTIME140.dll` — and on x64 `WebCore.dll` is ballast anyway, since the driver links
WebCore statically.

**Likely resolution, still unverified.** `Apotheosis.csproj` carries
`<SDKReference Include="Microsoft.VCLibs, Version=14.0" />`. That is the framework-package dependency
through which Windows provisions the MSVC CRT for a packaged app, and it is the same mechanism that
gives the x64 payload its `MSVCP140.dll`. On an ARM32 device the same dependency is expected to
provision the `_APP`-named variants, because the UWP CRT is architecture-specific. So this is more
likely a provisioning question than a missing-binary question — but only a device can confirm it. If
the ARM app fails to start with a loader error, check this first; the fix is the ARM32 UWP
redistributable (`Microsoft.VCLibs.*.UWPDesktop`), never substituting x64 DLLs.

Note the asymmetry that forced the issue: x64 links the **desktop** CRT and ARM links the **store**
CRT, and that is not a style choice. ARM's `cairo.lib` comes from vcpkg and was built against the store
CRT; dropping `lib\arm\store` from `$env:LIB` leaves every cairo object with an unresolved
`__security_cookie`. The CRT flavour is dictated by the third-party libraries, so each architecture has
to be re-checked on its own. See [BUILD-X64.md](BUILD-X64.md) §7.

### What is still missing for a device run

- `native\ARM` **is staged** — 21 files, 85.2 MB, audited against the ARM driver's imports plus the
  dynamically loaded `icudt78.dll`: nothing missing, nothing extra. `WebCore.dll` is correctly absent.
- The appx must still be built with `/p:Platform=ARM` so the ARM payload is the one staged.
- The three `*_APP` CRT DLLs are the one unresolved item (above).
- No on-device validation has happened; the phase exit criterion in [INDEX.md](INDEX.md) is a real
  Lumia 950, and x64 results remain necessary but not sufficient.

### ARM payload inventory

`llvm-readobj --coff-imports` on the **built driver** (not on `WebCore.dll`, which no longer ships)
names 22 imports. Where each one lives:

| DLL | Source |
|---|---|
| `JavaScriptCore.dll` | `build-arm32-gpu\bin` |
| `libEGL.dll`, `libGLESv2.dll` | `Src\angle\arm` |
| `icuuc78`, `icuin78`, `libcrypto-3-arm`, `libssl-3-arm`, `fontconfig-1`, `freetype`, `harfbuzz`, `harfbuzz-icu`, `jpeg62`, `libcurl`, `libpng16`, `libwebp`, `libwebpdemux`, `libxml2`, `pixman-1-0`, `z` | `C:\vcpkg\installed\arm-uwp\bin` |
| `MSVCP140_APP`, `MSVCP140_2_APP`, `VCRUNTIME140_APP` | **not on this machine** — see the UWP CRT note above |

`WebCore.dll` is deliberately absent from that list: the driver links WebCore statically. Staging
`build-arm32-gpu\bin\WebCore.dll` would only add 33 MB of dead weight to the package.

already built:

| Artifact | State |
|---|---|
| `build-arm32-gpu/lib/WebCore.lib` | **present**, 5.0 MB, 2026-08-23 |
| `build-arm32-gpu/lib/JavaScriptCore.lib` | **present**, 2.2 MB |
| `build-arm32-gpu/lib/WTF.lib` | **present**, 33 MB |
| `build-arm32-gpu/lib/PAL.lib` | **present**, 6.8 MB |
| `Src/angle/arm/{libEGL,libGLESv2}.lib` | **present** |
| `C:\vcpkg\installed\arm-uwp/lib` | **present** — including `fontconfig.lib`, which x64-uwp lacks |
| ARM port objects `Src/port/*.arm32.obj` | present but **stale** (2026-08-24) |

So `pwsh -File Src/port/build-driver-dll-arm32.ps1 -Arch arm` is minutes, not half a day. Reserve
the WebKit rebuild for an actual WebKit change.

## 1. What the driver-DLL build script had wrong (fixed 2026-09-26)

`Src/port/build-driver-dll-arm32.ps1` is the one command that builds the driver DLL for either
architecture. It could not build *either* one; four independent defects, all fixed:

1. **`$Root` was one level too deep.** `Split-Path -Parent $PSScriptRoot` yields `<repo>\Src`, so
   every `$Root\WebKit\...` and `$Root\Src\angle\...` resolved to a path that does not exist. The
   x64 driver DLL in the payload had therefore been produced by hand, not by this script.
2. **The ARM branch pointed at a tree this repo does not have** (`build-clang-gpu`). The real ARM
   tree is `build-arm32-gpu`.
3. **`$EnvScript` for x64 sourced the *ARM* environment**, which set `APOTHEOSIS_TARGET=arm32` and
   put the arm32 toolchain on `INCLUDE`/`LIB`/`PATH`. The script poisoned its own environment: the
   x64 compile step then aborted in `assert-target.ps1` and `link.exe` could not find
   `WindowsApp.lib`. Each architecture now gets its own env script.
4. **The library list was one list, written for `lib.exe`**, which never resolves externals — a name
   that does not exist is simply never pulled in, so `cairo.lib`, `fontconfig.lib`, `expat.lib`,
   `sqlite3.lib`, `zlib.lib` and `libjpeg-turbo.lib` sat in the x64 command for months unnoticed.
   `lld-link` *does* resolve, and failed on six of them. The lists are now per-architecture, with a
   fail-closed existence check that names the missing library.

The compile step also used to parse `-I` paths out of `harness-cmd.bat`, which is an ARM-only dump
of the *previous* tree layout — so the x64 run inherited ARM include paths and every source died on
`no such file or directory`. Both architectures now delegate to the compilers that are kept in step
with the real trees (`compile-driver-gpu-x64.ps1` reads `build-x64-gpu\build.ninja`,
`compile-driver-gpu-arm32.ps1` reads `build-arm32-gpu\build.ninja`).

## 2. Why the ARM driver DLL was deleted once, and what actually blocks it

`link-driver-gpu-arm32.ps1` documents that its `lld-link` step was **removed on purpose**: the DLL
link "could not resolve the protected RenderTheme virtuals (`minimumControlSize`,
`adjustButtonStyle`, …) referenced by the `StubRenderTheme` vtable in `stubs-other.cpp`".

That diagnosis is right, but two conclusions drawn from it were wrong:

- **The fix is import mode, not more exports.** `build-arm32-gpu\lib\WebCore.lib` is an *import*
  library (every archive member is literally named `WebCore.dll`, 8540 `__imp_` symbols) for an ARM
  `WebCore.dll` that already exists. In import mode those protected virtuals become `__imp_`
  references resolved at load time. So the ARM driver DLL links `WebCore.lib` and ships
  `WebCore.dll` next to it in `native\ARM\`, exactly as the ARM harness does at runtime.
- **The gap is not ARM-specific.** Linking against the x64 import library leaves **58 distinct
  symbols unresolved** — all `RenderTheme` protected virtuals, plus `RenderTheme::RenderTheme()`,
  `SystemFontDatabase::SystemFontDatabase()`, `Font::initCharWidths()` and friends.
  `llvm-readobj --coff-exports` on `WebCore.dll` finds **zero** hits for `baselinePosition`: the
  DLL does not export them.

x64 avoids this because its driver DLL links a **full static WebCore archive**
(`build-x64-19041-probe\lib\WebCoreFull-x64-19041.lib`, 3.2 GB) where those symbols are real code.
**There is no equivalent ARM archive on this machine** — `build-arm32-gpu\lib` holds only the four
archives listed in §0. That is the concrete remaining work for the ARM driver line:

> **Option A (cheap, recommended).** Link the ARM driver DLL in import mode against
> `build-arm32-gpu\lib\WebCore.lib` and ship ARM `WebCore.dll` + `JavaScriptCore.dll` in
> `native\ARM\`. If the unresolved list matches x64's 58 symbols, it is the expected, benign case
> and the import model is working as designed.

> **Option B (expensive).** Build an ARM full static WebCore archive the way
> `build-x64-19041-probe` did (archive every ARM WebCore `.obj`). This is the multi-hour job, and
> only becomes necessary if import mode proves unworkable on W10M.

## 3. The ARM payload is empty

`Src/Apotheosis\native\ARM\` **does not exist** (0 files), while `native\x64\` holds 33 staged DLLs.
ARM packaging is therefore not just "build the driver" — the third-party runtime DLLs must be staged
too. The dependencies the x64 line had to add late are all present in the ARM triplet
(`C:\vcpkg\installed\arm-uwp\bin`, 34 DLLs):

```
brotlicommon.dll  brotlidec.dll  brotlienc.dll  bz2.dll  libexpat.dll  libsharpyuv.dll  iconv-2.dll
```

plus ARM builds of `freetype`, `harfbuzz`, `harfbuzz-icu`, `jpeg`, `libpng16`, `libwebp`,
`libwebpdemux`, `libxml2`, `z`, `libcurl`, `libssl`, `libcrypto`, `pixman-1`, `libEGL`, `libGLESv2`
and the ICU set, plus `build-arm32-gpu\bin\WebCore.dll` and `JavaScriptCore.dll` in import mode.
The five fonts under `Assets/fonts` are architecture-independent and already staged for x64.

## 4. ARM-specific traps that are already known

- **Three toolchains, never mixed**: clang-cl + lld-link for the engine and `Src/port/*.cpp`; MSVC
  v143 (`bin\Hostx64\arm\cl.exe`) for the C++/CX harness. C++ exceptions are off (`_HAS_EXCEPTIONS=0`
  + `/EHs-c-`) — clang's thumbv7 backend cannot lower `cleanupret`.
- **ASCII path, no spaces** — the Ruby generators and meson break otherwise.
- **`__try/__except` compiles for ARM too.** The managed-host SEH guard is not an x64-only
  construct; it must be re-verified by building `-Arch arm`, since the filter is compiled for
  thumbv7 there.
- **fontconfig is linked on ARM but not on x64.** `arm-uwp` has `fontconfig.lib` and its header;
  `x64-uwp` has neither. WebCore itself is built `USE_CAIRO 1`, `USE_SKIA 0`, no `USE_FREETYPE` /
  `USE_FONTCONFIG` on **both** architectures, so the cairo backend does the font work — which is
  part of why x64 can render at all without a working fontconfig configuration.
- **JIT is disabled on both lines** (`useJIT=false`): executing JIT-generated code inside the W10M
  AppContainer kills the process with no exception and no dump.
- **Cairo ARM float clamp**: on ARM32, `Canvas::setImageInterpolation` could corrupt a surface after
  a float overflow (NEON saturation vs x87). Guarded under
  `#if defined(__ARM_PCS_VFP) || defined(__thumb__)`.

## 5. Moving a build tree breaks `.lib` archives that hold absolute member paths

**Apotheosis 2026-09-26.** `cairo-complete-x64-uwp.lib` was created with `lib.exe`, and its members
are recorded with **absolute paths**:

```
llvm-ar t build-x64-gpu/libs/cairo-complete-x64-uwp.lib
  → C:\...\build-x64-gpu\libs\cairo.x64.obj        (absolute!)
  → build-x64-gpu\libs\psl\psl.lib                 (relative — a different archive)
```

`lib.exe` stores the path it was given. So the moment that directory is **renamed or moved**, the
archive is broken in a way that is easy to misread: the file is present, `Test-Path` says `True`,
its hash is unchanged, `llvm-ar t` lists all 134 members — and the link still fails with

```
lld-link: error: could not open 'cairo-complete-x64-uwp.lib': no such file or directory
```

The message names the **member name as written in the command** (a bare `cairo-complete-x64-uwp.lib`),
not the path that is actually missing, so it reads as "the library is absent" when the library is
right there. Two hours went into this. Three things that would have caught it sooner:

1. **`llvm-ar t <archive>` prints the member paths — always read them** after moving a tree.
2. **Rebuild the archive with relative members** rather than copying it:
   `llvm-ar rc <new>.lib <objs...>` run *from inside* the directory, so members are plain filenames.
   That is what the current `build-x64-19041-probe/libs/cairo-complete-x64-uwp.lib` now contains.
3. **Prefer full paths for such libraries in the link line.** `$b\libs\cairo-complete-x64-uwp.lib`
   rather than the bare name. Bare names depend on `/LIBPATH` resolution, which hides the real
   problem behind a misleading error; the full path fails loudly and immediately instead.

The decisive test for a tree is to make it unusable and confirm the build still passes:

```powershell
Rename-Item build-x64-gpu\libs libs-HIDDEN-test   # make the dependency impossible
pwsh -File build-x64-19041-probe/run-driver-link.ps1
# DRIVER_LINK_EXIT=0  → the tree is genuinely self-sufficient
Rename-Item build-x64-gpu\libs-HIDDEN-test libs    # put it back
```

Do this **before** deleting anything. "The file is where I copied it" is not the same claim as
"the build no longer needs the old tree", and only the second one justifies a deletion.

## 6. Renaming a build tree breaks 4655 symlinks silently

**Apotheosis 2026-09-26 — the expensive one.** The WebKit build trees are full of **symlinks**: the
generated-header directories are populated with links back into the WebKit source tree, so the
compiler sees a tidy `JavaScriptCore/Headers/...` layout. `build-x64-gpu` carries **4655 of them**.

Renaming the tree breaks every link whose *target* contains the old directory name. The failure is
almost impossible to read, because nothing in the text files mentions it:

```
fatal error: 'JavaScriptCore/Bytecodes.h' file not found
```

…and every check that looks like it should settle it says the file is fine:

| Check | Result | Why it lies |
|---|---|---|
| `Test-Path <file>` | `True` | the link entry itself exists |
| `Get-ChildItem -Filter Bytecodes.h` | finds it | enumeration sees the link, not its target |
| `Get-Item <file>.Length` | `0` | a symlink has no length of its own |
| `Get-Content <file>` / `File.ReadAllText` | **`DirectoryNotFoundException`** | the real target is gone |
| `build.ninja`, `CMakeCache.txt`, all `.ps1` | clean | symlink targets live in NTFS, not in text |

Only two things actually diagnose it:

```powershell
Get-ChildItem <tree> -Recurse -Force |
  Where-Object LinkType -eq 'SymbolicLink' | Select-Object -First 5 FullName, Target

Get-Item <file> -Force | Select-Object LinkType, Target
```

The fix, after renaming:

```powershell
$stale = Get-ChildItem <tree> -Recurse -Force |
  Where-Object { $_.LinkType -eq 'SymbolicLink' -and ($_.Target -join '') -like '*old-name*' }
foreach ($l in $stale) {
    $t = ($l.Target -join '').Replace('old-name', 'new-name')
    Remove-Item $l.FullName -Force
    New-Item -ItemType SymbolicLink -Path $l.FullName -Target $t -Force
}
```

Verify with a real build, then count what is still broken — both should be reported, not eyeballed:

```powershell
(Get-ChildItem <tree> -Recurse -Force | Where-Object LinkType -eq 'SymbolicLink').Count   # 4655
# ... and of those, how many fail Test-Path
```

**The rule: renaming a build tree is not a rename.** It is a rename *plus* a text sweep *plus* a
symlink sweep. Do all three, and prove it with a clean compile of the affected target — in this case
`WebCoreDriver.cpp`, which pulls in the JSC headers and fails within seconds if any link is stale.
A green `ninja` run would not have caught it: the engine objects are already built and ninja would
have considered them up to date.

## 7. Watching a build: `Src/tools/build-monitor.ps1`

A `-j1` WebKit build on this machine is hours of silence. The monitor reads `.ninja_log` and reports
done/total plus a rate-derived ETA, and it never touches the build:

```powershell
# leave this running in its own window; Ctrl+C stops it
pwsh -File Src/tools/build-monitor.ps1 -Tree build-arm32-gpu

pwsh -File Src/tools/build-monitor.ps1 -Tree build-x64-gpu -IntervalSec 20
pwsh -File Src/tools/build-monitor.ps1 -Tree build-x64-gpu -Once      # one line, for a scripted check
```

```
[19:05:40]   1512 / 1548   files   97,7%  ETA   17m 34s  #############################.
```

Two things it gets right that the obvious implementation gets wrong, both found by running it:

- **Count `.obj` edges only.** This tree's `.ninja_log` holds **20 526** entries, but ~19 000 are
  Python generator steps (`generate-js-builtins.py` and friends) that finish in milliseconds.
  Counting log lines gives "20526 / 1548 files, 1323%".
- **Count distinct outputs, not log lines.** The log is append-only and survives every run, so line
  counts accumulate across rebuilds. Distinct output paths are the stable numerator.

The ETA comes from the **most recent 15 minutes**, not the whole run: early edges are small and
fast, the late ones are the big unified sources, so a whole-run average reports a finish time far
earlier than the real one. `IDLE` means no edge completed in the last two minutes — useful, because
a build can be stuck in one long TU and still look busy.

## 8. Deployment

Manual, over WDP, from the x64 build machine:

```powershell
pwsh -File Src/tools/deploy-launch.ps1 -Ip <device> -Ver 0.2.0.6 -Pub <publisher>
```

`Apotheosis.csproj` must stage the ARM DLLs with `<Link>%(Filename)%(Extension)</Link>` so they land
at the **package root** — inside the AppContainer there is no `SetDllDirectory`, so a
`native\ARM\WebCoreDriver.dll` subdirectory makes `DllImport` fail. This is already configured for
x64 and is the first thing to check when the ARM build produces an app that will not start.


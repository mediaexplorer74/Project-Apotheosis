# Aug 18, 2026 - ARM32 WebCore.dll link: three root causes found & fixed

> Session log for whoever picks this up next. Goal of the session: finish the ARM32 line after
> the OS-reinstall rebuild - `WebCore.dll` link (build-arm32-gpu) was failing with three different
> root causes, each fixed in source. The link was relaunched at the end of the session; the result
> was not yet polled when this file was written. See the "Current state" section below.

## The three root causes, in the order they surfaced

### 1. x64-uwp library paths leaked into the ARM link

- **Symptom**: linker errors like "jpeg.lib machine type x64" for jpeg, libpng16, libwebp,
  libwebpdemux, pixman-1, freetype (the Cairo/vcpkg libs).
- **Root cause**: `WebKit/Source/WebCore/PlatformWinUWP.cmake` (lines 66-73) hardcoded
  `C:/vcpkg/installed/x64-uwp/lib/{jpeg,libpng16,libwebp,libwebpdemux,pixman-1,freetype}.lib`.
  This is the source of the x64 contamination in `build-arm32-gpu/build.ninja` - **not** the
  CMakeCache, which was verified clean.
- **Fix (source)**: arch gate added:
  ```cmake
  if (CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM")
      set(... "C:/vcpkg/installed/arm-uwp/lib/...")
  else()
      set(... "C:/vcpkg/installed/x64-uwp/lib/...")
  endif()
  ```
- All 6 libs verified present under `C:/vcpkg/installed/arm-uwp/lib/` before patching.
- A surgical stopgap patch of `build.ninja` (x64-uwp -> arm-uwp) was applied first (backup:
  `build-arm32-gpu/build.ninja.bak-x64paths`); it became obsolete once the cmake fix triggered an
  auto-regen (`[0/1] Re-running CMake...`) - the regenerated build.ninja carries the correct paths
  from source.

### 2. webcore-exports.def uses x64 MSVC mangling

- **Symptom** (after fix 1): link failed with 20+ undefined symbols; in lld's error output the
  `<root>` is the DEF file - every symbol was a member function from the .def
  (FrameSelection::setSelection, Element::setAttribute, GraphicsLayerTextureMapper::setBackgroundColor, ...).
- **Root cause**: x64 MSVC mangling embeds an `E` marker (e.g. `QEAA`, `AEBV`, `PEAV`, `UEBA`),
  ARM32 thiscall mangling drops it (`QAA`, `ABV`, `PAV`, `UBA`). The DEF file was generated for
  x64 and shared with the ARM link.
- **Verified empirically**, not guessed: probe TU compiled with clang-cl
  `--target=armv7-windows-msvc -mthumb` (`k32test/mangleprobe.cpp/.obj`):
  - `pub(C const&)` -> `?pub@C@@QAAXABV1@@Z`, `priv()` -> `?priv@C@@AAAXAAV1@@Z`,
    `virt() const` -> `?virt@C@@UBAXXZ`, ptr arg/ret -> `?ptr@C@@QAAPAV1@PAV1@@Z`,
    static -> `?stat@C@@SAXXZ`.
- **Fix**: new `Src/port/webcore-exports-arm32.def` (31 entries) via systematic transform.
  **Do NOT modify `webcore-exports.def` in place** - the x64 tree (build-x64-gpu) references it.
  `PlatformWinUWP.cmake` now selects the DEF per arch (WEBCORE_EXPORT_DEF), with a comment
  explaining the mangling difference.
- **Transform table x64 -> ARM32** (apply to member-function entries only; DATA/free/static
  (`SA`, `YA`) and vftable (`??_7...`) entries are unchanged):

  | x64 pattern | ARM32 replacement | | x64 pattern | ARM32 replacement |
  |---|---|---|---|---|
  | `$$QEAV` | `$$QAV` | | `QEAA` | `QAA` |
  | `$$QEAU` | `$$QAU` | | `AEAA` | `AAA` |
  | `AEBV` | `ABV` | | `IEAA` | `IAA` |
  | `AEAV` | `AAV` | | `QEBA` | `QBA` |
  | `AEBU` | `ABU` | | `UEBA` | `UBA` |
  | `AEAU` | `AAU` | | `UEAA` | `UAA` |
  | `PEBV` | `PBV` | | `AEBA` | `ABA` |
  | `PEAV` | `PAV` | | `IEBA` | `IBA` |
  | `PEBU` | `PBU` | | | |
  | `PEAU` | `PAU` | | | |

  One manual fix beyond the table: `setBackgroundColor@GraphicsLayerTextureMapper` needed
  `UEAA` -> `UAA` (the `UEAA` -> `UAA` row covers it - it was missed in the first pass and caught
  by grepping for residual `E` markers). Verify with: zero matches for
  `QEAA|AEAV|PEAV|AEBV|UEBA|QEBA|AEAA|IEAA` etc. in the arm32 DEF.

### 3. FontCascade::fontForCombiningCharacterSequence undefined (USE_HARFBUZZ)

- **Symptom** (after fix 2): exactly ONE undefined symbol left -
  `FontCascade::fontForCombiningCharacterSequence(StringView) const`, referenced from
  `ComplexTextController.cpp` (compiled as `UnifiedSource-3c72abbe-24.cpp.obj`).
- **Root cause chain**:
  - ARM: `USE_HARFBUZZ=ON`. `OptionsWinUWP.cmake` forces `HarfBuzz_FOUND TRUE`
    (needed: `LocaleICU.cpp:45-80` includes `hb-icu.h` under `#if USE(HARFBUZZ)`).
  - The generic fallback implementation in `FontCascade.cpp:1761-1773` is guarded
    `#if !PLATFORM(COCOA) && !USE(HARFBUZZ)` -> compiled OUT on ARM.
  - The USE(CAIRO) implementation lives in `platform/graphics/cairo/FontCairoHarfbuzzNG.cpp`,
    shipped by `platform/FreeType.cmake`, which is **not** included by `PlatformWinUWP.cmake` ->
    never compiled.
  - x64 was never hit: there `find_package(HarfBuzz COMPONENTS ICU)` fails (ICU NOTFOUND) ->
    `USE_HARFBUZZ=OFF` -> generic branch active -> x64 linked fine. This asymmetry is why only
    ARM broke.
- **Fix (port pattern, not upstream)**: stub in `Src/port/stubs-font-uwp.cpp`, inside the existing
  `#if PLATFORM(WIN) && USE(CAIRO) && defined(WK_WINUWP)` guard: `#include "FontCascade.h"` +
  implementation falling back to `glyphDataForCharacter`. Verified beforehand:
  `glyphDataForCharacter` definition at `FontCascade.cpp:434` is NOT behind any `#if`;
  declaration at `FontCascade.h:212`.

### 4. The ARM driver "link" step was an obsolete DLL link (WebCoreDriver-gpu.dll)

- **Symptom** (after fixes 1-3): the driver DLL link failed with ~40 undefined symbols, all
  `WebCore::RenderTheme` protected virtuals (`minimumControlSize`, `controlSize`,
  `adjustButtonStyle`, `adjustCheckboxStyle`, `adjustColorWellStyle`, `adjustInnerSpinButtonStyle`,
  `adjustMenuListStyle`, `adjustMeterStyle`, ...).
- **Root cause**: `stubs-other.cpp:405-411` defines `RenderTheme::singleton()` via a
  `StubRenderTheme : RenderTheme` whose vtable references every base protected virtual. Nobody in
  the port layer calls `RenderTheme::singleton()` (verified by grep), so in the HARNESS link the
  object file is never pulled from the static archive - no references, no problem (this is exactly
  how the x64 line works). But `link-driver-gpu-arm32.ps1` lld-linked ALL 12 driver objs directly
  into a DLL - and WebCore.dll does not export those protected virtuals (verified with
  llvm-readobj: RenderTheme exports on both x64 and ARM WebCore.dll = 7 color getters only).
- **Evidence the DLL was dead weight**: the harness never loads a driver DLL - it
  LoadPackagedLibrary's `JavaScriptCore.dll` first, then `WebCore.dll`
  (`MainPage.xaml.cpp:1350-1373`), and statically links `WebCoreDriver-gpu.lib`
  (`Harness.vcxproj:93`, ARM: `WebCoreDriver-gpu.lib;WebCore.lib;JavaScriptCore.lib;PAL.lib;WTF.lib;...`
  with `/FORCE:MULTIPLE`). No script references `WebCoreDriver-gpu.dll`.
- **Fix**: `link-driver-gpu-arm32.ps1` rewritten (Aug 18) to match `link-driver-gpu-x64.ps1`:
  compile the 12 sources, then archive them into `WebCoreDriver-gpu.lib` via llvm-lib - no DLL
  step. Result: `WebCoreDriver-gpu.lib` 1.01 MB / 12 objs. Also added env-var self-derivation
  fallback to `compile-driver-gpu-arm32.ps1` (same pattern as the link script).

### 5. Port objects compiled with dllexport semantics -> direct data refs (LNK2019 on ARM32)

- **Symptom**: the ARM harness LINK (the first real link of the whole chain) failed with exactly
  8 LNK2019s, all DATA symbols from `WebCoreDriver.arm32.obj` inside WebCoreDriver-gpu.lib:
  `HTMLNames::{aTag,scriptTag,inputTag,textareaTag,classAttr,idAttr,srcAttr}` and
  `ResourceRequestBase::s_defaultTimeoutInterval`. Everything else resolved.
- **Investigation chain**: the ARM WebCore.dll DOES export all 8 (llvm-readobj) and the ARM
  import lib carries `__imp_` thunks (llvm-nm) -> the import lib was not the problem. The x64
  Harness.exe provably imports `aTag`/`s_defaultTimeoutInterval` (dumpbin /imports) -> the
  mechanism works on x64. The difference: the x64 driver objs reference `__imp_?aTag@...`
  (llvm-nm on WebCoreDriver.x64.obj) while the ARM objs reference the plain `?aTag@...` -
  MSVC link auto-thunks direct data references on x64 (RIP-relative) but NOT on ARM32.
- **Root cause**: `WEBCORE_EXPORT` in `platform/PlatformExportMacros.h` is `WTF_IMPORT_DECLARATION`
  (dllimport) for consumers, but dllexport when `BUILDING_WebCore` is defined. The ARM compile
  script lifted its flags from the STALE `harness-cmd.bat` (original dev box), which carries
  `-DBUILDING_WebCore` (verified in the bat). The x64 compile script
  (`compile-driver-gpu-x64.ps1:27-34`) explicitly strips `-DBUILDING_WebCore` and
  `-DWebCore_EXPORTS` for exactly this reason (its comment even names `HTMLNames::aTag`).
- **Fix**: `compile-driver-gpu-arm32.ps1` now strips the same two defines from the bat-derived
  command (English Apotheosis comment). All 12 objs recompiled + re-archived with the fix.
  The stale `harness-cmd.bat` remains as the flag template - only the defines differ.

## Engineering notes worth knowing (do not re-learn these)

- **Bindings edge is always "dirty" by design**: the stamp
  `Source/WebCore/CMakeFiles/WebCoreBindings` is never created, so "Generate bindings" re-runs on
  every ninja invocation - but `restat=1` drops the step counter after it runs
  (observed `[1/659]`->`[2/172]`, `[1/502]`->`[2/4]`). Consequently `ninja -n` (dry run) massively
  overestimates the plan (500 steps = 35 bindings + 464 objs + link); trust the real run's log,
  not the dry run.
- **Build discipline**: ARM env via `Src/setenv.ps1` + `Src/port/arm32-uwp-env.ps1`;
  strictly `ninja -j1` (4 GB box, constant swap); long builds detached
  (`Start-Process pwsh -WindowStyle Hidden -PassThru`, `PriorityClass = BelowNormal`);
  log `C:\Users\media\AppData\Local\Temp\opencode\k32test\webcore-link.log`, runner
  `k32test\run-webcore.ps1`.
- **Link command shape**: `lld-link @CMakeFiles\WebCore.rsp /out:bin\WebCore.dll
  /implib:lib\WebCore.lib /machine:ARM /APPCONTAINER /OPT:NOICF /OPT:REF
  /DEF:...webcore-exports-arm32.def`. OOM fallback: drop `/DEBUG`.
- **JSC/WTF/PAL/bmalloc are OBJECT libraries** on ARM -> the driver link needs their .obj files.
- Cross-checking .ninja_deps (5.1 MB) showed fresh object timestamps; .ninja_deps is intact.

## Current state (Aug 18, ~22:45)

- `PlatformWinUWP.cmake` (libs + DEF per arch) and `stubs-font-uwp.cpp` (stub) fixed in source.
- **WebCore.dll LINKED OK (21:56:40, 32.8 MB)** - `build-arm32-gpu\bin\WebCore.dll` +
  `lib\WebCore.lib`.
- **WebCoreDriver-gpu.lib archived OK (22:43:07, 0.99 MB / 12 objs)** - driver DLL step removed
  (cause #4); objs recompiled after cause #5 fix (all data refs now `__imp_`, verified via llvm-nm).
- **ARM harness + appx BUILT OK (22:44:02)** - `Src\harness\AppPackages\Harness\Harness_0.1.9.3_ARM_Test\Harness_0.1.9.3_ARM.appx`
  (46 MB). Contents verified fresh: WebCore.dll (32.8 MB), JavaScriptCore.dll (10.4 MB),
  Harness.exe, ANGLE arm (libEGL/libGLESv2), full vcpkg arm-uwp DLL set (cairo, freetype,
  harfbuzz, curl, ssl, libxml2, sqlite...), ICU78 (icuuc78/icuin78/icudt78.dll).
  xaml/Connect check passed (61 fields / 88 case labels). Run: `build-harness.ps1` with
  `APOTHEOSIS_ARCH=arm` + `APOTHEOSIS_BUILD_GPU=<root>\build-arm32-gpu\lib` (the ARM
  AdditionalLibraryDirectories do NOT hardcode the ARM build dir, unlike x64's - the env var
  becomes the MSBuild property).
- **ICU on ARM is NOT a mismatch - it is intentional (resolved Aug 18)**: the ARM line runs on
  vcpkg ICU **78** by design (Harness.vcxproj:159-161 documents it; the ARM engine is compiled
  against vcpkg ICU 78 headers, WebCore.dll imports icuuc78.dll -> icudt78.dll with 31.6 MB of
  REAL data inside - no .dat injection needed). The harness's `LoadPackagedLibrary("icuuc75.dll")`
  block (MainPage.xaml.cpp:320, x64-era stub-ICU hack) is guarded by `if (hIcu)` and simply skips
  on ARM (file absent). By contrast the x64 line IS ICU75 (engine imports icuuc75/icuin75, package
  ships icuuc75+icuin75+icudt75 STUB + icudt75l.dat, harness injects the .dat) - consistent there,
  but hackier. Nothing to fix; the ubrk crash path that motivated the x64 hack does not exist on
  ARM because icudt78.dll carries real data.
- `build.ninja.bak-x64paths` kept in build-arm32-gpu (obsolete after regen, harmless).
- mangleprobe artifacts: `k32test/mangleprobe.cpp`, `k32test/mangleprobe.obj`.

## Deployment (next, manual)

- Device: Lumia 950, WDP at `https://192.168.3.51:443` (Deploy-Robust.ps1 default IP) - was NOT
  reachable at session end (device off or on another network).
- `Src\tools\Deploy-Robust.ps1 -Ip <ip>` or `Wdp-Deploy.ps1` against the ARM appx above;
  crash diagnostics (exit-ok.txt / crashverdict.txt / crash-report.txt) will tell on-device
  health without a debugger.

## Next steps (handover checklist)

1. ~~Poll `webcore-link.log`~~ DONE: WebCore.dll linked, no FAILED lines.
2. ~~Driver link~~ DONE: WebCoreDriver-gpu.lib archived (causes #4/#5 fixed).
3. ~~Harness ARM build~~ DONE: Harness_0.1.9.3_ARM.appx packaged.
4. **Deploy to the Lumia 950 via WDP** (device was offline at session end): Deploy-Robust.ps1 /
   Wdp-Deploy.ps1, then check LocalState diagnostics (exit-ok.txt, crashverdict.txt,
   crash-report.txt, jitresult.txt) + the trace suite.
5. If ICU-related text failures on device: arch-gate the icuuc75/icudt75l.dat injection in
   MainPage.xaml.cpp to use icuuc78 on ARM.
6. x64 line (appx 0.1.8.51 / 0.1.9.3) must stay untouched; never build the appx with
   `/p:MinimalTest=true` (links no engine at all).
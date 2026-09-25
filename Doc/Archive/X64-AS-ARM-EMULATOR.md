# X64 as an ARM32 emulator — scope, fidelity, coexistence

*Companion to `Doc/ARM32-RECOVERY.md` (the "machine was reinstalled, rebuild everything" checklist)
and `Doc/ARM32-BUILD-GUIDE.md` (how the ARM32 build works). This file answers one question: **how
much of a Lumia-950 defect can be found, reproduced and fixed on the Win11 x64 dev box, and what
must be enabled so the x64 build never stops working while the ARM32 build is brought back up.**
Everything below was verified on this machine on 2026-08-16 with the quoted paths and commands;
inferences that were not verified are marked UNVERIFIED.*

## 0. The guarantee this document is about

An untested ARM32 build on the phone will misbehave. When it does, the fix cycle must not depend on
the phone: it must be possible to reproduce, instrument and fix on x64, then ship one ARM32 build.
The x64 build is therefore not a convenience — **it is the debugger for the ARM32 build.** Two rules
follow, and the rest of this document exists to keep them true:

1. **No ARM32-enabling change may break `Release|x64`.** Every such change is validated by a full
   x64 msbuild before it is kept.
2. **Every defect that *can* be found on x64 must be found on x64 first.** The phone cycle (deploy
   over dropping Wi-Fi, no debugger, no dump on `__fastfail`) costs orders of magnitude more.

## 1. Verified machine state (2026-08-16)

| Item | Path | State |
|---|---|---|
| MSVC toolset (only one on disk) | `VC\Tools\MSVC\14.44.35207` | ✅ both default stamp files agree on it |
| Toolset headers (shared by all targets) | `14.44.35207\include` | ✅ single directory, not per-arch |
| ARM32 CRT/STL libs | `14.44.35207\lib\onecore\arm` | ✅ ~48 libs, verified `coff-arm` / `thumb` |
| ARM32 store CRT libs | `14.44.35207\lib\arm\store` | ❌ missing (`arm32-uwp-env.ps1` demands it) |
| ARM32 `cl.exe` (harness only) | `bin\Hostx64\arm\cl.exe` | ❌ missing (VS Installer component) |
| SDK with ARM32 headers+libs | `Windows Kits\10\Lib\10.0.19041.0\{um,ucrt}\arm` | ✅ present |
| SDK 26100 | `Windows Kits\10\Lib\10.0.26100.0` | ⚠️ **no `arm`** (x64/x86/arm64 only) |
| ANGLE ARM32 | `Src\angle\arm` | ✅ verified `IMAGE_FILE_MACHINE_ARMNT`, Store/D3D11 flavor |
| ICU 75 ARM32 | `C:\icu-arm-uwp` | ❌ missing — the one external blocker (§11) |
| vcpkg `arm-uwp` | `C:\vcpkg\installed\arm-uwp` | ❌ missing |
| Ruby (WebCore IDL generators) | on `PATH` | ❌ missing |
| Source-built deps | `deps-build\` | ⚠️ only `harfbuzz-x64` (see §10) |
| Disk free | `C:` | 52.3 GB free of 118.3 GB |

The `lib\onecore\arm` row is the significant find: it was installed as a side effect of package
`Microsoft.VC.14.44.17.14.CRT.arm.OneCore.Desktop.base` (14.44.35226) into the pinned toolset tree.
The **engine** is compiled by clang-cl, which needs the CRT *libraries* plus the shared *headers* —
not `cl.exe`. So the ARM32 engine may be much closer to buildable than `ARM32-RECOVERY.md` §1
assumes; `bin\Hostx64\arm\cl.exe` is required only for the C++/CX harness. UNVERIFIED whether
`lib\onecore\arm` can substitute for `lib\arm\store`; the cheap test is one TU compiled with
clang-cl plus an `lld-link /MACHINE:ARM` probe with the LIBPATH swapped.

## 2. The two builds, exact inventory

Both builds share one `Package.appxmanifest`, one C ABI, one harness source tree and one WebCore
source tree. They differ in how that code is linked and what runtime ships beside it. The x64 column
was **measured inside the produced package**
(`Src\harness\AppPackages\Harness\Harness_0.1.8.51_x64_Test\Harness_0.1.8.51_x64.appx`,
53 676 896 B, 30 binaries + `cacert.pem`); the ARM column is what `Harness.vcxproj` declares, since
no ARM package exists yet.

| | x64 (measured) | ARM32 (declared) |
|---|---|---|
| Engine | **`WebCore.dll` 46.2 MB + `JavaScriptCore.dll` 18.8 MB** (shared) | **static** `WebCore.lib`+`JavaScriptCore.lib`+`PAL.lib`+`WTF.lib` into `Harness.exe` |
| CRT | **desktop** `MSVCP140`, `MSVCP140_2`, `VCRUNTIME140`, `VCRUNTIME140_1` (copied from `C:\Windows\System32`) | none packaged — framework `Microsoft.VCLibs.140.00 (ARM)` = **`_APP` CRT** |
| cairo | static `cairo-complete-x64-uwp.lib`, **no DLL in package** | `cairo.lib` + **`cairo-2.dll`** |
| fontconfig | **absent entirely** (not linked, not packaged) | `fontconfig.lib` + **`fontconfig-1.dll`** |
| ICU 75 | `icuuc75` 1.8 MB, `icuin75` 3.0 MB, `icudt75.dll` 36 KB stub, `icudt75l.dat` **30.7 MB** | same four names from `C:\icu-arm-uwp\bin` (does not exist yet) |
| ANGLE | `libEGL.dll` 33 KB + `libGLESv2.dll` 2.0 MB (`Src\angle\x64`) | same names from `Src\angle\arm` (present, ARMNT) |
| Text/graphics | `freetype`, `harfbuzz`, `pixman-1-0`, `jpeg62`, `libpng16`, `libwebp`, `libwebpdemux`, `libsharpyuv` | same set from `arm-uwp\bin` |
| XML/compress | `libxml2`, `libexpat`, `z`, `bz2`, `brotlidec`, `brotlicommon`, **`iconv-2`** | same minus `iconv-2` |
| Net/TLS | `libcurl`, `libssl-3-x64`, `libcrypto-3-x64`, `cacert.pem` | `libcurl`, `libssl-3-arm`, `libcrypto-3-arm`, `cacert.pem` |
| Extra link inputs | `psl.lib`, `windowsapp.lib` explicit | neither (documented asymmetry, `ARM32-BUILD-GUIDE` §6) |
| Engine config delta | bmalloc | `-DUSE_SYSTEM_MALLOC=ON` |

Consequences worth internalising, because they are where "it worked on x64" stops being evidence:

- **Engine linkage is the largest irreducible gap.** On x64 the engine is two DLLs: static
  initialisers run at DLL load, symbols resolve dynamically, and `/OPT:REF` cannot strip across the
  DLL boundary. On ARM32 the same code is archived into `Harness.exe`: different initialisation
  order, one link closure, and dead-stripping that the 146 stubs must survive. Class of defect this
  hides: static-init order crashes and missing-symbol/stripped-symbol failures.
- **The CRT under test is a different library.** x64 exercises the desktop CRT (`MSVCP140.dll`);
  the phone can only use the `_APP` CRT. Anything CRT-behavioural (locale, `_putenv_s`, stdio,
  thread-local init) is untested by x64 by construction.
- **fontconfig exists only on ARM32.** Font *selection and fallback* therefore run through a
  component x64 does not have. This is exactly the area where ya.ru's text broke (§9), so a font fix
  validated on x64 must be re-validated on the phone rather than assumed.
- **`icudt75l.dat` is architecture-neutral** (the `l` suffix is little-endian; ARM32 is
  little-endian too). It transfers byte-for-byte from `C:\icu-x64-uwp\bin`, which narrows the ICU
  blocker to ~5 MB of code (§11).

## 3. Fidelity matrix — by class of defect, not by one percentage

A single "the x64 build is N% faithful" number is misleading: fidelity is near-total for some defect
classes and exactly zero for others. Budget testing effort per class.

| Defect class | Reproducible on x64 | Why |
|---|---|---|
| Layout / CSS / DOM / rendering logic | ~95% | identical WebCore sources, identical `WK_WINUWP` patches, same cairo raster code |
| JavaScript semantics | ~95% | same JSC, and **both builds are baseline-JIT-only** (`ENABLE_DFG_JIT=OFF`, `ENABLE_FTL_JIT=OFF`) |
| Network / TLS / cert handling | ~95% | same curl+OpenSSL, same packaged `cacert.pem`, same `CURLOPT_CAINFO_BLOB` path |
| ICU / text encoding / CJK | ~90% | same ICU 75, and the data blob is byte-identical |
| Driver logic, C ABI, threading model | ~95% | same `Src\port` sources, same single-engine-thread discipline |
| Harness UI, gestures, navigation, diag | ~95% | same C++/CX sources; only DPI and input device differ |
| App Container sandbox behaviour | ~80% | both are real AppContainers; OS API surface differs (Win11 vs Win10M 15254) |
| GPU compositing recipe | ~70% | same ANGLE build and TextureMapper path, but D3D11 FL11+ vs **FL9_3** (§8 can raise this) |
| Font selection / fallback | ~50% | **fontconfig is ARM-only**; x64 has a different family-resolution path |
| Static-init order, link closure | ~20% | DLL engine vs static engine (§2) |
| CRT behaviour | ~10% | desktop CRT vs `_APP` CRT |
| Memory pressure / OOM | ~30% | phone has 2 GB and a much smaller commit budget than the dev box |
| **ARM codegen and ABI** | **0%** | different backend entirely — see the list below |

The zero row is the one that must never be rationalised away. It contains, from the landmine list:

- `std::partial_ordering` sret ABI split (the `ALWAYS_INLINE` `WK_WINUWP` header fix)
- exceptions: thumbv7 cannot lower `cleanupret` → `_HAS_EXCEPTIONS=0` + `/EHs-c-`
- ARM NEON float→int saturation differing from x87 → the cairo clamp guarded by
  `#if defined(__ARM_PCS_VFP) || defined(__thumb__)`
- thumbv7 code-size and alignment effects, and JIT baseline codegen for ARM
- `Frame::Navigate` → `0xc000027b` (banned; navigate through the C ABI)

**Corrections to earlier claims** made in the course of verifying this table:

- `CLAUDE.md`'s build-config table says `build-x64-gpu` includes `FTL_JIT`. It does not:
  `configure-gpu-x64.ps1:40` sets `-DENABLE_FTL_JIT=OFF`, and `build-x64-gpu\cmakeconfig.h:29`
  confirms `ENABLE_FTL_JIT 0`. Both builds are baseline-JIT-only, so the JIT row is a *strength* of
  the emulator, not a gap.
- A full flag diff of `configure-gpu-x64.ps1` vs `configure-gpu-arm32.ps1` yields **exactly one**
  engine-config difference: ARM adds `-DUSE_SYSTEM_MALLOC=ON` (x64 keeps bmalloc). That is a real
  allocator-behaviour divergence and belongs in the "memory pressure" row.
- The x64/ARM32 **SDK** divergence that earlier notes treated as inherent is not inherent: SDK
  **19041 serves both targets** (§5).

## 4. Can the VC toolset be *identical* for x64 and ARM32? Yes — by construction

This question has a definite answer, and it is not "the versions will be similar but slightly
different". Verified on this machine:

1. **There is exactly one toolset on disk:** `VC\Tools\MSVC\14.44.35207`. Both
   `Microsoft.VCToolsVersion.v143.default.props` and the corresponding `.txt` stamp name that same
   version — there is no second v143 to drift to.
2. **The headers are one shared directory.** `14.44.35207\include` is not per-architecture, so the
   CRT and STL headers compiled into the ARM32 engine are *literally the same files* as those
   compiled into the x64 engine. Header-level divergence is impossible.
3. **Only two things are per-target:** `bin\Hostx64\<target>\cl.exe` and `lib\<target>\`. Both are
   emitted by the same toolset build (14.44.35207), so the CRT/STL *implementation* matches the
   headers on both sides.

So "identical VC for both targets" is not something to configure — it is guaranteed as long as one
toolset is used. The real hazard is procedural, and it is worth stating precisely:

> Installing the **unversioned / "latest"** ARM build-tools component in the VS Installer can drop a
> *newer* v143 toolset alongside 14.44.35207 and flip the default stamp files to it. The x64 build
> would then silently start compiling against a different CRT than the prebuilt `build-x64-gpu`
> engine — the exact class of breakage rule 1 (§0) forbids.

Two mitigations are in place:

- Select the **versioned** component (*MSVC v143 – VS 2022 C++ ARM build tools (v14.44-17.14)*),
  not the unversioned one.
- `Harness.vcxproj` now pins `<VCToolsVersion>14.44.35207</VCToolsVersion>` in the `Globals` group.
  This works because `Microsoft.Cpp.VCTools.props` imports the default props with
  `Condition="'$(VCToolsVersion)' == ''"`, so an explicit project value survives and wins. MSBuild
  was the **only** unpinned consumer: the clang lines already hardcode the toolset through
  `APOTHEOSIS_MSVC` (`setenv.ps1:21`) and `arm32-uwp-env.ps1`.

## 5. One SDK serves both targets

SDK **10.0.19041.0** carries `um\` and `ucrt\` libraries for `arm`, `arm64`, `x64` and `x86`. SDK
**26100 ships no `arm` at all**. The project previously set `WindowsTargetPlatformVersion` to 26100
unconditionally, which meant any future `Release|ARM` build would have failed to find
`um\arm\WindowsApp.lib` — a latent blocker found by reading, not by building. It is now
platform-conditional:

```xml
<WindowsTargetPlatformVersion Condition="'$(Platform)'=='ARM'">10.0.19041.0</WindowsTargetPlatformVersion>
<WindowsTargetPlatformVersion Condition="'$(Platform)'!='ARM'">10.0.26100.0</WindowsTargetPlatformVersion>
<WindowsTargetPlatformMinVersion>10.0.14393.0</WindowsTargetPlatformMinVersion>
```

Note that `arm32-uwp-env.ps1:12` already used `10.0.19041.0` and carried a comment saying 26100
lacks ARM32 libs — the clang side had it right; only MSBuild disagreed. `setenv.ps1:23` still
defaults `APOTHEOSIS_SDK_VER` to the x64 value (`10.0.26100.0`); `arm32-uwp-env.ps1` does not call
`vcvarsall` and assembles `INCLUDE`/`LIB` by hand, so it remains authoritative for the ARM build.

**Validation of §4+§5 against rule 1:** a full `Release|x64` msbuild after these edits compiled,
linked `Harness.exe` and produced `Harness_0.1.8.51_x64.appx` (53 676 896 B) with 0 errors. The
ARM-enabling changes are a demonstrated no-op for x64.

## 6. Coexistence — the collision points and the rule for each

These are the places where the two builds touch the same file or the same machine state. Each one is
a way for ARM32 work to break the x64 debugger, so each has a rule.

| # | Shared thing | Rule |
|---|---|---|
| 1 | `Src\port\harness-cmd.bat` — a single-arch compiler-command capture, currently **ARM32** | The active x64 GPU scripts (`compile-driver-gpu-x64.ps1`, `link-driver-gpu-x64.ps1`) do **not** read it (0 references), so re-capturing it for ARM is safe today. If the legacy x64 scripts (`compile-driver-x64.ps1`, `link-driver-x64.ps1`, `recompile-stubs-x64.ps1`, `hx.ps1`) are ever needed, **split it per arch** instead of flipping it back and forth |
| 2 | `Src\harness\Package.appxmanifest` | Shared on purpose → the two packages carry the **same version by construction**. MSBuild overrides `ProcessorArchitecture` per platform. Bump once; the deploy script's `-Ver` must match |
| 3 | `Src\setenv.ps1` `APOTHEOSIS_*` | Arch-switched by `APOTHEOSIS_ARCH`. `APOTHEOSIS_SDK_VER` defaults to the x64 value — for the ARM build, `arm32-uwp-env.ps1` is authoritative (§5) |
| 4 | `WebCoreDriver.h` in **two copies** (`Src\port`, `Src\harness`) | Any ABI change must land in both, or the two builds disagree silently |
| 5 | Build trees and object suffixes | `build-<arch>-<variant>`; `.x64.obj` vs `.arm32.obj` / `.arm32-jit.obj` / `.arm32-soft.obj`. No object can collide across arches |
| 6 | VS Installer ARM component | Versioned only (§4) |
| 7 | vcpkg | Never install the `icu` port (ICU 78 vs the engine's 75); never install `harfbuzz`/`fontconfig` via vcpkg (meson cannot cross-compile to UWP here) |
| 8 | `*-arm32.ps1` scripts | They route through `arm32-uwp-env.ps1` and **hard-fail on this x64 box by design**. Never invoke them expecting success here; x64 work uses the `*-x64.ps1` variants exclusively |
| 9 | Upstream WebCore sources | Every change stays behind `#if defined(WK_WINUWP)` + an `Apotheosis:` comment, so both arches inherit fixes (the CJK sentinel fix and `WebCoreGpuResize` reached ARM this way) |

Rebuilding the ARM32 engine does **not** touch `build-x64-gpu`: separate trees, separate objects,
separate driver libs, separate ICU/vcpkg prefixes. The only genuinely shared, mutable state is the
list above — and after the §4/§5 edits, none of it requires flipping a global setting back and forth
to switch which build is being worked on.

## 7. Optional: cap the x64 GPU path at Feature Level 9_3

The GPU row in §3 sits at ~70% for one reason: the dev box negotiates D3D11 FL11+ while the Lumia is
FL9_3. FL9_3 restricts texture sizes, non-power-of-two behaviour, instancing and shader model, so a
composited page that works on x64 can still fail on the device for reasons unrelated to ARM.

Because both builds link the *same* 2016 Windows-Store ANGLE, this gap is closable in software: cap
the requested feature level in the ANGLE display initialisation path
(`PlatformDisplayWin.cpp:44-51`) behind an opt-in flag, so the x64 build can be made to negotiate
FL9_3 on demand. Estimated effect: GPU-class fidelity from ~70% to ~85%.

Cost: one `ninja -C build-x64-gpu WebCore -j1` (hours, single-threaded by necessity) plus a driver
relink. Because it is a real cost and an opt-in behaviour change, **this is a decision to take
deliberately, not a step to apply silently.** Recommended shape: default off, enabled by an
environment variable or a driver-init flag, so the same binary can test both levels.

## 8. The x64 ladder — what to clear before touching the phone

The device ladder in `ARM32-BUILD-GUIDE.md` §8 stays as it is. This is the *pre-flight* ladder: every
rung is runnable on the dev box, and a failure here would otherwise have been discovered on the
phone at 100× the cost.

| Rung | What it proves | How |
|---|---|---|
| 0 | Package installs, app starts, no static-init crash | `MinimalTest=true` build (no engine libs, `MainPage.minimal.cpp`) |
| 1 | Engine boots, `WebCoreGpuInit` succeeds | launch, read `LocalState\log.txt` |
| 2 | Static paint | `WebCoreRenderHtml` / `about:home` |
| 3 | Real network + TLS in an AppContainer | `https://example.com` — expect `vua=1 sheets=1 pending=0` |
| 4 | Subresources, scripts, compositing scroll | Hacker News |
| 5 | Heavy real-world pages, CJK, Cyrillic | ya.ru, hh.ru, dzen.ru |
| 6 | Real input: tap, scroll, pinch, keyboard | interactive session |
| 7 | **Viewport changes** — resize on the dev box, rotation on the phone | resize the window while a heavy page is loaded (see §9.1: this rung is currently red) |
| 8 | Offscreen batch reproducibility | `LocalState\autodiag.txt` → `shot_N.bmp` + `autodump.txt` |

Two harness hooks make rungs 3–8 scriptable without a debugger: `LocalState\testurl.txt` (visible
navigation) and `LocalState\autodiag.txt` (offscreen batch: `WebCoreGpuInit(nullptr,…)` +
`WebCoreSessionLoad` + `WebCoreGetDiag` + `WebCoreGpuLayerInfo` + BMP dumps). `autodiag.txt` takes
precedence over `testurl.txt`. Both write to `LocalState`, which is readable from the dev box at
`%LOCALAPPDATA%\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState`.

Note that rung 7 is exactly the rung that the *phone* exercises involuntarily — a Lumia rotates —
while on the dev box it takes a deliberate window resize to hit. That asymmetry is why §9.1 went
unnoticed for so long.

## 9. Defects this method already caught (first post-reinstall session, 2026-08-16)

The x64 build earned its keep on the first run: five defects, of which two are device-critical and
neither would have been pleasant to diagnose on the phone.

### 9.1 Resize wedges the engine thread — every later job queues behind it (device-critical)

**Reported symptom:** typing `ya.ru` into the address bar showed `Loading https://ya.ru`, then
`Load timed out` about 40 s later; navigation never happened. The same URL had loaded correctly
minutes earlier through `testurl.txt`/`autodiag.txt`.

**Evidence** — `LocalState\log.txt`, chronological, with the successful resize shown for contrast:

```
20:37:55.276  ApplyViewportSize: 1024x694 session=1 gpu=1     <- posted
20:38:01.188  ApplyViewportSize: engine rc=0 for 1024x694     <- returned after 5.9 s
20:43:18.119  ApplyViewportSize: 1368x758 session=1 gpu=1     <- posted; NEVER returns
20:43:58.134  WATCHDOG: dumping diag: ... contents=1024x5176  <- stale, pre-resize geometry
20:44:13.288  NavigateTo: url=https://ya.ru pushHistory=1 loading=0
                                                              <- no "WE-job:body-start" follows
20:45:19.343  OnPageTapped: ... session=0 links=5
20:45:22.846  NavigateTo: url=https://ya.ru pushHistory=1 loading=0
                                                              <- again no "WE-job:body-start"
20:46:02.892  WATCHDOG: dumping diag: ... (identical stale text; log ends, app closed)
```

**Mechanism.** `WebEngine::post` (`MainPage.xaml.cpp:454-461`) is a mutex + `std::deque` + condvar
FIFO drained by **one** engine thread; it never drops a job. `WriteStage("WE-job:body-start")` is the
*first* statement of the navigation job body (`:1361`). Its absence therefore proves the job never
started, which proves the engine thread was still executing the previous job — the 1368×758 resize
posted at 20:43:18 (`:1636`), whose body calls `WebCoreGpuResize` (`:1639`). That call
(`WebCoreDriver.cpp:2221-2264`) destroys the `TextureMapper` and `GLContext`, calls
`GLContext::create(display, nativeWindow)` — the ANGLE call that marshals surface creation to the
panel dispatcher — and finishes with `finishInteractionPaint`. It had not returned ~2.7 minutes
later when the app was closed.

So **`Load timed out` is a misleading message**: nothing timed out on the network, and navigation was
never dispatched. The 40 s UI watchdog simply relabelled a wedged engine thread as a load failure.

**Why it matters far more on the phone:** a Lumia changes `ContentArea` size on **rotation**, which
enters the same `ApplyViewportSize` → `WebCoreGpuResize` path. On the device this is a rotation hang
on any heavy page, not an edge case reachable only by dragging a window border.

**Not yet proven:** the exact stall point inside `WebCoreGpuResize`. Two candidates, indistinguishable
from this log because both precede `writeDiag`: (a) ANGLE surface destroy/create marshalling to the
panel dispatcher — the documented mutual-wait deadlock hazard; (b) relayout plus full backing-store
regeneration of a live 1024×5176 SPA at a new width with `g_gpuScrollFast=false`. The 5.9 s taken by
the *successful* 1024×694 resize is uncomfortably long and consistent with either. **Next step:**
stage markers around the destroy / create / paint steps inside `WebCoreGpuResize`, then repeat the
resize on hh.ru. That is a driver-only change — `link-driver-gpu-x64.ps1`, no engine rebuild.

**Two secondary defects in the same path**, both in `OnLoadWatchdog` (`MainPage.xaml.cpp:1507-1523`):

- it calls `WebCoreGetDiag` **from the UI thread** while the engine thread is mid-call, violating the
  single-engine-thread rule (it returned the last-written diag text, which is why the dumps looked
  frozen at `contents=1024x5176`);
- `++m_opSeq` (`:1515`) invalidates the in-flight resize's completion callback, so even a late return
  could never log `engine rc=` nor update `kW`/`kH`; and `m_sessionActive = false` (`:1517`) is why
  the later tap logged `session=0`. The watchdog masks the very evidence it exists to collect.

### 9.2 Invisible text on ya.ru — the webfont stub advertises support it does not have

`FontCustomPlatformData::supportsFormat()` (`Src\port\stubs-other.cpp:244-271`) returns **true** for
`truetype`, `opentype`, `woff` and `svg`, while custom-font creation in the same file is deliberately
stubbed out — `AddFontMemResourceEx` is desktop-only and unavailable in the AppContainer, and
`fontPlatformData()` is `RELEASE_ASSERT_NOT_REACHED()`. WebCore therefore *commits* to the
`@font-face` source, receives nullptr, and paints glyphless text instead of falling back to a system
family. Confirming context: upstream `FontCustomPlatformDataWin.cpp:59-72` returns nullptr when
`renameAndActivateFont` fails, `build-x64-gpu\cmakeconfig.h:169` has `USE_WOFF2 0` (so woff2 sources
can never work regardless), and `Src\harness\gdi-stubs.cpp` contains no `AddFontMemResourceEx` at
all. ya.ru's layout was otherwise correct — images, SVG, opacity and transform layers all present.

- **Cheap fix:** make `supportsFormat` return `false` so WebCore falls back to the next CSS family.
  Stub recompile + `link-driver-gpu-x64.ps1`; no engine rebuild.
- **Proper fix:** implement custom fonts via FreeType (`FT_New_Memory_Face` +
  `cairo_ft_font_face_create_for_ft_face`) — freetype is already linked and packaged on both arches.
- **Cross-arch caveat (§2):** fontconfig exists only on ARM32, so the *fallback* family lookup after
  this fix takes a different path on the phone. Verify on both, do not extrapolate from x64.

### 9.3 `kW`/`kH` are mutated globals — a latent heap overflow on the device

`static int kW = 720, kH = 1080;` (`MainPage.xaml.cpp:620`) is mutated from the resize paths
(`:1592-1593`, `:1667-1668`), while the autodiag lambda allocates its buffer **once** as
`(size_t)kW * kH * 4` (`:1189`) and then passes the *live globals* to
`WebCoreSessionLoad(url, kW, kH, rgba.data())` (`:1199`). The visible artifact was harmless — every
`shot_*.bmp` came out 1024×694 although URL 1's diag reported `contents=720x1080`. The device
consequence is not harmless: at 1440×2560 a buffer allocated before a viewport growth is overflowed
by up to ~4.7×, in the on-device autodiag loop, with no debugger attached. Fix: snapshot the
dimensions once alongside the buffer and pass the snapshot.

### 9.4 Version strings frozen at v0.1.8.25

Three hardcoded `L"Apotheosis v0.1.8.25"` strings (`MainPage.xaml.cpp:929, 939, 1000`) are what the
UI shows while the manifest is at 0.1.8.51 — this is the mismatch seen on screen during the session.
Cosmetic, but it makes screenshots useless as build evidence, which matters for a project whose only
device feedback channel *is* screenshots.

### 9.5 Two latent ARM blockers found by reading, not by running

Neither would have surfaced until the first `Release|ARM` build, and both are now fixed: the
unconditional SDK 26100 (§5) and ICU **78** DLLs listed in the ARM deployment group while the ARM
import libs bind `icuuc75.dll` — the documented ICU version trap, which would have failed at load
with no useful message.

### 9.6 Positive baseline (worth recording, since it is the reference for regressions)

Offscreen autodiag: `WebCoreGpuInit rc=0`, all five `WebCoreSessionLoad rc=0`. example.com over both
HTTP and HTTPS rendered pixel-perfect, which also proves the packaged `cacert.pem` TLS path works in
the AppContainer. Hacker News fully correct including compositing-driven scroll (`max=0,628`,
`scrollPos=0,300`, `frame scrolled contents (position 0.00 -300.00)`, scrollbar layer present).
hh.ru rendered correctly in the interactive session (logo, Cyrillic tabs, photo); its blank offscreen
BMP was a **snapshot-timing artifact** — `WebCoreSessionLoad` returns before SPA hydration — not an
engine failure. Visible pass: 4/4 sites navigated, no crashes, working set 204–342 MB.

## 10. Open items

1. **`deps-build\` vs vcpkg contradiction for the ARM graphics stack.** `ARM32-RECOVERY.md` §5 says
   cairo/fontconfig/harfbuzz for ARM are source-built into `deps-build\`, but
   `Harness.vcxproj:150-168` packages them from `C:\vcpkg\installed\arm-uwp\bin`, and `deps-build\`
   currently contains only `harfbuzz-x64`. One of the two is stale. Resolve **before** the first ARM
   harness link, or the package step will fail on missing files.
2. **`MinimalTest` — keep the mechanism, fix the documentation.** It is the crash-isolation bisect
   tool (drops all engine libs, swaps in `MainPage.minimal.cpp`, keeps the engine DLLs so a DLL
   static-init crash can still be isolated) and the cheapest possible ARM32 rung-0 smoke test:
   it needs no ICU, no driver lib and no cairo, so it can validate `Release|ARM` packaging and
   startup *before* the ICU blocker is solved. Do not delete it. But `CLAUDE.md`'s documented harness
   build command passes `/p:MinimalTest=true`, which produces a build with **no browser** — that line
   is a trap for anyone following the docs and should be corrected.
3. **FL9_3 cap** (§7) — decide deliberately; costs one WebCore rebuild.
4. **`lib\onecore\arm` substitution** (§1) — cheap experiment, potentially removes the "install the
   ARM component first" prerequisite for the *engine* build.
5. **Stage markers inside `WebCoreGpuResize`** (§9.1) — driver-only change, unblocks the rung-7 red.

## 11. The one remaining external blocker: ICU 75 for ARM32

Narrowed, but not eliminated. Of the four packaged ICU files, **one transfers for free**:
`icudt75l.dat` (30.7 MB) is architecture-neutral — the `l` suffix means little-endian and ARM32 is
little-endian too — so it can be copied byte-for-byte from `C:\icu-x64-uwp\bin`. The other three are
code: `icuuc75.dll` (~1.8 MB), `icuin75.dll` (~3.0 MB) and the 36 KB `icudt75.dll` stub. There is no
shortcut around producing them: all three come out of one ICU 75 build for `thumbv7-uwp`, and ICU
ships no official ARM32 Windows binaries. What the narrowing buys is scope — ~5 MB of code to build
and verify instead of 36 MB, with data-related divergence excluded by construction.

Once the DLLs exist, the import libs follow the x64 procedure (`make-icu75-libs.ps1`, currently
hardcoded `/machine:X64` + `C:\icu-x64-uwp\lib`): `dumpbin /EXPORTS` → `.def` →
`lib.exe /machine:ARM /def:...` → `C:\icu-arm-uwp\lib\{icuuc,icuin,icudt}.lib`, with the DLLs and the
`.dat` in `C:\icu-arm-uwp\bin`.

**UNVERIFIED alternative worth one experiment.** The "never install the vcpkg `icu` port" rule exists
to protect the **already-built** `build-x64-gpu` tree, which imports `icuuc75`/`icuin75`. The ARM
engine, by contrast, is configured and compiled from scratch — so it only has to be *self-consistent*
with whatever ICU is on its `CMAKE_PREFIX_PATH`. That opens the possibility of obtaining ICU for
`arm-uwp` through vcpkg instead of by hand. Two caveats before spending a night on it: vcpkg's `icu`
port builds **78**, and 75 is the version this WebKit (2.52.4) is known-good against, so pin 75 via a
version override rather than accepting 78; and it is unproven that vcpkg can cross-build ICU for
`arm-uwp` at all on this machine (the meson-based ports could not). The test is unattended and
binary: it either produces `arm-uwp` ICU or it does not.

---

*Status: the x64 build is verified working after the ARM-enabling edits (§5), the coexistence rules
are in place (§6), and the ARM32 path is blocked on exactly one external dependency (§11) plus the
local prerequisites in `ARM32-RECOVERY.md` §1. Rung 7 of the pre-flight ladder (§8) is red — see
§9.1.*

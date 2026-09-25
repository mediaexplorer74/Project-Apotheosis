# The HarfBuzz and ICU divergence between the two build lines

Found on 2026-08-20 while catching the x64 engine up with the ARM32 fixes. It had been hiding for
months: the two lines shape text with **different engines** and normalise it with **ICU versions three
major releases apart**. The x64 line — the one used as the fast diagnostic mirror for ARM32 — was the
odd one out, which quietly undermines every rendering comparison made on it.

## How it surfaced

The x64 WebCore link failed:

```
lld-link: error: duplicate symbol: public: class WTF::RefPtr<class WebCore::Font const, ...>
  __cdecl WebCore::FontCascade::fontForCombiningCharacterSequence(class WTF::StringView) const
```

Two definitions of one function were being linked:

| Where | Guard |
|---|---|
| `platform/graphics/cairo/FontCairoHarfbuzzNG.cpp:96` | none |
| `platform/graphics/FontCascade.cpp:1763` | `#if !PLATFORM(COCOA) && !USE(HARFBUZZ)` |

So they collide in exactly one configuration: **Cairo on, HarfBuzz off**. That is the x64 configuration,
and nothing else in the tree is built that way.

## The causal chain, end to end

Each link was verified, not assumed.

1. **`USE_HARFBUZZ` is set only when HarfBuzz is found.** `Source/cmake/OptionsWinUWP.cmake:250`:
   ```cmake
   if (HarfBuzz_FOUND)
       SET_AND_EXPOSE_TO_BUILD(USE_HARFBUZZ ON)
   endif ()
   ```
   Measured: `build-arm32-gpu/cmakeconfig.h` has `#define USE_HARFBUZZ 1`; the x64 `cmakeconfig.h` has
   no such line at all.

2. **x64 asks for a component it cannot have.** `OptionsWinUWP.cmake:73-77` gates the lookup on the
   architecture:
   ```cmake
   if (CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM")
       find_package(HarfBuzz)
   else ()
       find_package(HarfBuzz COMPONENTS ICU)
   endif ()
   ```
   Note which branch gets the component: **ARM, which has `harfbuzz-icu.lib`, is asked without it, and
   x64, which does not have it, is asked with it.** The comment above those lines describes the
   intent correctly and the code does the opposite. The x64 cache shows the consequence:
   ```
   HarfBuzz_LIBRARY         = C:/vcpkg/installed/x64-uwp/lib/harfbuzz.lib   (found)
   HarfBuzz_ICU_LIBRARY     = NOTFOUND
   HarfBuzz_ICU_INCLUDE_DIR = NOTFOUND
   ```
   One missing component makes `HarfBuzz_FOUND` false, so step 1 never fires.

3. **x64's harfbuzz is built without ICU on purpose.** `Src/port/build-harfbuzz-x64.ps1:30` passes
   `-Dfreetype=disabled -Dicu=disabled`, against ARM's `-Dfreetype=enabled -Dicu=enabled` in
   `build-harfbuzz-arm32.ps1:23`. On disk:
   ```
   x64-uwp/lib:  harfbuzz.lib
   arm-uwp/lib:  harfbuzz.lib  harfbuzz-icu.lib  harfbuzz-raster.lib  harfbuzz-vector.lib
   ```

4. **And that was forced by where ICU comes from.** meson locates ICU through pkg-config:
   ```
   vcpkg arm-uwp/lib/pkgconfig:  icu-uc.pc  icu-i18n.pc  icu-io.pc
   C:\icu-x64-uwp:               no pkgconfig directory at all
   ```
   The x64 ICU is a hand-built tree with `icudt.lib`, `icuin.lib`, `icuuc.lib` and no `.pc` files, so
   `-Dicu=enabled` could not have worked there. This is the same root as task #11, "move x64 ICU into
   vcpkg".

5. **The second definition comes from this fork, not from upstream's source list.** `lld-link` named
   both sites, which is the only reason this was settled rather than argued:
   ```
   >>> defined at Src\port\stubs-font-uwp.cpp:53
   >>> defined at ...WebCore\DerivedSources\unified-sources\UnifiedSource-3c72abbe-27.cpp.obj
   ```
   The port's own `stubs-font-uwp.cpp` implements the function, and its comment states the precondition
   plainly: *"This port has USE_HARFBUZZ=ON … so provide the plain glyphDataForCharacter fallback
   here."* With HarfBuzz on that is correct — `FontCascade.cpp`'s branch is disabled and the HarfBuzzNG
   backend is not compiled, so without the stub the symbol would be missing. With HarfBuzz **off**, as
   on x64, `FontCascade.cpp`'s branch is compiled and the stub becomes a second definition.
   The stub is now guarded with `#if USE(HARFBUZZ)`, i.e. it declares the precondition it always
   assumed. When x64 gets HarfBuzz the guard becomes always-true and can be removed.

   *A false trail worth recording, because it cost an hour:* the first diagnosis blamed
   `platform/FreeType.cmake`, which adds `FontCairoHarfbuzzNG.cpp` under `if (USE_CAIRO)` rather than
   `if (USE_HARFBUZZ)`. That reasoning is sound in the abstract and wrong here — `FreeType.cmake` is
   included only by the GTK, WPE and PlayStation ports, not by `PlatformWinUWP.cmake`, so this fork
   never reads it. The giveaway was mechanical and should have been checked first: after editing that
   file, `build.ninja` was still older than the edit, meaning CMake had not re-read it. An edit to a
   file the build does not include cannot change anything.

6. **Why it linked before at all.** Until this rebuild the x64 objects were older than the change that
   brought `FontCairoHarfbuzzNG.cpp` into the compiled set, so only one definition existed in the
   objects being linked. Nothing was fixed in between; the collision simply had not been assembled yet.

## The ICU half, which is separate and larger

| | ICU | Data delivery |
|---|---|---|
| x64 | **75.1**, `C:\icu-x64-uwp` (`icuuc75.dll`) | `icudt75l.dat` packaged and injected |
| ARM32 | **78.3**, vcpkg `arm-uwp` | no `.dat` packaged, the data DLL is used |

The harness already hides this: it tries `icuuc75.dll`, then `icuuc78.dll`, then `icuuc.dll`. Useful
robustness, but it also meant nobody had to notice the split. ICU governs normalisation, locale data
and line breaking, so this affects more than shaping.

## The full list of ways the two lines differ, as of 2026-08-21

Kept here because every one of them has, at some point, made a measurement on one line fail to transfer
to the other. The first five have something in common — none is in the application's own code, so UWP's
promise that a build is identical across targets held for the API surface and broke everywhere
underneath it. Items 6 and 7, added the same evening, break even that consolation: they are ours, and
they are in the *tooling*, which turned out to be the more expensive kind.

1. **Text shaping.** ARM32 has HarfBuzz with the ICU component; x64 has `USE_HARFBUZZ` off. The cause
   chain is in this document above.
2. **ICU version.** ARM32 gets 78.3 from vcpkg, x64 has 75.1 in `C:\icu-x64-uwp`. Three major versions
   apart, and it also decides whether pkg-config files exist, which is what made item 1 happen.
3. **JIT — CLOSED 2026-09-18, and the stated shape of it was wrong.** The probe does return 42 through
   both RW→RX and RW→RWX on the Lumia while all three paths fail in the x64 AppContainer, and that part
   stands. What does not stand is the conclusion drawn from it — that "x64 runs the LLInt interpreter"
   while the device JITs. **Both lines build `ENABLE_JIT=ON`, `ENABLE_DFG_JIT=OFF`, `ENABLE_FTL_JIT=OFF`,
   `ENABLE_C_LOOP=OFF`**, i.e. LLInt + baseline JIT on both, measured in both `CMakeCache.txt` files. The
   real divergence was a *runtime switch*, not a build configuration: the packaged
   `Src\harness\Assets\navseq.txt` carried `jit=0`, which the harness turned into `JSC_useJIT=false`, so
   **every bench session ran the interpreter and no bench session had ever run the JIT.** That line has
   been removed (`navseq.txt` now ships `enabled=0`, JIT on, autoplay silent) and `0.1.9.107` was
   exercised with it on; the dzen.ru freeze reproduced identically either way, so this axis no longer
   explains anything. Details: `Doc/DZEN-SCROLL-DEATH.md` §10b-§10d.
4. **Windows SDK contents per architecture.** 26100 ships no `um\arm` at all, and `volatileaccessu.lib`
   exists for arm64/x64/x86 and for no ARM32 SDK anywhere. Which desktop APIs leak into the APP
   partition also differs by SDK version: `CertOpenSystemStoreW` is reachable from 26100's x64
   `WindowsApp.lib` and absent from 19041's ARM one, and 19041 keeps `LoadLibrary` and
   `GetModuleHandle` out of the partition entirely while 26100 does not.
5. **Debug information in the port objects.** `compile-driver-gpu-x64.ps1` lifts its flags straight out
   of `build-x64-gpu\build.ninja`, and those carry `/Zi` alongside `/O2`, so every object embeds the type
   tables of everything it saw through WebCore's headers. The ARM script composes its own flags and asks
   for none. The effect is dramatic and easy to mistake for a build error:

   | | largest object | all 12 objects | archive |
   |---|---|---|---|
   | x64 | `WebCoreDriver.x64.obj` 13,172,179 B | 60,262,989 B | 47,415,582 B |
   | ARM32 | `WebCoreDriver.arm32.obj` 319,322 B | 776,225 B | 1,042,488 B |

   Same source, 41 times the size. It does not reach the appx — the linker takes only the code it needs,
   and the packages differ by five megabytes, for other reasons — but it does explain why an x64 relink
   takes noticeably longer, and it is why symbolising an x64 stack works at all.

6. **The input path — the bench is driven by scripts, the phone by fingers.** `x64-cycle.ps1 -Url`, the
   `nav.txt` watcher and the packaged `navseq.txt` player each call `NavigateTo` exactly once. A real tap
   on the address bar's action button does not: on the device it produced a Click **and** an Enter
   `KeyDown` 28 ms apart, so every navigation loaded the page twice, and the second load was what killed
   the process. The bench could not reproduce it because it never generated the gesture. Fixed at the
   source (`OnUrlKeyDown` now sets `e->Handled`) with a duplicate guard in `NavigateTo` behind it.
   **This one is closable rather than permanent:** the dev machine is a Surface with a touchscreen, so
   touch gestures *can* be exercised there. Nobody had, which is the whole lesson — a scripted
   destination is not a scripted gesture.
7. **GPU default.** `x64-cycle.ps1` wrote `gpudefault=0` into `settings.ini` on every run, so every
   "the bench survives this" result had been obtained with compositing switched **off**, while the phone
   runs with it on and calls `EnableGpu` after the first successful load. The bench was not running the
   code being blamed. A `-Gpu` switch now seeds `gpudefault=1`; the default stays 0 because the software
   path is the project's baseline.

   With that switch the bench was finally put in the phone's configuration — GPU on, navigation by hand
   — and reproduced the sequence exactly: `WebCoreGpuInit` returned 0, the first frame came back
   `EnableCompositing=0 Composite=-12`, the harness stayed on software present. **It survived**, reaching
   `rs=C` with `pending=1`. The phone at the same point sits at `rs=I` with `pending=10` and nine to
   twelve requests still in flight. So the remaining difference is not the GPU: it is that the device is
   slow enough for the pump's quiet counter to give up while a third of the page is still loading, and
   the bench is not.

8. **RETRACTED, 2026-09-03 — "the driver has no GPU path on x64".** Listed here for a few hours as a
   sixth divergence and then withdrawn, because it was an artefact of stale comments rather than a
   measurement. `Src/port/WebCoreDriver.cpp` carried a line in `paintToRGBA` asserting
   "x64-gpu: USE(TEXTURE_MAPPER)=0 ? GPU code path excluded; always falls through to Cairo", and
   `OptionsWinUWP.cmake` still says the x64 line has no ICU component for HarfBuzz. Both are false:
   `build-x64-gpu/cmakeconfig.h` has `USE_TEXTURE_MAPPER 1`, `WebCoreDriver.x64.obj` contains the
   `gpuPresent` / `gpuCompositeReadback` symbols, and on a page that forces a compositing layer the x64
   build reports `EnableCompositing=1 Composite=0` and fills the buffer through the readback path.
   **Both architectures compile and take the GPU branch.** The stale comment in `paintToRGBA` has been
   deleted; the CMake gate is retained deliberately (see the comment there — editing that file forces a
   reconfigure the ARM build directory cannot survive), but its comment now describes what it really
   does. Recorded as a retraction rather than removed, because "a comment mistaken for a measurement"
   is the recurring failure mode in this document and worth one more entry.

## What was done on 2026-08-21, and what deliberately was not
**Done — the duplicate is gone and the link succeeds.** `Src/port/stubs-font-uwp.cpp` now guards its
`FontCascade::fontForCombiningCharacterSequence` with `#if USE(HARFBUZZ)`, so it provides the symbol
exactly when upstream does not. Verified: `WebCore.dll` linked at 08:15:50 on 2026-08-21, 46,218,240
bytes, zero errors, in two ninja tasks — recompile the stub, link the library.

**Not done, on purpose — enabling HarfBuzz on x64.** That is the real fix and the maintainer agreed to
it, but it changes how text is shaped, and a change you can only judge by looking at rendered text
must not be made unattended overnight with no baseline to compare against. The order matters: get a
working x64 engine first, then change the font stack, then compare the same page before and after.

**Also relevant to how much this is worth doing:** the two lines already diverge on something with a
bigger performance effect. The JIT probe returns 42 through both the RW→RX and RW→RWX paths on the
Lumia, so executable memory works there, while on x64 in the AppContainer all three paths fail with
execution trapped by DEP/ACG — x64 runs the LLInt interpreter. The device is the faster of the two in
that respect, so x64 is a mirror for correctness only and every speed claim has to be measured on
hardware. Recorded here because it is the same category of trap: an x64 measurement that does not
transfer to the device.


## The proper fix, in two stages

**Stage 1 — give x64 real HarfBuzz.** Enough to make both lines shape text the same way, which is what
matters for comparing rendering:

1. Write `icu-uc.pc`, `icu-i18n.pc`, `icu-io.pc` into `C:\icu-x64-uwp\lib\pkgconfig`, modelled on the
   vcpkg `arm-uwp` ones. They are plain text; the ARM copies are the template, with `Version: 75.1`,
   `-licuuc` / `-licuin` / `-licudt` and `baselibs = WindowsApp.lib`.
2. Change `Src/port/build-harfbuzz-x64.ps1` to `-Dfreetype=enabled -Dicu=enabled` and rebuild. freetype
   is already present in vcpkg `x64-uwp`.
3. Reconfigure `build-x64-gpu` so `find_package(HarfBuzz COMPONENTS ICU)` succeeds and `USE_HARFBUZZ`
   turns ON. The gate added in step "Done" then stops mattering, which is the point — it is a guard, not
   a preference.
4. Rebuild WebCore and compare a text-heavy page against the pre-change screenshot.

While there, fix the inverted arch gate in `OptionsWinUWP.cmake:73-77` rather than leaving a comment
that contradicts its code. Once both lines have `harfbuzz-icu`, the gate can go away entirely.

**Stage 2 — align ICU (task #11).** Move x64 from the hand-built `C:\icu-x64-uwp` 75.1 to vcpkg
`icu:x64-uwp`, which brings 78.x plus the `.pc` files that stage 1 has to hand-write. Then both lines
share one ICU, the version-guessing in the harness (`icuuc75` / `icuuc78` / `icuuc`) can collapse to one
name, and `C:\icu-x64-uwp` leaves the system drive root.

## Why this matters beyond one link error

The x64 line exists to make ARM32 debuggable at a few minutes per iteration instead of twenty. That
argument only holds where the two lines agree. Text shaping and Unicode handling are precisely where the
current open defects live — empty frames, missing glyphs, layout that is correct while nothing is
painted — so a divergence here is not cosmetic: it can make an x64 experiment prove something that is
not true of the device. Closing it is what keeps the mirror honest.


## Closed on 2026-08-22: both lines now share the text stack

Divergences 1 and 2 are gone. The x64 line was moved from the hand-built `C:\icu-x64-uwp` ICU 75.1 to
`icu:x64-uwp` from vcpkg (78.3, the same package the ARM line uses), harfbuzz was rebuilt there with
`-Dfreetype=enabled -Dicu=enabled`, and `USE_HARFBUZZ` is now 1 on both. Verified in
`build-x64-gpu`: `cmakeconfig.h` carries `#define USE_HARFBUZZ 1`, and every ICU and HarfBuzz cache
entry resolves under `C:/vcpkg/installed/x64-uwp`. WebCore linked at 05:15, 46,211,584 bytes, in a
940-task build with zero failures; the app then started and reported `ICU: loaded icuuc78.dll` where
it used to load 75.

The stage order in the section above was swapped, and doing ICU first was the cheaper route: stage 1
step 1 was "hand-write `icu-uc.pc`, `icu-i18n.pc`, `icu-io.pc`", and vcpkg ships all three, so that
step disappeared instead of being done.

The inverted architecture gate in `OptionsWinUWP.cmake` is gone rather than corrected, because its
premise is gone: both lines can now be asked for `HarfBuzz COMPONENTS ICU`.

### Four things that had to be fixed to get there, none of them ICU

Recorded because each was a latent gap that only a from-scratch configure could expose, and together
they cost more than the ICU switch itself. Full account in project memory
(`from-scratch-configure-was-never-tested`).

1. **The vcpkg triplet.** `x64-uwp.cmake` still carried the `VCPKG_VISUAL_STUDIO_PATH` +
   `VCPKG_PLATFORM_TOOLSET v143` pair that `arm-uwp.cmake` had removed on 2026-08-17 with an explicit
   "do not add them back", and it lacked `VCPKG_CMAKE_SYSTEM_NAME WindowsStore`. Without the latter
   vcpkg never sets `VCPKG_TARGET_IS_UWP`, so icu's portfile skipped its whole UWP branch and the
   build died on `putil.cpp(635): error C3861: '_tzset': identifier not found`.
2. **Ruby.** A from-scratch configure fails with "Ruby 2.5 or higher is required". Ruby 3.4.10 was
   installed and simply not on `PATH`; every previous reconfigure had reused a cached
   `Ruby_EXECUTABLE`. `Src\setenv.ps1` now exports it.
3. **`sqlite3.h`.** `SQLite3_INCLUDE_DIR` had been `C:/icu-x64-uwp/include` — the header lived inside
   the *ICU* tree and was found there by accident. It is the only copy on the machine. Now installed
   into the vcpkg prefix, the way `build-sqlite-arm32.ps1` already does it for ARM. **This is why
   `C:\icu-x64-uwp` cannot simply be deleted yet:** the CMake Cairo fallback branch still names it too.
4. **`harfbuzz-icu.dll`.** With `USE_HARFBUZZ` on, `WebCore.dll` gained an import of
   `harfbuzz-icu.dll`, which vcpkg builds as a separate shared library. It was not in the x64 package,
   so the app died in the loader before `main`: LocalState came back holding only `settings.ini`, not
   even `stage-1-main.txt`. That empty-LocalState signature always means a missing dependency. The ARM
   ItemGroup had packaged it all along; the x64 one now does too.

### What the font comparison established, and what it did not

For every font size present in both the before and after `ya.ru` runs — 10 of 12 — the `platformInit`
metrics are identical. Nothing about text measurement moved.

It does **not** establish that shaping is unchanged, and HarfBuzz is a shaper. The before-side baseline
came from a live `ya.ru` session with scrolling rather than deterministic content, and the engine that
produced it is overwritten, so that comparison cannot now be made. A deterministic reference for the
*next* change is recorded in `Doc/font-baseline-2026-08-21/`, along with the reason to take baselines
from reproducible content in the first place.

Still open from the original list: item 4 (SDK contents per architecture), 5 (`/Zi` in x64 port
objects), 6 and 7 as noted above. Item 3 is closed (see the rewritten entry above) — and note it closed
as *not a divergence at all*: the two lines build the same JIT tiers, and the difference that did exist
was a runtime switch in the packaged `navseq.txt` rather than a property of either build.

---

## Postscript, 2026-09-03: the x64 line was rebuilt, and what that revealed

Builds 0.1.9.41 through .77 were made on the **ARM line only** and never compiled for x64 — 37
consecutive builds. On 2026-09-03 the x64 line was brought up end to end: engine 376/376 clean, driver
relinked (45.39 MB), appx built, installed and run. Three things had rotted in the meantime, and all
three are the same failure: **a file that has not been compiled for an architecture is not a file that
compiles for it.**

1. **`ApoUnwindStack` used ARM `CONTEXT` members** — `Pc`, `Lr` — with no architecture split, so the
   harness did not compile for x64 at all (`error C2039: 'Pc': is not a member of '_CONTEXT'`). Now
   behind `APO_CTX_PC` / `APO_CTX_SP` / `APO_HAS_LR`; x64 uses `Rip`/`Rsp` and has no LR fallback.
2. **The register dump** had the same problem, plus 20-odd `R0..R12` reads. Now two branches, with the
   x64 side printing the argument registers that carry a park address under the Windows x64 convention.
3. **`VerifyXamlConnect` was hooked `AfterTargets="MarkupCompilePass1"`** although it reads
   `MainPage.g.hpp`, which **Pass2** writes. After a Clean removed that file the guard blocked the very
   build that would have regenerated it. Now `AfterTargets="MarkupCompilePass2"`.

### The wedge dumper was lying on x64, and it had been believed

Worth recording in full, because a whole diagnosis was built on its output and had to be retracted.

`ApoBuildImageList` walked the address space up to `0x80000000` — correct for a 32-bit user space,
useless on x64, where the DLLs sit near `0x00007FFD_xxxxxxxx`. The walk therefore ended before reaching
a single module, `g_apoImageCount` stayed 0, and every frame fell through to a fallback that guessed the
module by testing `addr - base < 0x4000000` (64 MB). WebCore's x64 image is `0x2CB2000` (44.7 MB), so
19 MB of whatever was mapped after it got reported as WebCore. Worse, every address was printed as
`(unsigned)`, dropping the high word, so `WebCore` at `0x7FFDE85F0000` and `JavaScriptCore` at
`0x7FFDEB2B0000` became indistinguishable.

The result was a confident, symbolised, entirely wrong stack: frames in JavaScriptCore read as
`WebCore+3900163` and symbolised against WebCore.dll as `MicrotaskQueue::performMicrotaskCheckpoint`.
The arithmetic is what exposed it — an offset larger than the image cannot be inside the image.
Re-attributed against the real bases from `log.txt`, those frames were `JSC::VM::updateStackLimits` and
`JSC::JSLock::willReleaseLock`, i.e. leaving the VM lock, not spinning in microtasks. A JS watchdog —
the fix that idea would have suggested — could not have helped: it interrupts script *execution*, and
the thread was past that.

Fixed on 2026-09-03: the walk ceiling is now `0x7FFFFFFF0000` on 64-bit; a single `ApoFormatAddr`
prints `%p` and bounds the fallback by the module's real `SizeOfImage` read from its PE header; and
`CopyStackWords` takes `uintptr_t` slots instead of `uint32_t`, so an x64 return address is one entry
rather than two halves matching nothing. The first dump from the fixed tool resolved cleanly to
`WebEngine::loop → the navigation job → condition_variable::wait_until`, which is the 2.5 s background
fetch wait, not a hang at all.

### Divergence 6 is closed, 7 stands corrected

**GPU path on x64 (was: "excluded").** A comment in `paintToRGBA` asserted *"x64-gpu:
USE(TEXTURE_MAPPER)=0, GPU code path excluded; always falls through to Cairo"*. False:
`build-x64-gpu/cmakeconfig.h` has `USE_TEXTURE_MAPPER 1` and `WebCoreDriver.x64.obj` contains the
`gpuPresent`/`gpuCompositeReadback` symbols. Both architectures compile and take that branch. Comment
deleted; the misreading it caused cost a morning.

**Two paint defects found and fixed on the x64 line, both applying to ARM equally:**

- The driver took the direct-present branch whenever a native window had ever been passed to
  `WebCoreGpuInit`, with no knowledge of whether the host still showed the panel. When the harness
  decided to stay on software present it *collapsed* `GpuPanel`, and the driver went on presenting into
  that hidden surface and returning `kOK` with `outRGBA` untouched: a fully loaded page in a white
  window, no error anywhere. Fixed by a new export `WebCoreSetDirectPresent(int)` (added to **both**
  copies of the ABI header) that the harness sets in both branches of its decision.
- `gpuCompositeReadback` returned `kOK` unconditionally, so an empty composite handed back a white
  buffer *and suppressed the Cairo fallback*, since `paintToRGBA` treats `kOK` as "frame delivered".
  Now returns `kErrNoView` when `contentPx == 0` and does not publish the frame hash. `contentPx` is
  the right test rather than `nonWhite`: it counts pixels differing from the document's own background,
  so a legitimately white page is not misjudged as empty and a dark page is not misjudged as full.

**The chrome client is now installed unconditionally.** It used to be `if (g_gpuActive)`, and since the
ChromeClient is a `UniqueRef` fixed at `Page::create`, a session built before `WebCoreGpuInit` ran could
never composite — measured in `gpuinit-steps.txt`, where the first two session builds occupy lines 1-38
and `[GPU] enter` is line 39. `WebCoreComposite` then returned `kErrNoSession` (-12), the harness read
that as "GPU unusable" and latched onto software present for the whole process. Safe by construction,
not by hope: `RenderLayerCompositor::cacheAcceleratedCompositingFlags` reads
`settings->acceleratedCompositingEnabled()` **first** and only then asks
`client().allowedCompositingTriggers()` (RenderLayerCompositor.cpp:632-639), so with the setting false
no layer is ever requested. Software rendering remains the baseline; the setting, not the presence of
the client, is the runtime gate.

Verified on a page that forces a layer (`will-change: transform` + `translateZ(0)`):
`EnableCompositing=1 Composite=0`, `nonwhite=710656/710656`, no readback-failure marker. And the
per-URL GPU re-probe from .81 is confirmed working — two `EnableGpu` blocks in one process, where
previously the probe ran once and never again.

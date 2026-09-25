# Session handoff — 2026-09-03, x64 build line

Written to hand this work to a fresh session. Read `CLAUDE.md` first, then this file.

Project memory (per-fact files plus an index) lives at
`C:\Users\media\.claude\projects\C--Users-media-source-repos-Vibe-Apotheosis\memory\`. That
`MEMORY.md` is an **index only** — one line per memory, never content. This file is the session
handoff and is a different thing.

**Every claim below is marked MEASURED or NOT VERIFIED. Do not promote the second kind.** Three
diagnoses were retracted during this session because they rested on unverified readings; the list is
in section 6 so nobody resurrects them.


## Current status — Sep 18, 2026 (morning)

> Current evidence: [STATUS-2026-09-18.md](Doc/STATUS-2026-09-18.md).
> The original 2026-09-03 handoff following this section is historical, including its installed
> version, old GPU observations and "next steps"; those do not override the .97 evidence.

- x64 `0.1.9.97` Release, installed **in-place** (LocalState backed up); built from .96 sources
  + PE32/PE32+ parser fix in `ApoReadModuleName` (watchdog diagnostic).
- **Runtime verification (2026-09-18):** navseq completed all 8 URLs without crash.
  example.com ×2, Hacker News, layertest.html, ya.ru all rendered correctly. **Ya.ru rendered
  a full page with images** (78 scripts, nonwhite=234723). dzen.ru redirected to SSO login —
  site requires login, not a browser bug.
- **CSS crash `0xc0000005 @ WebCore.dll+0x204901f` did NOT reproduce** on .97 for any tested
  site. The .96 guard (stale `CalculationValue` handle check) remains a candidate fix;
  mechanism not proven.
- **GPU readback:** fail-closed guard verified (78 fallback messages, 0 readPixels). Cairo
  fallback renders correctly. Compositing visible on layertest.
- **Process stability:** PID 10648 survived ~40 minutes of active navigation. No crash events.
- **Memory/disk:** 4 GB RAM; ~38 GB free disk. Toolchain: ICU 78 vcpkg, SDK 19041 harness /
  26100 x64 engine, MSVC 14.44.
- ⚠️ `x64-cycle.ps1` removes the installed package — avoid for data-preserving updates.

---

---

## 1. Where the x64 line stands

MEASURED, all of it:

| Artefact | State |
|---|---|
| `build-x64-gpu/bin/WebCore.dll` | 46 215 680 bytes, 08-25 03:01, engine rebuild 376/376 clean |
| `build-x64-gpu/bin/JavaScriptCore.dll` | 18 778 112 bytes, 08-24 16:12 |
| `Src/port/WebCoreDriver-gpu-x64.lib` | 45.39 MB, relinked after every driver edit |
| `Package.appxmanifest` | `0.1.9.84` |
| Installed on bench | `EdgeHTMLReborn.Harness_0.1.9.84_x64__edmb40rfkwsbg` |

The ARM line was also fully rebuilt on 08-22: 1195/1195 clean, `build-arm32-gpu/bin/WebCore.dll`
34 860 544 bytes at 15:07. ARM is at `0.1.9.77` on the device and its silent-death defect is closed
(see `Doc/PUMPLOOP-SILENT-DEATH.md`).

**`Src/harness/Assets/navseq.txt` still carries `jit=0`.** That is a leftover diagnostic from the ARM
experiment that exonerated the JIT. It must go back to `jit=1` before any performance claim is made,
on either line. Also `enabled=1` there right now, which means the packaged sequence plays on startup
and will fight a human tester for the address bar.

---

## 2. Why 37 builds of drift mattered — three x64-only breakages

Builds .41 through .77 were ARM-only. Nothing in them was ever compiled for x64, and three defects
accumulated invisibly. All three are now fixed; all three are the same lesson, which is now a rule in
`CLAUDE.md`: **a file that has not been compiled for an architecture is not a file that compiles for
it.**

1. **`ApoUnwindStack` used ARM `CONTEXT` members** (`Pc`, `Lr`) with no architecture split, plus a
   `RtlLookupFunctionEntry` call whose argument width is different on x64. The x64 harness did not
   compile at all. Now behind `APO_CTX_PC` / `APO_CTX_SP` / `APO_HAS_LR` macros with `_M_ARM` and
   `_M_AMD64` branches and an `#error` for anything else.
2. **The register dump** had the same problem — `R0..R12` do not exist on x64. Now two branches; the
   x64 one leads with `RCX`/`RDX`/`R8`/`R9`, which are the argument registers that carry a park
   address under the Windows x64 convention.
3. **`VerifyXamlConnect` was hooked `AfterTargets="MarkupCompilePass1"`** but reads
   `Generated Files\MainPage.g.hpp`, which **Pass2** writes. After a Clean removed that file, the
   guard failed the very build that would have regenerated it. Now `AfterTargets="MarkupCompilePass2"`,
   `BeforeTargets="ClCompile"` kept as a belt.

Recovery performed for the missing `MainPage.g.hpp`: copied from `MainPage.g.hpp.backup` (17 Aug)
even though `MainPage.xaml` had changed on 24 Aug, because the verifier then reported
`xaml/Connect in sync: 61 fields, 88 case labels, 88 connectable elements, 61 handler attachments`.
The markup edits had not touched connectable structure. **Do not restore that backup without running
the verifier afterwards** — a stale `Connect()` binds `x:Name` fields to the wrong controls and a null
`^` handle in C++/CX is an access violation `catch (...)` cannot catch.

---

## 3. The white screen — two separate mechanisms, both closed

This was **not one defect**. Anyone looking for a single root cause will not find it.

### 3a. Frame presented into a hidden surface (MEASURED, fixed, confirmed)

The driver knew only that a native window had once been passed to `WebCoreGpuInit`
(`g_gpuPresentMode`). It therefore took the direct-present branch in `paintToRGBA` and returned `kOK`
**without filling `outRGBA`**. Meanwhile the harness had decided to stay on software present and
collapsed `GpuPanel`. So the page rendered and was presented into a surface nobody could see, while
the visible `WriteableBitmap` kept its zero-filled buffer: `nonwhite=0/710656` on a document at
`readyState Complete`, with no error logged anywhere.

Fix: new export `WebCoreSetDirectPresent(int)`, declared in **both** copies of `WebCoreDriver.h`
(`Src/port/` and `Src/harness/` — editing only one silently desynchronises the ABI). The driver's
`g_gpuDirectPresent` defaults to false, so software rendering stays the baseline. The harness calls it
in **both** branches of the present decision (`MainPage.xaml.cpp`, around 5352-5372).

Confirmed: `nonwhite` went from `0/710656` to `59430/710656`, and `gpuinit-steps.txt` shows
`[GPU] direct present -> 0`.

### 3b. Empty composite reported as success (MEASURED cause, fix NOT VERIFIED end to end)

`gpuCompositeReadback` returned `kOK` unconditionally. `paintToRGBA` treats `kOK` as "frame delivered,
stop here", so a TextureMapper pass that painted nothing handed back a white buffer **and suppressed
the Cairo fallback**. Sequence: first load paints through Cairo (GPU not yet up) and looks right; then
`WebCoreGpuInit` runs; the next session gets a root layer; the next live tick reads back an empty
composite whose hash differs from the last frame, so the harness accepts it as new pixels and blits
white over a good page.

Fix: return `kErrNoView` when `contentPx == 0`, and **do not** publish `g_lastFrameHash` in that case
(publishing the hash of a rejected frame makes the next tick think the page changed). `contentPx` is
the right test rather than `nonWhite`: it counts pixels differing from the document's own background,
so a legitimately white page is not judged empty and a dark page is not judged full.

**Verification status:** a one-off run of a hand-made layer page reported `Composite=0`,
`EnableCompositing=1`, `nonwhite=710656/710656` — the readback branch ran and filled the buffer. But
in the packaged 8-URL sequence the process died before the layer probe's `nonwhite` was logged, so the
fix is **not** confirmed under load. See section 5.

---

## 4. Composite went from -12 to -4 (MEASURED, fixed)

`WebCoreComposite` guards on `(!g_gpuActive || !g_session || !g_session->page || !g_session->chrome)`
and returns `kErrNoSession` (-12). The culprit was the last term: `buildSession` installed
`PortChromeClient` only under `if (g_gpuActive)`, and the harness builds its first sessions **before**
calling `WebCoreGpuInit` — measured in `gpuinit-steps.txt`, where the first session builds occupy lines
1-38 and `[GPU] enter` is line 39. The ChromeClient is a `UniqueRef` fixed at `Page::create`, so that
session could never composite, the first-frame probe read -12, and the harness latched onto software
present permanently, for every later session too.

Fix: install the client unconditionally. **Safe by construction, not by hope** —
`RenderLayerCompositor::cacheAcceleratedCompositingFlags` (`RenderLayerCompositor.cpp:632-639`) reads
`settings->acceleratedCompositingEnabled()` first and only asks `client().allowedCompositingTriggers()`
*inside* that branch. With the setting false no layer is ever requested and `PortChromeClient` behaves
as `EmptyChromeClient`. The setting, not the presence of the client, is the runtime gate.

This deliberately does **not** enable compositing unconditionally. That was tried once and cost a
silent `__fastfail` on the device with no dump (`CLAUDE.md`).

Result: `Composite` now reports -4 (`kErrNoView`, "no root layer") on pages without a layer, which is
the honest answer, and 0 on the layer probe.

---

## 5. THE OPEN DEFECT on x64 — narrowed to one call (see 5a; full write-up in `Doc/X64-GPU-READBACK-DEATH.md`)

`0.1.9.84`, packaged 8-URL sequence, `gpudefault=1`. What is MEASURED:

- `example.com` painted fully (`710656/710656`), twice.
- `news.ycombinator.com` painted (`587134/710656`), `Composite=-4` as expected (no layer).
- The layer probe **loaded**: `SL: curl done crc=0 http=0 size=539` — 539 bytes is exactly
  `layertest.html` — then `DocumentWriter feed done`, then `SL: pumpLoop done`.
- Then the `delay=0` burst fired three navigations in 0.8 s, all parked behind the in-flight load
  (`engine owned (loading=1 interacting=0 busy=1) -> parked`), and the process died.
- `unhandled.txt` ABSENT — no unhandled exception. `exit-ok.txt` ABSENT — not a clean exit.
- `crashverdict.txt` is from an **earlier** run (18:20 vs log at 21:04). Check dates before reading it.
- No fresh `wedgedump-<PID>.txt` for this run: newest is `wedgedump-9560.txt` at 18:19:22.
- `port-trace.txt` tail: `pump: RunLoop::run() returned` → `settle.stop enter` → `settle.stop done` →
  `watchdog.stop done` → `spa: probe rc=0 kick='no-mod'` (last line). So the pump completed and both
  timers stopped cleanly.
- **No `[GPU] present failed` and no `[GPU] readback failed` anywhere.** My paint fixes never reported
  a fallback, so 3a/3b are not implicated.
- `[GPU] enter` appears a second time at line 234 — the per-URL GPU re-probe from .81 works.

NOT VERIFIED / next measurements:

1. Why no wedge dump fired. The dumper needs `noProgress && (busy || m_loading)` for N consecutive
   beats; the death took ~0.8 s, so it may simply not have had time. Confirm before changing anything.
2. The `if (g_gpuActive) return kOK` early-out in `WebCoreGpuInit`: on the second probe the context
   already exists, and it is unknown whether that leaves a stale context when the window size differs.
   This is the nearest unexamined suspect.
3. `http=0` with `size=539` is normal for `file://` (no status code), but it means the layer probe
   exercises the local-file load path, **not** the network path real sites take. A network-served page
   with a forced layer would be a better probe.

### 5a. 2026-09-04: narrowed to one call. Read this instead of the three items above

All three are now answered, and the death has a location. The method was to light the unlit window
rather than reason about it: the whole stretch from `probeSpaModule` to `after-load` carried no trace
at all, so a ~0.9 s death inside it could only be guessed at. Markers now cover it (`tail:` in
`WebCoreSessionLoad`, `rb:` inside `gpuCompositeReadback`).

MEASURED, over two runs (`0.1.9.85` and `.86`, x64 bench, `-Gpu`):

- The death is inside **`TextureMapper::beginPainting(FlipY::No, texture.ptr())`**
  (`WebCoreDriver.cpp`, in `gpuCompositeReadback`). The trace reaches `rb: docBg ok` and the next
  marker, `rb: beginPainting ok`, never appears. Two consecutive runs, same boundary.
- The texture itself is innocent: `rb: texture ok glErr=0x0000` — `BitmapTexture::create` of
  1024x694 with `SupportsAlpha|DepthBuffer` succeeds with a clean GL error state.
- `gpuPrepare` (flush + `updateBackingStoreIncludingSubLayers` + animations) completes.
- It is **not** specific to the artificial probe. In `.85` the entry that died was **ya.ru**, a
  network-served page, with `rootLayer=1`. Item 3 above is therefore already answered: a real site
  reaches this path, and dies in it.
- `rootLayer=1` occurs exactly once per run. Every other entry (example.com twice, HN, and both
  earlier pages) logs `rootLayer=0` and returns through Cairo with a full `nonWhite`. So the GPU
  branch has one entry point per run and it is fatal every time it is taken.
- Item 1 is answered: at `busy=1` for less than one 700 ms beat, the six-beat wedge threshold
  (4.2 s, raised deliberately to clear the 2.5 s fetch budget) cannot fire. **No dump is expected
  here, and its absence says nothing.** This is a fast fault, not a hang.
- Item 2 is answered and is NOT the cause: `WebCoreGpuInit`'s early-out now logs, and it prints
  `[GPU] already active at 1024x694 -- reusing context` — the geometry matches, so there is no
  stale-size mismatch. The early-out was silent, which is what made it look suspicious.

The suspect, from reading upstream rather than from the trace — **NOT VERIFIED, one run away**:
`beginPainting` ends in `bindSurface` → `BitmapTexture::bindAsSurface` → `createFboIfNeeded` →
`initializeDepthBuffer` → `depthBufferFormat()`, and that function
(`WebKit/Source/WebCore/platform/graphics/texmap/BitmapTexture.cpp:74-81`) does

```cpp
auto* glContext = GLContext::current();
if (glContext->version() >= 300 || glContext->glExtensions().OES_packed_depth_stencil)
```

with **no null check**. Three things line up: it is reached only when `Flags::DepthBuffer` is set,
which is exactly what `gpuCompositeReadback` requests; only on a texture's *first* bind, so nothing
earlier in the session goes near it; and `GLContext::current()` returns null unless the thread-local
`s_currentContext` is a **Native**-type wrapper (`GLContextWrapper.cpp`) — an ANGLE context made
current anywhere on the engine thread makes it read null without anything being wrong with our own
context. `gpuCompositeReadback` also discarded `makeContextCurrent()`'s return value.

`.86` prints `rb: makeCurrent=` and `rb: pre-beginPainting current=`, which decides it. A null there
convicts the path; a non-null clears it and moves the search into `bindAsSurface`'s GL calls.

Also fixed on the way, because it silently invalidated the previous run: `x64-cycle.ps1` seeded only
`test.html` into LocalState, never `layertest.html`, and the install in its own step 5 wipes
LocalState. The `.85` run therefore failed that entry with `curlcode=37` ("Could not open file") and
skipped the only sequence entry that reaches the GPU branch — visibly, in `log.txt`, but the run
still read as a pass at a glance. The cycle script now writes both.

---

## 6. Retracted diagnoses — do not resurrect these

Four confident conclusions died during this session. Each is listed with what killed it, because each
is the kind of mistake that costs a whole day when repeated.

1. **"`WebCoreGpuInit` never ran on x64."** Wrong. Concluded from `tail -12` of
   `gpuinit-steps.txt`, which showed only watchdog spam. A grep of the whole file found all eight
   `[GPU]` markers ending in `[GPU] done`. **A tail is not a grep.**
2. **"The wedge is a spin inside `MicrotaskQueue::performMicrotaskCheckpoint`."** Wrong, and the reason
   is a defect in the instrument, not in the reasoning. The dumper attributed addresses by testing
   `addr - base < 0x4000000` (64 MB) while WebCore's x64 image is 0x2CB2000 (44.7 MB), and printed
   `(unsigned)` — dropping the high word. So JavaScriptCore frames were reported as `WebCore+3900163`,
   an offset **larger than the image**, and symbolised against the wrong DLL. Correct reading, after
   fixing the dumper: `RunLoop::runImpl` → `performMicrotaskCheckpoint` → `JSLock::willReleaseLock` →
   `VM::updateStackLimits` — i.e. *finishing* a unit of JS work, not spinning in it. The JSC watchdog
   would therefore have been useless: it interrupts execution, and the thread was past execution.
3. **"The white screen is closed."** Premature. Said on seeing a non-zero `nonwhite` in the log while
   the screen was still white. **A counter proves a buffer was filled, not that anything is visible.**
4. **"The curl scheduler's main-thread `waitForCompletion` is the ARM wedge."** Probed directly: zero
   `curlsched:` lines in a whole session. Never entered.

### Instrument defects found and fixed (all in `MainPage.xaml.cpp`)

- Module attribution bounded by the **real** `SizeOfImage` read from the mapped PE header
  (`ApoImageSize`), not a 64 MB guess; addresses printed `%p`, never a truncating cast. Single shared
  formatter `ApoFormatAddr` so the three copies of this logic cannot drift again.
- `ApoBuildImageList` walked only to `0x80000000` — correct for ARM32, so on x64 it found **no modules
  at all** and every frame fell through to the guess. Ceiling is now address-size dependent.
- `CopyStackWords` read the stack as `uint32_t`; on x64 that splits every 8-byte return address into
  two halves matching nothing. Now `uintptr_t` throughout, with full-width printing.

---

## 7. Doc state

- `Doc/PUMPLOOP-SILENT-DEATH.md` — postscript added: ARM `.77` closed, x64 open, the retractions above.
  Its own "next steps" list is still accurate for what remains.
- `Doc/HARFBUZZ-ICU-DIVERGENCE.md` — divergence 6 (input path) closed: the bench has a touchscreen and
  the packaged sequence now drives it. Divergence 7 (`gpudefault=0`) closed by the `-Gpu` switch.
- `Doc/PLAN.md` Phase 0 — **NOT updated**. Should record that the from-scratch rebuild of both lines
  actually happened, and that it is what exposed the three x64 breakages in section 2. This is the one
  documentation task still outstanding.
- `Doc/WIKI_EN.md` §11 — accurate; written this session for newcomers (layering, App Container as a
  *smaller* Win32, the three event loops, the mine map).

## 8. Operational notes worth keeping

- `Add-AppxPackage` refuses a same-version reinstall with different contents (`0x80073CFB`). Bump the
  manifest for every **install**, not only every device deploy. This cost a rebuild today.
- The gateway returned three malformed tool results this session, including a phantom "edit applied"
  for a change that was **not** on disk (verified absent with `sed`). Standing rule adopted: after every
  Edit/Write to these files, confirm with `sed`/`git diff` before building on it.
- `nav.txt` is only watched if it exists when the app **starts**; writing it later does nothing.
- The x64 line's real value was demonstrated today: stacks symbolise to function and line, which never
  once worked on ARM32. Use it to debug ARM problems, not as a second product.

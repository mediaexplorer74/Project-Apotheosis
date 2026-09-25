Instructions for AI agents working in this repository. Deliberately short: durable rules and exact
commands only. Everything that ages — status, milestones, root-cause history — lives in the places
listed under "Where the rest lives", not here.

## How to work here (read first)

- **MVP is the goal.** v1.0 is a light, minimal browser (see "What v1.0 is" in `Doc/PLAN.md`): ten
  sites in sequence, pages that finish, content that paints, scrolling that moves. Do not gold-plate;
  judge every change against that, not against web-platform completeness.
- **Be concise.** Short answers, minimal new prose. Do not re-explain what a comment or the code
  already says. State a judgment call in a line, not a page.
- **Do not spawn documentation.** There are already ~45 `Doc/*.md` files. Do not create a new doc to
  record a finding, status or fix. Append the one or two lines to the *existing* relevant file, or
  fold it into `Doc/PLAN.md` under the right live item. A finding that does not change anyone's next
  step belongs in a code comment, not a document.
- **Do not opportunistically refactor or "improve" working code.** Comment/doc cleanup is a side
  task, not the mission. Prefer the smallest change that makes the next measurement possible.
- **Comments.** New comments and docs are in English. Some Chinese comments in `.ps1` build/deploy
  scripts are deliberately left as-is (a tribute to the original team) — do not translate or remove
  them. The `?`-mangled comments in the port/harness `.cpp` may be replaced with a terse English
  one-liner (see `Doc/DESTROYED-STRINGS.md`); the `L"???"` module-name placeholder is not one of them.
- **The `en/ru/zh` UI strings in `MainPage.xaml.cpp` are a feature, not a defect.** Do not "clean
  them up".

## What this is

**Project Apotheosis / EdgeHTML Reborn** — modern WebKit/WebCore (webkitgtk-2.52.4) ported to
**Windows 10 Mobile / Lumia 950 (ARM32, UWP, App Container)** with JIT and GPU compositing, so a
phone Microsoft abandoned renders real modern pages. Day-to-day work happens on the **x64-uwp**
build (`build-x64-gpu`) because it can be run and debugged locally; **ARM32 on the real device is
the actual target**, and every change has to stay portable to it.

Branches: `gpu-path1` = GPU line (current), `master` = JIT-only archive.

## Hard constraints

- **ASCII paths only.** The Ruby generators and meson choke on non-ASCII and on spaces. The repo
  lives at an ASCII, space-free path; vcpkg at `C:\vcpkg`. Scripts resolve everything from
  `$env:APOTHEOSIS_ROOT` (set by `Src\setenv.ps1`) — **never hardcode an absolute path**.
- **Three toolchains, do not mix.**
  - WTF/JSC/WebCore: **clang-cl** (LLVM 22.x). ARM32 `--target=thumbv7-unknown-windows-msvc`,
    x64 `x86_64-unknown-windows-msvc`. Pure MSVC is not an option — upstream dropped it.
  - `Src\port\*.cpp`: clang-cl, linked with **lld-link**.
  - `Src\harness` (C++/CX UWP): **MSVC v143** (VS 2022 Community). ARM32 needs
    `arm32-uwp-env.ps1` to hand-build INCLUDE/LIB; x64 uses the normal `vcvars64.bat`.
- **C++ exceptions are off.** clang's thumbv7-windows-msvc backend cannot lower `cleanupret`, so
  `_HAS_EXCEPTIONS=0` + `/EHs-c-` everywhere. Code that needs exceptions cannot be used.
- **Every upstream WebKit edit is guarded** by `#if defined(WK_WINUWP)` and carries an
  `Apotheosis:` comment. The guard keeps upstream semantics intact; the comment is how the next
  agent finds the fork's edits.
- **Software rendering is the base, GPU is a runtime opt-in.** Compositing is gated strictly on
  `g_gpuActive` (false until `WebCoreGpuInit` succeeds). Without GPU the engine falls back to
  Cairo + EmptyChromeClient with zero regression. Enabling compositing unconditionally once cost a
  silent `__fastfail` on the device with no dump.
- **The GPU surface is the panel's physical pixels; the engine viewport has to stay in DIPs. Two
  numbers, and `WebCoreSetPageZoom` is what keeps them apart.** `GpuPanel` is a `SwapChainPanel`, so
  XAML stretches whatever swapchain size ANGLE is given across the whole panel: `CompositionScale` is
  **2.00 on the bench, 2.5 on the Lumia**. A DIP-sized surface is therefore magnified (the original
  «растянутая морда»), while handing the *physical* size to the engine un-compensated lays the page out
  at twice its width — `contents=2736x12368` where software mode gives `1368x12368` for the same window
  and page. The GPU path renders at `DIP × CompositionScale` **with CSS page zoom set to the same
  factor**, which keeps the layout at DIP width while rasterising at device resolution. It is *not*
  `deviceScaleFactor`: that only raises tile resolution (`GraphicsLayerTextureMapper.cpp:586`) and leaves
  the root layer at the CSS width (`RenderLayerCompositor.cpp:3225`), i.e. the page in the top-left
  quarter. `WebCoreSetPageZoom` re-lays out and deliberately does **not** repaint.
  **A navigation resets the factor behind you** (measured twice), so it is a request the port remembers
  (`g_pageZoom`) and re-asserts at present time (`apoReassertPageZoom`), not a one-shot setting — and a
  per-navigation hook that returns early for the "normal" case (as `ReevaluateGpuForDocument` did at
  `if (ec != 0) return;`) leaves every page after the first laid out twice too wide. `Doc/GPU-LIVENESS.md`
  §9; ARM32 cost of the 2.5× raster is measured on the bench but **not yet on the device**.
- **Threading.** Present happens **only on the engine thread**. The UI thread must **never**
  synchronously wait on the engine: ANGLE marshals surface create/resize back to the panel
  dispatcher, so waiting both ways deadlocks and `RunOnUIThread`'s timeout calls `std::terminate`.
  All C ABI calls are serialized on one engine thread.
- **An ABI entry point that can run as the engine thread's *first* job must call
  `ensureWebCoreInitialized()` itself, and `RunLoop::mainSingleton()`'s ASSERT is not a guard.**
  `WebCoreReleaseMemory` was the one entry point that skipped it, which was harmless until the harness
  finally called it — then the app failed to start in **about three launches in ten**. The UWP memory
  event fires *at handler registration* (the platform reports a fresh process's first reading as a rise
  into `low`, the lowest level), the handler posts a `mem-release` job, and that job lands ~60 ms before
  `WebEngine: loop ready` — so `WebCore::releaseMemory` → `releaseCriticalMemory` →
  `MemoryPressureHandler::isUnderMemoryPressure` → the Windows constructor, whose member initialiser is
  `m_windowsMeasurementTimer(RunLoop::mainSingleton(), …)`. `mainSingleton()` is
  `{ ASSERT(s_mainRunLoop); return *s_mainRunLoop; }` and **the ASSERT compiles out in release**, so a
  still-null `s_mainRunLoop` yields a null *reference* and the `Ref<RunLoop>` refcount increment faults
  at **offset 8** — which is the `faultaddr=0x8` you will see, and the reason the frames read
  `JSC+da0ab0` / `WebCore+199fad2` / a `Harness` lambda. Two things to carry: the fix belongs in the
  driver (init before touching WebCore, so *any* pre-init job is safe) **and** in the harness (do not
  release at `low` — the platform's resting state — since on the Lumia that is re-decodes the CPU cannot
  spare); and a startup race that the platform triggers only sometimes has to be **armed by hand** to be
  verified at all — `LocalState\mem-release-race.txt` posts the job at the exact failing position and
  `Src\tools\startup-soak.ps1` reports per launch whether the arm fired, refusing to count a sweep that
  never armed the trigger. Measured 2026-09-24: 9 armed launches, 9 alive, 0 `0xC0000005`.
  `Doc/PLAN.md` 0o.
- **The C ABI header exists twice** — `Src\port\WebCoreDriver.h` and `Src\harness\WebCoreDriver.h`.
  Adding or changing an export means editing **both**, or the ABI silently disagrees.
- **The port is not part of WebCore: it links WebCore's *import library*.** So the only WebCore symbols
  `Src\port\*.cpp` can reference are the `WEBCORE_EXPORT`ed ones — plus anything **defined inline** in a
  header (a template, or an `inline` like `LocalFrame::protectedDocument()`), which needs no symbol at all.
  An out-of-line function that is merely *visible* in a header compiles fine and fails at **link** time with
  `LNK2019`, in the harness build, long after the edit — neither the port's own compile nor `lib.exe`
  notices. Measured 2026-09-18: `Frame::page()`, `Document::page()` and `LocalFrame::page()` are not
  exported (`Page::forEachPage` is), and neither is `JSC::StackVisitor::Frame::hasLineAndColumnInfo()`.
  Check first with `llvm-nm --defined-only build-x64-gpu\lib\WebCore.lib` — `--defined-only` matters,
  because the archive also carries `__imp_?sym` entries and a plain grep matches imports. Table and worked
  replacement: `Doc/UNKNOWN-EXPORTS.md`.
- **`Page::create` keeps the empty-client defaults silently, and one of them cost weeks.** A web platform
  feature is not "present" because its header is included — it is present only if something was installed
  on the `PageConfiguration`. `pageConfigurationWithEmptyClients()` supplies no-ops that answer *success*:
  with no `StorageNamespaceProvider`, `localStorage` existed, threw nothing and stored nothing, which made
  dzen.ru's read-after-write loop non-convergent and froze the engine. Before blaming a site for a loop, a
  mis-sync or a dead control, confirm the feature it depends on is installed, not merely declared.
  `Doc/DZEN-SCROLL-DEATH.md` §10f; `Src\port\PortStorage.{h,cpp}` is the shape of the fix.
- **Per-architecture build outputs must carry the architecture in their name.** Object files already do
  (`.x64.obj`, `.arm32.obj`), and the driver archive now does too:
  `WebCoreDriver-gpu-x64.lib` / `WebCoreDriver-gpu-arm32.lib`. Both link scripts used to write one
  `WebCoreDriver-gpu.lib`, so whichever ran last replaced the other — on 2026-08-21 an ARM relink turned
  the 47 MB x64 archive into the 1 MB ARM one and the next x64 build failed on unresolved
  `WebCoreSetCACertBlob`, symbols that existed but in a library for the wrong machine. If you add another
  per-architecture artefact, suffix it.
- **Never freeze XAML-generated code.** `MainPage.xaml.cpp` ends with `#include "MainPage.g.hpp"`
  and must keep doing so. The XAML compiler renumbers the connection ids inside `Connect()` on every
  `MainPage.xaml` change; a hand-pasted copy of the generated file does not. A stale id binds an
  `x:Name` field to the wrong control or leaves it null — and a null `^` handle in C++/CX is a raw
  dereference, i.e. an access violation that `catch (...)` cannot catch under `/EHsc`. That is what
  killed startup silently in 0.1.8.64 through 0.1.8.67. `Generated Files\` is build output: read it,
  never edit it.
- **Never build the harness with `/t:Rebuild`.** Clean deletes `Generated Files\MainPage.g.hpp`, and
  the XAML compiler does not write it back (`MarkupCompilePass2` alone dies with `WMC9999`), so the
  next compile fails with `C1083: Cannot open include file: 'MainPage.g.hpp'`. That file is a build
  *input* in practice. Recovery: copy `MainPage.g.hpp.backup` over it, then check that every `x:Name`
  in the freshly generated `MainPage.g.h` is assigned in `Connect()` **in the same order** — ids are
  handed out in document order. `Src\tools\_build-appx-x64.bat` uses a plain `Build`; keep it that
  way. Details in `Doc/UNIFICATION.md` §C1. The guard that catches this drift,
  `Src\tools\verify-xaml-connect.ps1`, is wired `AfterTargets="MarkupCompilePass2"` — **not** Pass1,
  which is where it used to be. It reads `MainPage.g.hpp`, and Pass2 is what writes that file, so on
  Pass1 the guard could fail the very build that would have regenerated it. Measured 2026-09-03.
- **Both architectures compile the GPU path. `USE_TEXTURE_MAPPER` is 1 on x64 too.** A comment in
  `paintToRGBA` claimed the opposite for months ("x64-gpu: USE(TEXTURE_MAPPER)=0 ... always falls
  through to Cairo"); `build-x64-gpu/cmakeconfig.h` and the symbols in `WebCoreDriver.x64.obj` both
  say otherwise. Do not reason about the x64 line as Cairo-only.
- **`ENABLE_JIT=ON`, `ENABLE_DFG_JIT=OFF`, `ENABLE_FTL_JIT=OFF`, `ENABLE_C_LOOP=OFF` — on both lines.**
  LLInt + baseline JIT and nothing above them, on the bench and on the phone alike (both
  `CMakeCache.txt` files agree; measured 2026-09-18). The benchmark is therefore *not* misleading on
  this axis, and `Doc/HARFBUZZ-ICU-DIVERGENCE.md`'s "interpreter on bench versus JIT on device" is
  closed. The LLInt on ARM32 is offlineasm-generated ARMv7 Thumb-2, JSVALUE32_64 — not the portable C
  loop, and not absent. `jitresult.txt` naming Thumb-2 while running on x64 is expected, not a bug.
  An older claim here that `build-x64-gpu` included FTL was wrong.
- **A file that has not been compiled for an architecture is not a file that compiles for it.**
  Builds .41 through .77 were ARM-only — 37 in a row — and when x64 was rebuilt on 2026-09-03 the
  harness did not compile at all: the wedge-dump unwinder and the register dump both used ARM
  `CONTEXT` members (`Pc`, `Lr`, `R0..R12`) with no architecture split. Anything touching `CONTEXT`,
  `RtlLookupFunctionEntry`, register names or pointer width must be written for both from the start,
  or compiled for both before it is trusted. See `Doc/HARFBUZZ-ICU-DIVERGENCE.md`, postscript.
- **A diagnostic that guesses which module an address belongs to will eventually lie, and it will be
  believed.** The wedge dumper walked the address space only to `0x80000000` (a 32-bit ceiling, so on
  x64 it found no modules at all), then fell back to attributing any address within 64 MB of a known
  base to that module — while WebCore's x64 image is 44.7 MB — and printed addresses as `(unsigned)`,
  dropping the high word. Frames in JavaScriptCore were reported as `WebCore+3900163` and symbolised
  against the wrong DLL, producing a complete and completely wrong diagnosis. Rules that came out of
  it: print `%p`, never a truncating cast; bound module attribution by the real `SizeOfImage` from the
  PE header; size stack-word buffers as `uintptr_t`; and **check the arithmetic** — an offset larger
  than the image cannot be inside the image.
- **An RVA means nothing without the build that produced it, and a `LocalState` full of logs holds
  more than one build.** On 2026-09-18 every WebCore frame in the aggregated `VEH #` lines appeared
  *twice*, the pairs differing by exactly `0xD0` in `.text` — two builds, not a broken unwinder. The
  same instruction has been recorded at `0x204901f`, `0x204903f` and `0x204910f`. `log.txt` is
  truncated per run but `prelaunch-*` is not, so **check which build a log came from before
  symbolising it, and symbolise against the DLL that produced it.** `llvm-symbolizer` and `cdb` both
  answer `??:0:0` on the clang-cl x64 PDB; the working recipe is `.pdata` from `llvm-readobj --unwind`
  for the function plus `llvm-pdbutil dump -l` for file:line, where the line-table ranges are
  **section-relative** (subtract the `.text` RVA `0x1000`) and `--unwind`'s are not.
  `Doc/CALC-HANDLE-DANGLING.md` §3.
- **Contain a defect in one function, not in each copy of the check.** A stale
  `LengthWrapperData::m_calculationValueHandle` (an overderef'd handle into the process-wide
  `Style::Calculation::ValueMap`, whose only guard is a compiled-out `ASSERT`) faults inside
  `WTF::visitOneVariant` on `v.index()` — and because the reference was **never an address**, the AV
  reads `0xF`/`0x10`, not a plausible wild pointer. It was fixed on 2026-09-17 in *one* of the two
  `nonNanCalculatedValue` overloads and came back through the other on 2026-09-18. Containment is now
  the single `apoCalculationHandleIsLive()`, a **file-local function** in `StyleLengthWrapperData.cpp`
  used by both overloads and by `isCalculatedEqual()`; a stale handle returns 0, as a NaN result would.
  Verify it by name in the **`.cpp`**, not the header: the first version was a member on
  `LengthWrapperData` and editing that widely-included header dirtied 362/362 unified sources (§6.2 of
  the doc — the reason it is a free function). `Doc/CALC-HANDLE-DANGLING.md`.
  Related diagnostic trap: **`WTFLogAlways` on this port sinks to `OutputDebugStringA`
  (`WTF/wtf/Assertions.cpp`), NOT to a file** — a `grep` for its message over `LocalState` returns
  nothing, and that absence proves nothing.
- **A stub that fabricates a value where upstream only `ASSERT`s it will eventually hand the engine a
  null and be dereferenced.** `Src\port\stubs-other.cpp`'s `platformSystemFontShorthandInfo` returned
  `{ AtomString(), 0, FontSelectionValue() }` — a **null** family and a size of 0. Upstream's
  `StyleFontFamily.cpp` documents the invariant (`ASSERT(!family.isEmpty())`) and nothing downstream
  re-checks it: the null atom lands in a `FontCascadeDescription`, gets hashed by
  `CSSFontFaceSet::fontFace` → `ASCIICaseInsensitiveHash::hash(const StringImpl*)` — whose only guard
  is also a compiled-out `ASSERT` — and dereferences `nullptr`. That is an AV reading `0x10`, and it
  took a symbolized dump plus six isolating test pages to trace back to the stub. A zero size is fatal
  independently: `FontPlatformData(0, …)` *is* the hash table's `emptyValue()`. When a stub has to
  invent a value, invent one that satisfies the invariant — `SystemFontDatabaseGLib.cpp` is the model,
  and `Doc/FONT-NULL-FAMILY-CRASH.md` is the whole audit. This is the `SILENT LIE` class of
  `Doc/STUB-AUDIT.md`, found the hard way.
- **`Add-AppxPackage` refuses a same-version reinstall with different contents** (`0x80073CFB`), so the
  manifest version must be bumped for every **install**, not only for every device deploy.
- **Editing any `WebKit/Source/cmake/*.cmake` file makes ninja regenerate `build.ninja`, and
  `build-arm32-gpu` does not survive that.** Measured 2026-08-22, at the cost of a full ARM WebCore
  rebuild. Its working `build.ninja` predates the CMake sources it was generated from, so regenerating
  it produces a *different and broken* configuration: shared Cairo instead of static (WebCore.dll then
  imports `cairo-2.dll`, which cannot load on Windows 10 Mobile at all), plus `user32/gdi32/msimg32`
  colliding with the port's own `stubs-gdi-uwp.cpp` — 20 duplicate symbols. Regeneration also drops the
  bindings stamp, which turns any target into a **1365-task from-scratch build**.
  Before touching a CMake file, take `cp build-arm32-gpu/build.ninja{,.keep}` and
  `cmakeconfig.h{,.keep}`; restoring them stops the reconfigure loop, but note that ninja compares the
  *recorded* mtime, so the CMake file must also be made older than `build.ninja`
  (`touch -r build-arm32-gpu/bin/WebCore.dll <the .cmake file>`). Two landmines the same reconfigure
  exposed, both pre-existing: the desktop libs above, and `wtf/win/MemoryFootprintWin.cpp`, which
  entered WTF's source list around 08-18, was never once compiled for ARM32 (its object simply did not
  exist), and cannot compile in an App Container — `QueryWorkingSet` and
  `PSAPI_WORKING_SET_INFORMATION` are outside `WINAPI_PARTITION_APP`. It is now guarded and returns 0.
  **`build-arm32-gpu` being reconfigurable at all is unfinished work, not a solved problem.**
- **A WTF header edit does not have to cost a full rebuild — but the naive way does.** Every
  `WebKit\Source\WTF\wtf\*.h` is copied into `build-x64-gpu\WTF\Headers\wtf\` by an unconditional
  `cmake -E copy` under `restat = 1`, and `RunLoop.h` pulls `RedBlackTree.h` into nearly everything:
  touching one turned a 2-task build into **736 tasks** (measured 2026-09-19) — every WTF unit, every
  WebCore unified source, the LLInt, both DLL links; a day at `-j1`. Template **member functions are
  instantiated only where they are odr-used**, so: edit the source header, `cp` it over the build copy,
  pin **both** back to the mtime ninja recorded for the copy (`touch -d "<recorded mtime>" <src> <dst>`
  — never move a timestamp with `cp`: `cp` copies *content*, and doing that once wrote a zero-byte
  `/tmp` file over a 20 KB upstream header), delete only the `.obj` of the translation units that
  actually call the changed member, and run ninja for them. `RedBlackTree::remove()` has exactly three
  callers: `generic/RunLoopGeneric.cpp`, `MetaAllocator.cpp`, and `ExecutableAllocator.cpp` (inside a
  JS `UnifiedSource-*`). WTF's own units include the *source* tree; WebCore and JSC include the build
  *copy* — both have to be updated. The next full build picks the change up everywhere for free.
  **`WebKit/` is a git work tree**, so `git -C WebKit checkout -- <path>` recovers an upstream file
  destroyed by accident; nothing under `Src/` has that safety net. `Doc/RUNLOOP-TEARDOWN-ABORT.md` §10.
- **A `CheckedPtr` that outlived its object is not a refcount bug — it is a corrupted container.**
  `crashDueToCheckedPtrToDeadObject()` fires when a link is decremented against an object that was
  deleted while still linked (the deleting-delete zeroes it and leaks it, so the zero is permanent and
  the crash deterministic). Upstream `RedBlackTree::remove()` left the unlinked node holding its own
  `m_left`/`m_right` `CheckedPtr`s — two stale links in the `y != z` branch, one in `y == z` — so a
  removed `ScheduledTask` kept a count alive on a node the tree still owned, and the failure only
  appeared when that node died. Fixed in the fork by `z->reset()` at the end of `remove()` (guarded,
  `Apotheosis:` comment). The lesson for the next bug of this family: an invariant that mixes a
  container with a counted pointer has to be *tested* — `apoRBCheck` in `RunLoopGeneric.cpp` walks the
  tree and compares the sum of the counts against the size (`sum != size` is a link from outside the
  tree), and `Src\tools\analyze-runloop-trace.ps1` reports it. Do not walk such a tree with a
  `CheckedPtr` in hand: it raises the count it is measuring. **The probe is in `JavaScriptCore.dll`, not
  `WebCore.dll`** (same split as the `WTFCrash` tracer), so a `grep` of `build-*/bin/WebCore.dll` finds
  nothing even for a marker the trace is full of — check the **appx payload**, which is what the
  installed package actually contains. `Doc/RUNLOOP-TEARDOWN-ABORT.md` §11.
- **Editing one widely-included WebCore *header* costs a full WebCore rebuild; the same change in the
  `.cpp` costs one translation unit.** Measured 2026-09-18 on `build-x64-gpu`: a declaration-only edit to
  `style/values/primitives/StyleLengthWrapperData.h` made **362 of 362** unified sources dirty — ~10 hours
  at `-j1` — because 348 of them reach that header transitively. Nothing was actually stale: the edit added
  a member *declaration* (no layout, no ABI, no codegen), and reverting it left the objects byte-correct.
  Prefer the `.cpp` whenever the two forms are equivalent (a file-local helper can do what a member method
  does, as long as the call sites are members). If you must undo such an edit, the mtime has to be restored
  **twice**, and the second one is the non-obvious half:
  `WebKit\Source\WebCore\...\Header.h` back to the time the objects were built, and — with `touch -h` —
  `build-x64-gpu\WebCore\PrivateHeaders\WebCore\Header.h`, because that path is a **symbolic link** and
  ninja on Windows stats **the link itself**, not its target; `touch` without `-h` follows the link and
  does nothing. Set the link one second *newer* than the target so the copy rule's `output >= input` check
  passes and does not recreate it. Verified: 362 pending → 2. Ninja has no content hashing — only mtimes —
  and `ninja -n -d explain` names the input it is unhappy about, which is how this was found at all.
  `Doc/CALC-HANDLE-DANGLING.md` §6.2.
- **One Windows SDK, `10.0.19041.0`, for both architectures — never newer.** 26100 ships no `um\arm` /
  `ucrt\arm`, so it cannot build ARM32 at all. `Src\harness\Harness.vcxproj` pins 19041 unconditionally,
  which also means both architectures run the **same XAML compiler**. The price of 19041 is that it
  keeps `LoadLibraryA/ExA` and `GetModuleHandleA/W` outside the `APP` partition: use
  `LoadPackagedLibrary` for package DLLs and the linker-provided `__ImageBase` for this module's base,
  as the harness now does. The x64 **engine** line is still on 26100 (`Src\setenv.ps1`,
  `Src\port\x64-uwp-env.ps1`, `build-x64-gpu`'s CMakeCache), so 26100 must stay installed until that is
  reconfigured, or an incremental WebCore build breaks. `Doc/UNIFICATION.md` Track B has the
  measurements.
- **No stack buffers inside the `#pragma strict_gs_check(push, off)` regions** of
  `WebCoreDriver.cpp`. The security-cookie prologue is what those regions switch off.
- **`ninja -j1`, always.** 4 GB RAM; unified-source translation units exceed 1 GB each.
- **Comments: existing Chinese text is upstream/fork heritage — never translate, rewrite or delete
  it.** Chinese comments in PowerShell (`.ps1`) scripts and existing intact comments remain untouched
  as heritage/tribute. New comments and docs are written in English. Some Chinese comments were mangled
  to `?` by an encoding accident; those may be removed or replaced, but only the mangled ones. The same accident
  also destroyed **seven user-visible string literals** (home page, error page, tab-switcher title);
  those are rewritten, not restored — there is nothing left to restore, the bytes are literally `0x3F`.
  Before "fixing" a `???` in this tree, check which of the three it is: destroyed text, deliberate
  placeholder, or header comment. `Doc/DESTROYED-STRINGS.md` has the audit.
- **Conciseness and doc hygiene: do not proliferate doc files.** Agents must keep answers and changes
  concise and focused on real code. Do not author speculative or sprawling documentation files (e.g. 100+
  breakdown markdown files) unless explicitly asked by the maintainer. Keep project memory tight, durable,
  and grounded in verifiable code and measured runtimes.
- Measure appx size with PowerShell `.Length`. `ls -la` misreads the columns because Windows owner
  names contain spaces.

## Repo layout

The repo tracks **only the port layer and the host app**. The multi-GB upstream tree and every
re-downloadable binary stay out of it.

- `Src/port/` — the WebCore driver, the Port-layer clients, the symbol stubs, and the build/link
  scripts. ⚠️ The real sources sit among a lot of one-off debugging residue (`repro_*.cpp`,
  `mangle-repro*`, loose `*.obj`/`*.lib`/`*.dll`, `*.log`, `undef-*.txt`, `_*.bat`) left over from
  fighting the link. Ignore those; the files that matter are listed below.
- `Src/harness/` — the UWP host app (C++/CX, XAML, `Package.appxmanifest`, signing certificate).
- `Src/tools/` — Device Portal (WDP) deployment, crash-dump collection, diagnostic loops.
- `angle/include` — ANGLE headers (tracked). `angle/arm`, `angle-windowsstore` binaries are
  gitignored because they can be re-downloaded.
- **Not in the repo**: `WebKit/` (the sparse webkitgtk-2.52.4 checkout), `build-*/`, `deps-build/`,
  fonts, `*.pfx`, `*.log`. The list of upstream ARM32 / App-Container patches lives in project
  memory, not here.

Core port sources:

| File | Role |
|---|---|
| `WebCoreDriver.cpp` / `.h` | The C ABI and the resident Page session: navigation, real event dispatch, link extraction, software and GPU present |
| `PortChromeClient.{h,cpp}` | Non-final `ChromeClient` subclass (EmptyChromeClient's compositing hooks are `final`): enables compositing, captures the root `GraphicsLayer`, sets needsPresent from `triggerRenderingUpdate` |
| `LoadingFrameLoaderClient.{h,cpp}` | A `FrameLoaderClient` that actually answers policy (`PolicyAction::Use`) instead of an empty one |
| `PortPlatformStrategies`, `PortNetworkStorageSession` | Install the LoaderStrategy and the network storage session |
| `PortStorage.{h,cpp}` | The in-memory Web Storage backend: `PortStorageNamespaceProvider` + one area per origin, with quota accounting and real `storage` events. Installed at **every** `Page::create` site |
| `stubs-*.cpp` | Stubs for platform symbols this port does not implement (crypto / network / pasteboard / ax / loader / other) |
| `Toolchain-ARM32-UWP-clang.cmake`, `arm32-uwp-env.ps1`, `clang-cl-arm-shim.h` | Toolchain and environment |

## Architecture

Three layers, decoupled by one C ABI:

```
Harness (C++/CX UWP, MSVC v143)                         Src/harness/
  · MainPage: address bar + toolbar, touch gestures → engine scroll/click/zoom/select
  · GpuPanel (SwapChainPanel) ← GPU direct present | RenderImage (WriteableBitmap) ← software
        │  C ABI = WebCoreDriver.h  (extern "C")
WebCoreDriver (clang-cl → WebCoreDriver-gpu.lib)         Src/port/
  · Resident Page/Frame/FrameView session, real event dispatch, link extraction
  · Two present paths: Cairo paintToRGBA (software) | TextureMapper → ANGLE swapchain (GPU)
  · PortChromeClient / LoadingFrameLoaderClient / Port*Strategies
        │
WebCore / JavaScriptCore / WTF (clang-cl, App Container)  WebKit/ (not tracked)
  · Every ARM32 / App-Container patch guarded by WK_WINUWP
```

GPU compositing recipe (`WebCoreComposite`, mirroring WebKit's `WCScene::update`):
`flushCompositingStateIncludingSubframes` → `updateBackingStoreIncludingSubLayers` →
`applyAnimationsRecursively` → `beginPainting`/`paint`/`endPainting` → `eglSwapBuffers` (direct
present) or `glReadPixels` (offscreen readback, for verification). The root layer is
`PortChromeClient::rootLayer()`, a synchronous `GraphicsLayerTextureMapper`
(`USE_COORDINATED_GRAPHICS=0`).

Resizing the GPU path rebuilds the GLContext and surface **on the engine thread**
(`WebCoreGpuResize` + `EGLRenderSurfaceSizeProperty`), then re-runs TextureMapper and
`finishInteractionPaint`. Doing it any other way stretches the whole page.

## Build configurations

| Directory | Configuration | Purpose |
|---|---|---|
| `build-x64-gpu` | x64-uwp: GPU (TextureMapper + ANGLE) + JIT (LLInt + baseline, no DFG/FTL) | The local development line |
| `build-arm32-gpu` | ARM32: GPU + JIT | The device line |
| `build-arm32-jit` | ARM32: JSC JIT, software rendering | JIT-only bisect line |
| `build-arm32-webcore` | ARM32: Cairo software rendering | Baseline, no JIT |

Directory names follow `build-<arch>-<variant>`; object files follow the same split — GPU
`.arm32.obj`, JIT `.arm32-jit.obj`, software `.arm32-soft.obj`, x64 `.x64.obj`. These directories are
build output: they are not tracked, and an ARM32 one has to be created by
`Src\port\configure-gpu-arm32.ps1` (then hours of `ninja -j1`) before it exists. Both architectures
share one `Src\harness\Package.appxmanifest`, so their appx version numbers cannot drift apart.

## Commands (PowerShell 7)

Every session starts with `. .\Src\setenv.ps1`, which sets `$env:APOTHEOSIS_ROOT`.

**x64 — the whole test loop in one command.** Bumps the manifest version (Add-AppxPackage refuses to
reinstall the same version, and the Remove-AppxPackage that would otherwise be needed wipes
LocalState), relinks the driver, builds the appx, reinstalls, seeds `LocalState\settings.ini`,
launches, then tails the logs:

```powershell
pwsh -File Src\tools\x64-cycle.ps1 -Url https://example.com
pwsh -File Src\tools\x64-cycle.ps1 -SkipDriver -SkipBuild -Url https://news.ycombinator.com
pwsh -File Src\tools\x64-cycle.ps1 -Url test -Gpu       # deterministic font page, GPU armed
```

> **The seeded `settings.ini` says `gpudefault=0`, so the bench runs with compositing OFF unless you
> pass `-Gpu`.** Every "the bench survives this" result obtained without it was produced by code that
> was not running the GPU path at all, while the device runs with it on — which invalidated a day of
> reasoning on 2026-08-21. The default stays 0 because software rendering is the project's baseline;
> pass `-Gpu` whenever the point is to mirror the device.
>
> **And until `0.1.10.13`, passing `-Gpu` did not mirror the device either.** The `gpu-init` job's
> diagnostic EGL probe called `eglTerminate` on the display the engine was about to use — ANGLE resolves
> `eglGetPlatformDisplay*` through a per-native-display cache, so the probe's EXT+D3D11 branch hands back
> the engine's own `Display` object. `EnableGpu` is retried on every new url while `!m_gpuOn`, and
> `m_gpuOn` only becomes 1 when the first frame's `WebCoreComposite()` returns 0, so the probe ran four
> times in one measured session. Every GL operation after the first composite then failed
> `glctx: eglMakeCurrent FAILED err=0x3001` (EGL_NOT_INITIALIZED — 228 of 228 readbacks), the harness
> stayed on software present, and `bt:`/`tm:` probes that looked dead had in fact fired only in the
> windows between a composite and the next probe. The probe now runs **once per process** and terminates
> **nothing**; on `0.1.10.13` the bench reached `Composite=0` → `presenting through the GPU surface` for
> the first time, with 178 TextureMapper paints and zero context failures. **Re-measure any GPU claim
> made before that build** — it was gathered against a context the host had killed.
>
> **`Composite=-4` is `kErrNoView` and it means three different things**: no frame, no view, or no
> `chrome->rootLayer()`. The marker `[GPU] Composite -> kErrNoView: … root=…` in `gpuinit-steps.txt` names
> which. A page with no promoted layers legitimately has no root layer (only `attachRootGraphicsLayer`
> sets one), so `-4` on such a page is the harness working as designed, not a failure. `Doc/GPU-LIVENESS.md`.
>
> **The GPU auto-probe keys on the *document's* url, not on the url the harness requested — and until
> `0.1.10.19` it keyed on the requested one, which made the GPU unreachable on real pages.** The two are
> different whenever the site redirects or the page navigates itself, and one session contained the proof:
> two loads of `https://dzen.ru/` eight minutes apart ended on `sso.dzen.ru/install` (body=0, no root
> layer, probe correctly refused) and on the real `dzen.ru` (`compositing=1`, root layer present). The
> key was the shared *requested* string, so the second load was never probed — seven probes, zero
> successes in the session. `OnNavDone` now takes the engine's url (`finalUrl`, already read on the engine
> thread and already marshalled to the UI thread — no new ABI call, and the UI thread still never waits on
> the engine). The decision is logged both ways: `EnableGpu: probing for document … (requested …)` and
> `EnableGpu: probe skipped, already tried for document …` — **a skipped probe must never be silent**, since
> silence is what hid this. Measured on `0.1.10.19`: first real page presented through the GPU at 05:53:22
> (`EnableCompositing=1 Composite=0`), session stable 3+ min.
>
> **Do not use `nonwhite` to tell whether the GPU is presenting.** `nonWhite` is computed on the
> readback/software path (`gpuCompositeReadback`); on direct present nothing fills the harness's RGBA
> buffer, so the diag value is whatever the last readback left and it stops moving (held at `571668` for a
> whole session). `Composite=0` and the `[GPU] paint path ->` marker are the signals.
> **`[GPU] direct present -> N` is not one of them**: it is written by `WebCoreSetDirectPresent`, i.e. the
> harness telling the driver whether the panel is on screen — not by `eglSwapBuffers`.
>
> **Once the GPU owns the screen, three separate guards stop a later plain document from taking it back,
> and each one hides the next.** `ReevaluateGpuForDocument()` is the hand-back; it must clear **four**
> things, and `0.1.10.23` was the build that cleared all four. (1) All harness blit sites are
> `if (!m_gpuPresent)`, and the frame for the new document is blitted from the load job *before*
> `OnNavDone` clears the flag. (2) The live tick is not a repair path — it blits only when the frame hash
> *changes*, and `StartLiveMode` has five guards plus a 40-static-tick self-stop — so the hand-back asks
> for one frame explicitly (`gpu-handback-frame`). (3) **`BlitToBitmap` opens with
> `if (g_directPresent.load()) return;`** — a silent early return — and that latch, set in `EnableGpu`'s
> success branch and cleared in its *failure* branch, was left set by this third path. (4) `m_gpuOn` is
> cleared so the existing `EnableGpu` re-arms on a later compositing page rather than growing a second
> path. Measured consequence on `0.1.10.22`, and the reason the guard matters: the hand-back logged
> `EnableGpu: handback frame blitted (1024x694 hash=3cf56188)` — engine-painted hash and all — beside a
> flat `#F0F0F0` window, which is `ContentArea`'s background through an `Image` with no `Source`. **A log
> line at a call site is not evidence the callee did the work.** `Doc/GPU-LIVENESS.md` §8.
>
> Also remember what the bench does **not** reproduce: it is driven by scripts, and each of `-Url`, the
> `nav.txt` watcher and the packaged `navseq.txt` player calls `NavigateTo` exactly once. A real tap on
> the phone produced a Click *and* an Enter `KeyDown` 28 ms apart and loaded every page twice. The dev
> machine is a Surface with a touchscreen, so gestures **can** be exercised here — a scripted
> destination is not a scripted gesture.
>
> **Since 2026-09-18 a gesture can be scripted too, and it enters through the same door a finger does.**
> `LocalState\nav.txt` — the same one-command-per-write file, same watcher — also carries
> `tap:<x>,<y>`, `taplink:<n>` (centre of link *n* of the extracted table, 0-based) and
> `taplinkstr:<text>` (first link whose URL contains `<text>`). All three route through `TapFromScript`
> → `HandleTapAt`, the identical function the XAML manipulation stack calls, and each logs a `simtap:`
> line. `taplink`/`taplinkstr` exist because raw coordinates are brittle: the link table is rebuilt on
> every load, so a coordinate that hit a link once hits a paragraph the next time. This closes the
> `Doc/TOPLEVEL-FETCH-BUDGET.md` §7c gap ("never exercised by a real tap") for *engine-side* taps — but
> only for them: it bypasses XAML gesture recognition, so it still cannot reproduce a defect that lives
> in the manipulation stack itself.
>
> **Who decides a tap's destination: the page, not the frame hash.** The harness's dispatcher
> (`MainPage.xaml.cpp`) adds a navigation of its own only when all four hold: `rc == 0`,
> the engine's document URL is unchanged, the finger was on a link, and the page did **not** call
> `preventDefault()`. That last term is `WebCoreLastClickDefaultPrevented()` — `1` refused, `0` a `click`
> existed and nothing refused it, `-1` no `click` recorded at all (which falls back too, preserving the
> original purpose: a tap the DOM never saw). Until `0.1.10.7` the gate was the **frame hash**
> (`WebCoreGetFrameHash()` before vs after = "did anything repaint"), which answered a presentation
> question where a behavioural one was needed: a page that `preventDefault()`s and updates later looks
> exactly like a page that ignored the tap, and the harness navigated against the page's explicit refusal.
> `changed` is still printed, but it is no longer a gate in either direction. One `TapDone: …` line per
> tap names the branch actually taken (`nav-link` / `in-page` / `nav-fail` / `dead`) and carries
> `refused=`. `Doc/TAP-DISPATCH.md` has the three-row measurement that forced this and the semantics.
>
> **Since `0.1.10.18` that line also carries `preEmpty=`, `termDiff=`, `pre=` and `cur=`**, and the gate's
> URL term is read on the **engine** thread immediately before `WebCoreClickAt` instead of from a
> UI-thread snapshot of `m_currentUrl` taken when the finger landed. The snapshot was wrong for a reason
> nothing in the harness keeps true: `m_currentUrl` is assigned only in `NavigateTo`, `ApplyEngineFrame`
> and `SaveActiveTab`, so **any URL change the page makes on its own leaves it stale for good** — measured
> with a page that sets `location.hash` (`termDiff=1`: the two candidate readings disagreed). The old term
> therefore read "the URL changed" for a click that changed nothing and sent the tap to `in-page`.
> **`refused=-1` on a tap that navigated is not "the page ignored the click"**: the commit replaces the
> document and the probe's `window.__apoClickLog` with it, before the readback runs. §8 of that document.
>
> **`m_currentUrl` is only ever written by harness-initiated navigation** (`NavigateTo`, `ApplyEngineFrame`,
> `SaveActiveTab`) — so a navigation the *page* starts never reaches the URL box, and the address bar
> reports the URL that was **requested** while the engine sits on a different origin. Measured while the
> engine was on `sso.dzen.ru/install` and the box still said `dzen.ru`
> (`Doc/DZEN-FIRST-LOAD-DETOUR.md` §4). It is the same structural fact as the tap gate's URL term above;
> fixing it means deciding which history entry a page-initiated navigation updates, and that was
> deliberately not bundled with the term fix.

**x64 — one file, just to see if it compiles** (seconds, the fastest way to chase a compile error):

```powershell
pwsh -File Src\port\compile-driver-gpu-x64.ps1 Src\port\WebCoreDriver.cpp Src\port\WebCoreDriver.x64.obj
```

**x64 — relink the driver only** (produces `WebCoreDriver-gpu.lib`):

```powershell
pwsh -File Src\port\link-driver-gpu-x64.ps1
```

**x64 — build the appx only.** msbuild needs a full VS environment, and the nested quoting for
`vcvars64.bat` does not survive a POSIX shell, so it lives in a batch file:

```powershell
Src\tools\_build-appx-x64.bat
```

> **Never pass `/p:MinimalTest=true`.** It swaps `MainPage.xaml.cpp` for the `MainPage.minimal.cpp`
> stub and drops every engine `.lib` from the link, i.e. it builds an appx with no browser in it.
> It exists to bisect static-initialisation crashes. A MinimalTest appx is recognisable at a glance:
> ~32 MB and 26 payload files instead of ~54 MB and 50, with no `libEGL.dll` and no ICU.

**x64 — install the built appx IN PLACE, keeping LocalState.** Bump `<Identity Version>` in
`Src\harness\Package.appxmanifest` first: `Add-AppxPackage` refuses a same-version reinstall
(`0x80073CFB`), and the script fails loudly if you forget. It also **stops a running `Harness` first**
— installing over a live process either fails or leaves the old binaries mapped, and a test run
against the old engine looks exactly like one against the new engine. Previous logs are moved to
`LocalState\prelaunch-<MMdd-HHmm>`, not deleted, because `gpuinit-steps.txt` is appended to.

```powershell
pwsh -File Src\tools\update-local-x64.ps1
```

> **Do not use `install-local-x64.ps1` or `x64-cycle.ps1` when LocalState matters.** Both call
> `Remove-AppxPackage`, which deletes the app's local data folder — logs, `settings.ini`, dumps.
> `-KeepState` does not help: it only decides whether stale logs are deleted *after* an install that
> has already wiped everything.

**After editing upstream WebCore** (a `WK_WINUWP` patch), rebuild incrementally, then relink:

```powershell
& "C:\Program Files\CMake\bin\ninja.exe" -C build-x64-gpu WebCore -j1
pwsh -File Src\port\link-driver-gpu-x64.ps1
```

**ARM32 — bring the line up from nothing.** The ARM tree is not present after a fresh clone (no
`build-arm32-*`, no `arm-uwp` vcpkg triplet, no `C:\icu-arm-uwp`), and the chain to rebuild it is long
enough to want running unattended:

```powershell
pwsh -File Src\tools\arm-bootstrap.ps1 -PreflightOnly   # seconds: checks, changes nothing
pwsh -File Src\tools\arm-bootstrap.ps1                  # hours: vcpkg → ICU → deps → configure → ninja
```

It runs every step in its own process, stops at the **first** failure, and leaves
`Src\tools\arm-bootstrap-logs\verdict.txt` naming the step, its exit code and the last 40 lines of its
output — so an unattended window ends in either progress or a diagnosis, never silence. The preflight
is the important half: it refuses to start without the MSVC ARM32 toolset and prints the exact
`setup.exe modify --add Microsoft.VisualStudio.Component.VC.14.44.17.14.ARM` line, because that
component is the one prerequisite the script cannot install itself. Use the **versioned** component,
not "(latest)": the tree is pinned to toolset 14.44.35207. Resume with
`-SkipVcpkg -SkipIcu -SkipDeps -SkipConfigure` as needed; `ninja` itself resumes on its own.

> **Two shims for the ARM line live under `C:\Program Files` and a VS or SDK update will wipe them.**
> VS 2022 17.14 ships no `vcvarsamd64_arm.bat` / `vcvarsx86_arm.bat`, and vcpkg enumerates target
> architectures purely by the presence of those files; and `vcvarsall` composes `LIB` with `um` from
> the newest SDK, so `Lib\10.0.26100.0\um\arm` — a directory that does not exist — has to be a junction
> to the 19041 one or every ARM link fails on `WindowsApp.lib`. The preflight recreates both.
> vcpkg also needs `--overlay-triplets=Src\port\vcpkg-triplets`, because the stock community
> `arm-uwp` triplet does not pin the SDK. Measurements and failure messages: `Doc/ARM32-BUILD-GUIDE.md`
> §11.

**ARM32 — day to day.** `pwsh -File Src\port\link-driver-gpu-arm32.ps1` for the driver;
`pwsh -File Src\tools\deploy-launch.ps1 -Ip <device-ip> -Ver <version>` to deploy and launch
(`-Ver` must match the manifest); `Src\tools\Deploy-Robust.ps1` when the phone drops WiFi mid-upload,
which it does, because power saving turns the radio off; `Src\tools\auto-diag2.ps1` for the
unattended uninstall → install → launch → pull dumps and BMP screenshots loop (it only arms itself
when the device has an `autodiag.txt`).

> The `*-arm32.ps1` scripts source `arm32-uwp-env.ps1` and fail hard on an x64 box. x64 work uses
> the `*-x64.ps1` variants, always. An ARM32 appx cannot run here either: x64 is verified in the
> Windows 11 AppContainer on the dev box, and ARM32 only on the phone.
>
> **One shell, one architecture.** `arm32-uwp-env.ps1` rewrites `INCLUDE` / `LIB` / `PATH` in the
> *current* session, and the x64 steps inherit the session — so running an x64 step after it would put
> `Lib\10.0.19041.0\um\arm` on `lld-link`'s search path. Each env script therefore stamps
> `$env:APOTHEOSIS_TARGET`, and the arch-specific steps call
> `& "$PSScriptRoot\assert-target.ps1" -Expect <arch>` **before** touching the environment, refusing on
> a mismatch. An unset marker always passes, which is why `setenv.ps1` deliberately does not stamp it:
> it is the shared bootstrap, not an architecture choice. Wired into `compile-driver-gpu-*`,
> `link-driver-gpu-*` and `configure-gpu-*`; the older one-off scripts still need the same one-liner.

## Diagnostics

Everything the app writes lands in its LocalState:
`%LOCALAPPDATA%\Packages\EdgeHTMLReborn.Harness_<hash>\LocalState`.

| File | Written by | Contents |
|---|---|---|
| `log.txt` | harness | Startup, navigation, `[STAGE]` markers, and the `diag:` line (document + loader state after every load and resize) |
| `ctor-trace.txt` | harness | One line per `MainPage` constructor checkpoint (`EarlyMark`), written before `LogInit` makes `log.txt` live. The last line present is where an early death happened |
| `unhandled.txt` | harness | HRESULT + message of an unhandled UI-thread exception (`App::UnhandledException`). Only appears when that happens |
| `heartbeat.txt`, `exit-ok.txt`, `crashverdict.txt` | harness | The crash verdict's three markers. `App::OnSuspending` writes `exit-ok.txt`; `RunCrashVerdict` (`MainPage.xaml.cpp`) reports `CRASHVERDICT: clean-exit` if it is present and `CRASHED last heartbeat=…` if it is not. **A forced kill never suspends, so it prints `CRASHED` too** — `update-local-x64.ps1` does `Stop-Process -Force`, so every install is followed by one. Check for a `prelaunch-*` directory before believing it; a dump or WER event proves a fault, their absence proves nothing. `Doc/TOPLEVEL-FETCH-BUDGET.md` §9f |
| `port-trace.txt` | driver | Loader scheduling (`serve started/inflight/pending`) and the settle loop |
| `gpuinit-steps.txt` | driver | `gpuLogMarker` / `gpuLogMarkerF` markers, one line per call, flushed immediately so a crash keeps them. `WebCorePort::portDiagLog` is the bridge for other translation units |
| `stage.txt`, `glyph.log`, `jitresult.txt`, `layertree.txt` | both | Last stage reached, font/glyph decisions (`ofd:` / `fset:` null-family lines — see below; silent unless `APO_TRACE_TEXT=1`), JIT probe result, layer tree dump |
| `jstack.txt` | you, by hand | Its **existence** arms the JS-stack self-test (`PortChromeClient::addMessageToConsole` → `WebCorePort::portDumpJsStack`, max 3 fires); the port writes `jstack #N url:line:col :: name` lines into `gpuinit-steps.txt`. A file, not a build flag, because it has to arm an already-installed build and WDP cannot write LocalState |

The `diag:` line is the fastest way to tell a rendering problem from a loading problem from a parser
problem: it carries contents size, painted-pixel count, resource statuses, and the document's
readyState / parsing / stylesheet / parser flags.

Inside it, `scr=defer=N err=N blk=N[...]` lists the scripts the parser owns, each tagged `l<c>e<c>f<c>`
— loaded, errored, fired its load event (i.e. executed). **`l1e0f0` is the interesting one**: the
bundle arrived, nothing failed, and it never ran. Do not resurrect the old `/r` flag
(`readyToBeParserExecuted()`): WebCore sets it only for an *inline* parser-inserted script waiting on
stylesheets, never for a `<script src=...>`, so it reads 0 for every external bundle whether or not it
ran — which sent the hh.ru investigation after a phantom. `Doc/DEFERRED-SCRIPTS.md` has the whole
story.

For a stalled load, do not read the trace by hand:

```powershell
pwsh -File Src\tools\analyze-loader-trace.ps1
```

It pairs the scheduler's `loader: +<ptr>` / `-<ptr>` lines, names any request that started and never
retired, cross-checks the diag `res:` list for resources WebCore waits on that were never started, and
prints which of the three candidate causes the evidence points at. Two traps are baked into it: MSVC's
`%p` prints **no** `0x` prefix (a pattern expecting one silently reports a clean run), and loader
addresses are recycled, so the pairing has to count per address rather than test set membership.

**And a third trap, found 2026-09-18 on `.99`/`.100` and closed in `.102`.** `portLoaderTrace`
(`PortPlatformStrategies.cpp`) used `fopen_s(&file, path, "a")` while the driver's tracer appended to
the *same* `port-trace.txt` with `fopen(path, "ab")` — and on `.99`/`.100` the port's writes silently
produced nothing while the driver's landed normally. The file held ~1700 lines from six translation
units and **zero** `loader:` lines, in a session where Hacker News plainly loaded five subresources
(`Cached`, `nonwhite=587134/710656`). The script then printed `verdict: the scheduler is exonerated` —
every figure a zero read from an empty input, and that verdict was believed once. `.102` mirrors the
proven function and adds a one-shot `PT: sink ok path=…` / `PT: SINK FAILED … errno=…` report, so a
silent sink names itself. **Which of the three changed details mattered is not isolated** — the sink
works, the mechanism is unproven.

Two guards now make that failure mode non-silent, and both were verified against the real empty `.100`
trace: the script exits 2 with "cannot conclude" when **neither** `port-trace.txt` nor
`gpuinit-steps.txt` has loader traffic, and it refuses the final verdict when `MARK` lines are present
but the `+`/`-` pairs are not (the pairs are the bulk channel and stay file-only, so this combination
is reachable). It reads both files. `portLoaderTraceBoth` mirrors low-volume loader events into
`gpuLogMarkerF` (`gpuinit-steps.txt`, immediate flush) — that channel is the one that has survived
every crash and hang here, and it is where a new loader observation should go first.

**In the diag's `res:[name(sN)]`, N is `CachedResource::Status`, not a stage number**: `s0` Unknown,
`s1` Pending, `s2` Cached (the resource **loaded**), `s3` LoadError, `s4` DecodeError. Reading `s3`
as "stage 3, never got there" is how a confirmed-loaded resource gets described as a failed one.

**`lasterr=` carries `phase=` and `type=`, and an empty-looking error is not "no error".** The old
string read `curlcode=0 domain= desc= url=` and was taken to mean "nothing failed, so no request was
made". It means neither: a *failure dispatch fired*, carrying a `ResourceError` of type `Null` —
empty because this port's own `LoaderStrategy` error factories (`cancelledError`, `blockedError`,
`cannotShowURLError`, `interruptedForPolicyChangeError`, …) all `return { }`, and `Null` is exactly
what `ResourceErrorBase::isNull()` tests for, i.e. the value other WebCore code reads as "no error".
`type=Null` means the load was refused or cancelled before it left; `type=Curl` means the transfer
failed. `phase=` names which of the three fail dispatches recorded it — necessary because
`g_lastNetError` holds only the **last** failure of the session and `dispatchDidFailLoading` fires
for subresources, so a failed favicon silently overwrites the navigation error being investigated.

**`type=PortBudget` means curl did not fail — *this port* gave up, and the transfer was thrown away.**
`WebCoreSessionLoad`'s top-level fetch runs on its own thread (`ApoFetchChannel`) and the engine waits
on `ft.cvDone`. Until `0.1.10.0` that wait was a hardcoded **2 500 ms**; it is now a **progress-aware
stall detector** — 2 500 ms, re-armed while the byte counter advances, ceiling **9 500 ms** (just above
the worker's own 8 s, so the worker's curl error is preferred to the consumer's guess). Measured on
dzen.ru 2026-09-18: a ~1 MB article was reported failed (`rc=-16`) and the *same* transfer then finished
`size=1016600 http=200 crc=0`. Until `0.1.10.0` this was filed as `type=Curl curlcode=28` and read as a
network fault — twice. It now carries `type=PortBudget curlcode=0 domain=port-budget`, and the worker
appends `SL: DISCARDED -- … no second request was made. If this line is present, the error page was
wrong.` so the two halves of the story sit next to each other. The dead-radio protection is not weaker,
it is delegated: the worker's own `CURLOPT_LOW_SPEED_LIMIT/TIME` and `XFERINFO` callback end a genuine
stall. Do **not** change the budget or the ceiling on your own — and know that the harness's wedge
watchdog used to be calibrated against this number by hand ("6 x 700 ms = 4.2 s, comfortably past the
2.5 s fetch ceiling"); it now asks `WebCoreGetFetchProgress()` instead, so **any future change here has
to keep those two in step**. `Doc/TOPLEVEL-FETCH-BUDGET.md` §5 for the reasoning, §7 for the
measurements, §9 for the second half of that coupling (below).

**`beat-stuck` / `WEDGE` / `beat-gauge` in `log.txt` is the harness's wedge watchdog, and a `WEDGE` line is
not by itself a finding — read its companions.** The watchdog dumps the engine stack when *nothing*
moved for N beats, where N depends on what the engine was doing, because the same beat count means
opposite things in different jobs: a top-level fetch in flight (16 beats — the engine is *supposed* to be
blocked, and the port ends that wait at its own 9.5 s ceiling), `job=nav-load` (30 beats — the ceiling
plus `pumpLoop`'s own 5 s watchdog plus the tail; a healthy 3.6 MB page measures ~7 s inside that one
job), and anything else (6 beats, unchanged). "Nothing moved" now means **three** things at once: the
completed-job counter `finished`, the in-flight fetch's byte/hop counter, and
**`WebCoreGetEngineActivity()`** — a monotonic gauge of work the engine thread has visibly done (load-job
stage boundaries, every 256 KB `DocumentWriter` chunk, every settle tick of the load pump, the live
tick's step index). Its absence was worth exactly what §7b's stale number cost: two cold dzen.ru
navigations were dumped as `WEDGE`s and loaded fine 1.9–2.6 s later, because `finished` counts *jobs* and
the whole load is one job. When only the gauge saves a dump, the run says so —
`beat-gauge: … engine activity moved … -- NOT a wedge, no dump`. **A `WEDGE` with `inflight=0
engineact=<frozen>` is the real thing; a large `engineact` delta across the stuck window is the fix
working.** `engineact=` and `inflight=` are on every `beat-stuck` line for that reason.
`Doc/TOPLEVEL-FETCH-BUDGET.md` §9. Two related traps in the same area: `fetchprog=-1` means *only* "no
top-level fetch in flight" — the sign is the contract, and a `-1` sentinel in either counter used to fold
into a large negative number and invert it; and the diag's `loads=S…/R…/C…/F…` field is **dead**
(`WebCorePortBumpLoad` has no callers at all), so it reads `S0/R0/C0/F0` on a page that loaded fifty
resources — never read it as "no resources were requested".

**`loader: CALL type=N main=M` — N is a `CachedResource::Type` and M is `frame.isMainFrame()`.** In
*this* tree the enumerator is `0` `MainResource`, `1` `ImageResource`, `2` `JSON`, `3` `CSSStyleSheet`,
`4` `Script`, `5` `FontResource`, `6` `SVGFontResource`, `7` `MediaResource` … — read it from
`WebKit\Source\WebCore\loader\cache\CachedResource.h`, because a legend here goes stale in silence.
The numbers previously printed in this file (`2` = image, `4` = stylesheet, `5` = script, `6` = font)
were upstream's ordering *before* `JSON` was inserted, were off by one from `CSSStyleSheet` onward, and
were wrong for a whole investigation: measured on 2026-09-24, `.css` arrives as `type=3` and `.js` as
`type=4`. The cheap cross-check needs no header at all — the port prints the URL's **leaf** on the
paired `+` line, so the extension names the type.
`PortLoaderStrategy::loadResource` is **not** a subresource-only hook: `CachedResource::load()` is
the only async caller of `LoaderStrategy::loadResource()`, `DocumentLoader::loadMainResource()`
reaches it through `CachedResourceLoader::requestMainResource()`, and upstream's
`WebResourceLoadScheduler` answers every type with `SubresourceLoader::create`. A page-initiated
navigation and an `<img>` on that page therefore arrive at the same hook — the label is what tells
them apart, and its absence sent one investigation to the wrong layer.

**`NAV:` markers in `gpuinit-steps.txt` trace a navigation the page started** (`0.1.9.103+`, written
from `LoadingFrameLoaderClient`'s own `FrameLoaderClient` callbacks, so **no upstream WebCore edit is
involved** and the diagnostic is portable to ARM32 unchanged). Read them as a chain:
`policy navigation` → `docloader create` → `provisional started` / `start provisional` →
`main request` → `policy response` → `COMMIT` / `committedLoad bytes=`. `create` with no `COMMIT` is
a request that went out and never came back; no `create` at all is a navigation that never started.
Every `docloader create` after the first is engine-initiated — the harness path feeds bytes through
`DocumentWriter` by hand and never creates a `DocumentLoader` for its own navigation.

**`storage:` lines in `gpuinit-steps.txt` say whether Web Storage is alive, and they are the first thing to
check when a page loops, mis-syncs or has dead controls.** `PortStorage.cpp` writes
`storage: provider installed`, `storage: namespace local|third-party|session`, `storage: set origin=… key=…
len=… writes=…` (first 8 writes of an area, then every 500th, so a looping page cannot fill the disk),
`storage: del origin=…` and `storage: QUOTA …`. **Their absence is the finding:** with no
`StorageNamespaceProvider` installed, `setItem` succeeds and stores nothing and *no line appears at all*,
which is exactly how the dzen.ru freeze hid for weeks. A page that plainly uses `localStorage` and leaves
no `storage: set` line is a page writing into `EmptyStorageArea`.

**A tap is diagnosed from `[HIT]` markers in `gpuinit-steps.txt`, and `settled:` alone does not answer
"did the page see it".** Three of them, written by `WebCoreDriver`'s click path, in order:
`[HIT] x,y -> <tag> id=[…] class=[…] href=[…]` names the element under the point *before* dispatch (a
tap-sized miss and a dead control look identical from outside); `[HIT] settled: press=N release=N
connected=N` is WebCore's own opinion — `connected=0` means the page's script pulled the element out of
the DOM, i.e. the control still on screen is a *presentation* problem, not a dead handler; and
`[HIT] dom rc=… …` is the verdict from the page itself. The last one installs a capture-phase listener
set on `document` (mousedown/mouseup/click, recording `target|tag|a=href|dp=defaultPrevented|tr=isTrusted|btn|path=`),
dispatches, then reads the log back and clears it in place. It separates the three causes `settled`
cannot — no DOM `click` ever existed (so the anchor's default action never had a turn), a click fired
with nothing listening, or the page called `preventDefault()` (`dp=1`). `rc` is the eval result;
`[HIT] dom-probe rc=… installed|already` is logged on every tap on purpose: `installed` means the
document was replaced since the previous tap, which by itself answers "did the engine navigate on its
own". Capture phase at the document, so an author `stopPropagation()` cannot hide the event from the
record. `Doc/DZEN-SCROLL-DEATH.md` and `harness-tap-dispatcher-swallows-spa-taps` in memory have the
findings this produced.

**`ofd:` and `fset:` lines in `glyph.log` are the font-description trace, and they are armed by a file.**
They exist because a null family in a `FontCascadeDescription` is a null dereference inside a hash
lookup, with every guard on the path being an `ASSERT` that release builds compile out (the habr.com AV
of 2026-09-18; `Doc/FONT-NULL-FAMILY-CRASH.md`). `FontCascadeFonts.cpp`'s
`opportunisticallyStartFontDataURLLoading` writes `ofd: NULL family i=N/M specified=D size=S` and
`CSSFontFaceSet::fontFace` writes `fset: REFUSED null family slope=… width=… weight=…`, both through
`apotheosisWebTrace` — the WTF bridge also used by `portDiagLog`. **Armed by creating
`LocalState\texttrace.txt`**, not by an environment variable: `WebCoreSetGpuInitLogFile` checks for that
file beside the log it is handed and calls `_putenv_s("APO_TRACE_TEXT", "1")` itself, because
`x64-cycle.ps1` launches through `Start-Process "shell:AppsFolder\…"` — the app is activated by the
shell and never sees the PowerShell session's environment. It has to be armed before the **first
navigation** (`apotheosisWebTrace` caches the flag on its first call), and it takes effect only on the
next launch. Cost measured: ~150 000 lines/minute, 20 MB in three minutes of a habr.com load, because
`gdv`/`gdc`/`brk` are per-glyph; remove the file when done. `ofd:` and `fset:` themselves fire only when
a null family actually appears. Per-occurrence, not throttled. `ofd:` is the more informative of the
two — it is the only place that still knows *which* description carried the null.
**Generalised:** the bench cannot pass an environment variable to the app it launches. Anything a run
must be told arrives as a file in LocalState (`nav.txt`, `jstack.txt`, `texttrace.txt`) or ships inside
the appx.

**`empty page: notice shown …` in `log.txt` is the harness saying the load succeeded and painted
nothing** — and it is the one diagnostic here the *user* also sees. `DiagLooksEmpty`
(`MainPage.xaml.cpp`) reads `body=` / `bodyKids=` / `nonwhite=` out of the diag string; when `rc=0` and
nothing was painted, the harness builds a panel in code (no XAML edit, so `MainPage.g.hpp` and
`verify-xaml-connect.ps1` stay untouched) that names **both** the URL that was requested and the one the
engine ended up on, with Reload / "Try desktop mode" buttons that retry *the requested address* —
reloading where a redirect left you can only reproduce the blank window. dzen.ru is the case that
produced it: `https://dzen.ru/` → `https://sso.dzen.ru/install?uuid=…`, `body=0`, `nonwhite=0`. A
`notice hidden` line means a following page painted. English-only by design: it is a diagnostic surface,
and the three-language string table is not worth the churn for it.

**A failed navigation shows the *error page*, and until `0.1.10.56` the user could do nothing with it.**
Do not read "no error page appeared" out of `title=<the previous site>` on the `nav: FAIL` line: the
title is read and printed **before** `MakeErrorHtml` is rendered, so it names the old document by
construction. The branch is sound — `WebCoreRenderHtml(MakeErrorHtml(...))` returns **0**, so
`ok = (rc == 0)` is true and the error page is blitted like any successful load, confirmed by the new
`error page: rendered url=… rc=0 blit=yes` line (`WE-job:post-try rc=0` alone cannot say it; the success
path writes that too). The defect was that the page was **inert**: the failure path sets
`sessionActive = false`, so `m_sessionActive` goes false and `HandleTapAt` uses its extracted-link-table
branch instead of `ForwardClickToEngine`, with every scroll/pinch handler early-returning — and
`MakeErrorHtml` emitted **no** `<a href>`, while `extractLinks` publishes only anchors whose protocol
`protocolIsInHTTPFamily()` and whose `boundingClientRect()` is non-zero. So the table was empty and a
full-screen "Could not load the page" answered no tap and no swipe. `.56` adds a "Try again" anchor to
the **failed** url (not the current document's — the engine is still on the previous page) and
`HtmlEscape`s both interpolated strings: `err` embeds the requested url, and `?a=1&b=2` was being parsed
as an entity reference. Verified end to end through the finger path with `taplinkstr:`.
**The design behind it is still worth knowing:** `WebCoreRenderHtml` builds a **throwaway `Page`**
(scripting and compositing off) and returns pixels, so the resident session keeps the previous document
and `diag:` / `GetTitle` / `GetFrameHash` keep describing a page that is **not on screen**. `about:home`
rides the same design and works; making the error page live means one document and one session, i.e. a
new export in **both** `WebCoreDriver.h` copies. PLAN item **0q**.
Reachable reproduer: `https://habr.com/ru/feed/` when it is unreachable — and its
`type=Curl curlcode=28 desc=Connection timed out after 3006 milliseconds` is the port's own deliberate
3 s `CURLOPT_CONNECTTIMEOUT`, **not a defect and not to be raised**.

**A debugger cannot walk into JavaScript here, so use the probe instead.** The LLInt is
offlineasm-generated code with no unwind info past its entry, which is why every `dps` sample of a JS
freeze in this project bottoms out in C++ (WebCore event dispatch, microtask checkpoints) and never
names a script. `apoWalkJsStack` (`Src\port\WebCoreDriver.cpp`) does name it: it walks
`vm.topCallFrame` with `JSC::StackVisitor::visit` and writes `<tag> #N url:line:col :: functionName`
lines through `gpuLogMarker`. It is called from the watchdog callback when a runaway is stopped, and —
for proving the mechanism on a healthy page — from `PortChromeClient::addMessageToConsole` when
`LocalState\jstack.txt` exists. `VM::topCallFrame` is live exactly while JS executes (the LLInt's
`doVMEntry` sets it on entry and restores it on exit), so an empty `topCallFrame` means the caller had
no JS on the stack, not that the probe is broken. `Frame::hasLineAndColumnInfo()` is **not exported**
from `JavaScriptCore.dll`; its body is `return !!codeBlock();` and that is what the probe uses.
`Doc/DZEN-SCROLL-DEATH.md` §10e has the verified output.

## Device prerequisites

The phone must be on, on the same WiFi, with **Device Portal** enabled (Settings → For developers).
The appx depends on `Microsoft.VCLibs.140.00 (ARM)`, which other -Reborn apps have usually already
installed. HTTPS needs help: an App Container has no system certificate store, so `cacert.pem` is
packaged and injected at startup via `WebCoreSetCACertBlob` (or `WebCoreSetCACertPath`) — curl and
OpenSSL then bring their own TLS 1.3, whereas the OS's Schannel path stops at 1.2.

**Crash-dump collection is per-package and every reinstall resets it.** `deploy-launch.ps1` now arms it
right after install, so a deployed build is always ready to leave a dump; if you install by any other
route, run `Src\tools\Wdp-Crash.ps1 -Enable` before reproducing, or the crash yields a
`CRASHVERDICT: CRASHED` line and no stack. Also note that launching over WDP with the screen off is not
a fair test: an app without foreground focus gets suspended and then reaped, which looks like a crash in
the process list but writes `clean-exit`. Reproduce from the tile or app list with the screen on.

## Where the rest lives

- **Claude Code project memory** (`MEMORY.md` index plus one file per fact) — per-milestone root
  causes, dead ends, real-device data points, and the upstream WebKit patch list. Scan `MEMORY.md`
  before starting work.
- **`Doc/UNIFICATION.md`** — the open plan for collapsing the two build lines into one: the dead
  3.49 GB `WebCoreFull.lib`, the duplicated static Cairo, `/FORCE:MULTIPLE`, and the two Windows SDKs.
  Carries the measurements behind each claim, so read it before re-deriving any of them.
- **`Doc/ARM32-DANGLING-SECURITYORIGIN.md`** — the ARM32 launch crash: `SecurityOrigin::protocol()` and
  `::host()` returned references to temporaries, which only ARM32 punishes. Carries the symbolised
  stack, the dump-and-cdb recipe that found it, and two documented false trails — including why a probe
  that faults or alters what it measures is worse than no probe.
- **`Doc/HARFBUZZ-ICU-DIVERGENCE.md`** — the two build lines used to shape text differently: x64 had
  `USE_HARFBUZZ` off and ICU 75.1 against ARM32's HarfBuzz and ICU 78.3. **Closed on 2026-08-22:** both
  lines now take ICU 78.3 and harfbuzz-with-ICU from vcpkg, `USE_HARFBUZZ` is 1 on both, and
  `C:\icu-x64-uwp` is referenced by no script. Read the document anyway before touching fonts: it holds
  the six causal links that produced the split, the four latent gaps that had to be fixed to close it
  (a from-scratch CMake configure had never been run on this line), and the divergences that
  remain — **not** JIT-on-device versus interpreter-on-bench: that one was closed on 2026-09-18, both
  lines build the same tiers, and the difference that did exist was a runtime switch in the packaged
  `navseq.txt`. See the hard constraint above.
- **`Doc/STUB-AUDIT.md`** — the `stubs-*` inventory: which files are compiled into `WebCore.dll`, which into
  the driver archive, and which by *nothing*; a three-way classification (HONEST FAILURE / SILENT LIE /
  NEVER REACHED); and the capability gaps that hid under a filename beginning with `stubs` — font
  selection, screen metrics, webfonts, clipboard, modifier keys, accessibility. Read it before estimating
  any work on "missing features", and before believing a `stubs-*` header comment: `stubs-crypto.cpp`
  describes a `CryptoDigest` it does not define, `stubs-loader.cpp` is not a stub at all, and
  `webcore-driver-stubs.cpp` is 83 % commented-out.
- **`Doc/UNKNOWN-EXPORTS.md`** — symbols that look available from the port layer and are not, with the
  `llvm-nm` check and the known replacements. Read it before referencing any WebCore accessor.
- **`Doc/CALC-HANDLE-DANGLING.md`** — the engine-thread AV reading `0xF`/`0x10`: the stale
  `Calculation::ValueMap` handle, the symbolised stack, the offline symbolisation recipe that works
  where `llvm-symbolizer` and `cdb` do not, and the two-builds-in-one-log-directory trap.
- **`Doc/DEFERRED-SCRIPTS.md`** — why hh.ru never finishes loading and why its buttons are dead: one
  root cause, with the `[HIT]` measurements, the field semantics, and the loader path to look at.
- **`Doc/FONT-NULL-FAMILY-CRASH.md`** — the habr.com AV reading `0x10`: a null `AtomString` family,
  hashed through `CSSFontFaceSet::fontFace` with every guard on the path compiled out, produced by
  `stubs-other.cpp`'s `platformSystemFontShorthandInfo`. Read it before touching any font stub — it
  carries the symbolized chain, the address-arithmetic trap (log address = **load**, symbolizer wants
  **preferred**), the four isolating test pages that closed the obvious reproducers, and why the exact
  installing call site was still open when it was written.
- **`Doc/TOPLEVEL-FETCH-BUDGET.md`** — the top-level fetch wait: why a correctly fetched page was shown
  as an error, why the error was read as a network fault twice, why the fixed budget must not simply be
  raised, the progress-aware stall detector that replaced it (`0.1.10.0`), and the wedge watchdog in the
  harness that had been calibrated against the old number. Read §3 before touching that wait. **§8 is the
  second defect, found while verifying the first**: `dlnow` counts body bytes, so a whole 302 chain reads
  as "no progress" and the worker's own `XFERINFO` callback was aborting healthy hops with `crc=42`;
  hop-aware progress, a report-only callback and `fetchprog = dl + (hops << 40)` are the fix, measured as
  `redirects=3 cookies=11 size=3459216 http=200 crc=0` on dzen.ru where it used to end at hop 1. **§9 is
  the third, in the watchdog itself**: `finished` counts completed jobs, so one `nav-load` job (3.6 MB
  page, ~7 s) is invisible to it — two healthy dzen.ru loads were dumped as `WEDGE`s. Fixed by
  `WebCoreGetEngineActivity()`, a chunked feed, a `beat-gauge` line for the dumps the gauge prevents, and
  a per-job threshold derived from the port's own bounds.
- **`Doc/GPU-LIVENESS.md`** — the harness's EGL probe terminated the engine's own display, which is why no
  `-Gpu` bench run before `0.1.10.13` reached the GPU present path. Carries the before/after counters
  (228/228 `eglMakeCurrent FAILED` → 0; 0 → 178 `tm:` paints), why the per-url retry multiplied it, and
  the two rules that followed: a probe that can change what it measures is not a probe, and grep all three
  layers.
- **`Doc/BITMAPTEXTURE-TEARDOWN-AV.md`** — the `~BitmapTexture` AV at RVA `0x418C2`: a garbage
  `RefPtr<const FilterOperation>` at `this+0x4E0`, proven by the refcount-deref code shape *and* by
  `ClipStack` layout arithmetic; the leading hypothesis (pool bookkeeping left inconsistent by the dead
  display), the registers to add to the harness dump if it recurs, the `llvm-symbolizer --obj=` recipe,
  and why the `CaptureStackBackTrace` chain from that record is junk.
- **`Doc/TAP-DISPATCH.md`** — who decides where a tap goes: the three-row measurement that showed the
  frame-hash gate navigating against a page's explicit `preventDefault()`, why `wasHandled()` cannot
  discriminate (measured, not assumed), the `refused` semantics (1/0/−1), how to read a `TapDone` line, and
  (§8) the URL term's correction on `0.1.10.18` with its four-tap measurement — including the honest
  ceiling: no branch-level reproduction of the reported "the page only flinched" was produced.
- **`Doc/DZEN-FIRST-LOAD-DETOUR.md`** — why `dzen.ru/` can render an *empty* document on its first load
  and the full page on the second: the top-level fetch's redirect chain ends on a Yandex autologin push
  page, that page navigates to `sso.dzen.ru/install`, and the install page's own CSP refuses its recovery
  navigation back to dzen. Carries the chain verbatim, the proof that **form POST works** (same URL: `405`
  for POST vs `404` for GET), the check that the CSP refusal is upstream-correct rather than a port policy
  bug, and the address-bar consequence of a page-initiated navigation.
- **`Doc/DESTROYED-STRINGS.md`** — the `?` (`0x3F`) bytes: which literals were destroyed rather than
  mis-encoded, why this is *not* a file-encoding problem (the same files carry working Cyrillic), what
  was rewritten, and the localization gap that remains in the built-in home and error pages.
- **`Doc/`** — the engineering archive: `ARM32-BUILD-GUIDE.md`, `ARM32-RECOVERY.md`,
  `X64-AS-ARM-EMULATOR.md`, `WEBKIT-UPGRADE.md`, `PLAN.md`, `Architect*.md`, crash triage notes, and
  `CLAUDE-legacy-2026-08.md` / `AGENTS-legacy-2026-08.md`, the verbatim previous versions of these
  instruction files.
- **`README.md` / `README-RU.md` / `README-CN.md`** — the user-facing description.

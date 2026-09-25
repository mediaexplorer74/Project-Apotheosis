# Real-site crash triage — playbook

**Written 2026-08-11, engine state = appx 0.1.8.33, x64-uwp dev line (`build-x64-gpu`).**

This file is for whoever picks the work up next — a human, or an AI model with less context than
the session that wrote it. It answers one question: **when the browser dies (or hangs, or paints
blank) on a real internet site, what do I do next?** Read §0 and §1 before touching anything.

Everything here is x64 (`build-x64-gpu`, Win11 AppContainer as the "emulator"). ARM32 / Lumia 950
is deferred — but see §7, because at least one of these bugs is almost certainly waiting there too.

---

## 0. State of the world (what is proven, what is not)

Proven working on x64, in this order (each verified by a log artifact, not by inference):

1. `LoadPackagedLibrary(JavaScriptCore.dll)` + `(WebCore.dll)` succeed inside the AppContainer.
2. JIT probe runs (`jitresult.txt`, 366 bytes).
3. The real XAML tree connects — `XAML: LoadComponent OK - real UI tree connected`.
4. Fonts resolve; text paints (see `Summary.md` §10).
5. **`pumpLoop` completes.** This is new as of 2026-08-11 and it is the big one. `port-trace.txt`
   now shows `twdct probe: compare ok` → `pump: timers armed, entering RunLoop::run()` → 16
   settle ticks → `pump: RunLoop::run() returned`. Before the `std::partial_ordering` fix
   (`Summary.md`, section "every real page load crashed") this had **never once** happened.

Fixed **and verified** 2026-08-12:

6. `evalJS` null-deref — `probeSpaModule` → `evalJS` → `JSValue::toWTFString()` on an **empty**
   JSValue. Guarded now (`Src/port/WebCoreDriver.cpp`, `if (!result) return kErrNoDocument;`).
   Verified: appx 0.1.8.33 logs `spa: probe rc=-6 kick=''`, no `UEF:` block, process still alive.
   See §3.1 for the signature, because this class of bug will recur.
7. **The network works — TLS included.** `http://example.com` → `rc=0`, `title=Example Domain`,
   fully painted. Then, with no further code change, `https://news.ycombinator.com` → `rc=0`,
   `title=Hacker News`, `contents=720x2012`, subresources enumerated
   (`res:[s.gif(s0) y18.svg(s0) news.css?…(s1)]`). So curl, DNS, sockets, the AppContainer's
   outbound capability and the packaged `cacert.pem` blob are **all fine**, and rungs 2–5 of §2
   all *load*. §5.1 was the top-ranked hypothesis in the first draft of this file and it was
   simply **wrong**; do not spend a cycle there.

Fixed 2026-08-12 but **not yet verified** (the appx was compiling when the session ended). These
three are the reason a real site showed *an empty window with only the scroll arrows* — see §3.3,
which is the failure the user actually reported, and it is **not** a crash:

8. **JavaScript was never enabled on the session path.** `buildSession` never called
   `setScriptEnabled(true)`; WebCore's default is `false`.
9. **The pump stopped while WebCore was still refusing to paint.** The settle loop now also waits
   on `Document::visualUpdatesAllowed()`.
10. **The cairo surface was handed to the harness transparent, not white.**

---

## 1. The triage loop (do exactly this)

One command does version-bump → driver relink → appx build → reinstall → seed `LocalState` →
launch → tail the four log files:

```bash
pwsh -File Src\tools\x64-cycle.ps1 -Url http://example.com -WaitSec 60
```

`-Url test` uses the local probe page it seeds itself. `-SkipDriver -SkipBuild` re-runs the last
appx against a new URL in ~20 seconds — **use that** when you only want to change the URL.

Then read, in this order, from
`%LOCALAPPDATA%\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState\`:

| file | what it tells you |
|---|---|
| `port-trace.txt` | **the engine's own narration.** Last line = how far the engine got. |
| `log.txt` | harness side: module bases, fonts, `NavigateTo`, and the UEF crash dump (code, address, 15 frames) |
| `stage.txt` | coarse `before-load` / `after-load` marker |
| `glyph.log` | per-codepoint glyph hits/misses (font fallback problems only) |

**The single most useful line in the whole system** is the last line of `port-trace.txt`. Add a
`WebCorePortTrace("…")` wherever you need a narrower bracket; it appends and flushes per line, so
it survives a hard crash. `DBG_STAGE` does **not** — it only reaches `OutputDebugStringA`, which
nothing inside the AppContainer can observe. Don't waste a build cycle on `DBG_STAGE`.

### Interpreting the diag string

`writeDiag` (`WebCoreDriver.cpp`) builds `g_lastDiag`, which the harness logs. Fields worth
knowing when a site renders blank rather than crashing:

- `loads=S<n>/R<n>/C<n>/F<n>` — load started / got a response / completed / failed. **These read
  `S0/R0/C0/F0` even on a fully successful load** (observed on both example.com and HN), so the
  counters are not wired on this path. Do not read anything into them; use `title=` and `res:[…]`.
- `js=<enabled>/<canExec>` — `1/1` is healthy. The second digit is a *consequence* of the first:
  `canExecuteScripts()` returns false whenever `scriptEnabled` is false, so `0/0` means
  `setScriptEnabled` was never called (§0.8) and tells you nothing about sandbox flags. `1/0`
  is the genuine sandbox case (see the `effectiveSandboxFlags = { }` note in `buildSession`).
- `vua=<0|1>` — `Document::visualUpdatesAllowed()`. **`vua=0` is the definitive "blank page"
  tell.** It means WebCore refused to paint *anything* (§3.3), so the surface holds only the white
  pre-fill. `sheets=<0|1>` is `haveStylesheetsLoaded()`; `vua=0 sheets=0` = a render-blocking
  stylesheet is still outstanding.
- `nonwhite=<n>/<total>` — count of pixels that are not pure white. **Read this carefully.** Since
  the white pre-fill landed (§0.10) an unpainted surface reads `0`, the same as a legitimately
  white page — use `vua=` to tell them apart. In logs from *before* that change the metric is
  inverted from the obvious reading: transparent black counts as non-white, so `nonwhite=777600/777600`
  ("100 % painted!") actually meant **nothing was drawn at all**, and `nonwhite=0/777600` meant a
  white background *was* painted. That inversion cost this project an evening.
- `pending=<n>` — subresources still in flight when we stopped pumping. Note that `pending>0` is
  **not** by itself a rendering problem: images do not block painting, only render-blocking
  stylesheets do, and those show up as `vua=0`.
- `spa=[…]` — the module-import probe result: `no-mod`, `kicked`, `eval-ok rootCh=<n>`, or
  `EVAL-ERR:…`. With the §0.6 fix, an empty value now means "the probe never ran".

---

## 2. The URL ladder — climb it in this order, do not skip

Each rung adds exactly one new subsystem. Skipping means you will not know which one broke.

| rung | URL | first thing it exercises |
|---|---|---|
| 1 | `-Url test` (local `file:///`) | nothing new — regression check |
| 2 | `http://example.com` | **curl, DNS, sockets through the AppContainer** — no TLS, no JS, ~1 KB of HTML |
| 3 | `http://info.cern.ch/hypertext/WWW/TheProject.html` | plain HTML with real links and relative URLs |
| 4 | `https://example.com` | **TLS** — the `cacert.pem` blob path (§5.1) |
| 5 | `https://news.ycombinator.com` | tables, a real stylesheet, many `<a>` (hit-test + `extractLinks`) |
| 6 | `https://en.m.wikipedia.org/wiki/Windows_Phone` | large DOM, images, web fonts |
| 7 | anything React/Vue | `<script type=module>` → `probeSpaModule` → dynamic `import()` |

**Results as of 2026-08-12** (all on x64, software rendering, `g_gpuActive=false`):

| rung | load | painted | note |
|---|---|---|---|
| 1 local `test.html` | ✅ `rc=0` | ✅ `nonwhite=39657/777600`, Cyrillic included | user-confirmed on screen |
| 2 `http://example.com` | ✅ `title=Example Domain` | ✅ | user-confirmed on screen |
| 3 `info.cern.ch` | not run | — | expected to pass; cheap, run it as a regression check |
| 4 `https://example.com` | not run separately | — | TLS proven by rung 5 instead |
| 5 `https://news.ycombinator.com` | ✅ `title=Hacker News`, `contents=720x2012` | ❌ **blank** — `vua=0` (§3.3) | fixes §0.8–0.10 target exactly this |
| 6 Wikipedia | not run | — | |
| 7 SPA (`dzen.ru` is the configured home page) | ✅ `rc=0` | ❌ blank white, `js=0/0`, `bodyKids=1` | needs §0.8; it renders nothing without JS |

The important lesson from the table: **"loads" and "paints" are two different milestones.** Every
rung already loads. Nothing after rung 2 has been shown to *paint*, and the cause was never the
network.

Record the last `port-trace.txt` line for each rung in `PLAN.md`. That table *is* the progress
report; without it the next session re-derives everything.

---

## 3. The two crash classes we have already met — learn their signatures

Both were invisible in the source and only fell out of a disassembly. Expect more of the same.

### 3.1 Empty `JSValue` treated as a cell (fixed 2026-08-11, class will recur)

Faulting instruction pattern:

```
movabsq $-0x1fffffffffffe, %rax   # 0xFFFE000000000002 == JSC::JSValue::NotCellMask
testq   %rax, %r14
jne     <not-a-cell>
cmpb    $0x2, 0x5(%r14)           # <-- AV reading address 0x5 ; 2 == JSType::StringType
```

`JSValue::isCell()` is `!(bits & NotCellMask)`, and the **empty** JSValue is all-zero bits — so it
answers *true*, `asCell()` hands back `nullptr`, and reading `JSCell::m_type` (offset 5) faults on
address `0x5`. **Rule: every JSValue coming out of `execute…IgnoringException` /
`evaluateInWorld` must be tested with `if (!result)` before any `toWTFString` / `isString` /
`getString`.** An AV on a tiny address (`0x5`, `0x8`, `0x10`) is almost always this shape: a real
member offset off a null base, not a wild pointer.

### 3.2 ABI split across translation units (fixed; see `Summary.md`)

`std::partial_ordering` returned by hidden-sret in one TU and in registers in another, with
*identical* mangled names, so the linker bound the mismatched pair silently. Tell: the faulting
address **climbed monotonically across runs** (it was a QueryPerformanceCounter tick sitting in a
stale register). **If an AV address trends upward over a session, suspect a clock value in a
register, not a wild pointer.** The fix is an `ALWAYS_INLINE` definition under
`#if defined(WK_WINUWP)`; `ALWAYS_INLINE` rather than plain `inline` is load-bearing, because a
merely-inline definition still emits a COMDAT under the same mangled name.

The invariant to re-check after any WTF rebuild — this must print nothing:

```bash
cd build-x64-gpu && grep -rlF '??__MWTF@@YA?AUpartial_ordering@std@@AEBVTimeWithDynamicClockType@0@0@Z' --include='*.obj' .
```

and `llvm-readobj --coff-imports bin/WebCore.dll` must not mention it either.

### 3.3 The page loads but the window is empty (diagnosed 2026-08-12 — **not a crash**)

**Symptom the user sees:** navigate to a real site; the window is blank, with two large up/down
arrows at the bottom right and nothing else. Those arrows are `ScrollFab` in
`MainPage.xaml.cpp` — they become visible when `sessionActive` is true, i.e. **they are proof that
the navigation succeeded.** They are not a scrollbar and not a rendering artifact. Do not chase them.

There were three independent causes, all in `Src/port/WebCoreDriver.cpp`:

1. **JavaScript was off.** `Settings::scriptEnabled` defaults to `false` in WebCore — WebKitGTK's
   `WebKitWebView` turns it on for its clients, but we assemble the `Page` by hand, and only the
   one-shot `WebCoreLoadUrl` ever called `setScriptEnabled(true)`. The persistent session path that
   the harness actually uses (`buildSession`) never did. Diag showed `js=0/0`, and the second zero
   is a red herring: `canExecuteScripts()` reports false *because* `scriptEnabled` is false, which
   is why the earlier `effectiveSandboxFlags = { }` fix did not help. Any JS-driven site (dzen.ru,
   most of the modern web) therefore built a DOM and painted a blank white page.
2. **We stopped pumping before WebCore was willing to paint.** This is the subtle one.
   `Document` holds an `OptionSet<VisualUpdatesPreventedReason>` — `Client`, `ReadyState`,
   `Suspension`, `RenderBlocking`. While it is non-empty,
   `RenderLayer::shouldSuppressPaintingLayer()` (`Source/WebCore/rendering/RenderLayer.cpp`,
   `if (!layer->renderer().document().visualUpdatesAllowed()) return true;`) refuses to paint
   **every layer in the tree** — so you get an untouched surface, not a partially styled one.
   A render-blocking stylesheet still in flight is exactly that state. On HN, the main resource's
   `isLoadingInAPISense()` had already gone false while `news.css` was pending, so `pumpLoop`'s
   quiet counter reached 16 (~0.8 s) and stopped ~3 s before WebCore would have drawn anything.
   The reason clears when the sheet arrives *or* when `Document`'s suppression timer fires at
   `settings().incrementalRenderingSuppressionTimeoutInSeconds()` — whose upstream default, **5 s,
   is longer than our whole pump budget.** Fix: the settle loop now counts
   `!visualUpdatesAllowed()` as not-quiet, and `buildSession` lowers that timeout to 2 s so a
   stalled stylesheet costs the styling and never the page.
3. **The handed-over surface was transparent.** `cairo_image_surface_create` zero-fills =
   transparent black, and when painting is suppressed WebCore does not even lay down a base
   background, so the harness blitted a fully transparent bitmap and the white `ContentArea` showed
   through. `paintToRGBA` now pre-fills opaque white, which is a browser's base canvas anyway.

**Reusable lesson:** a blank window has three distinct explanations that look identical on screen —
*nothing loaded*, *nothing executed* (JS off), and *nothing painted* (visual updates prevented).
The diag string now separates them: `title=`/`res:[…]` for the first, `js=` for the second,
`vua=` for the third. Check `vua` **before** suspecting the renderer, Cairo, fonts, or the GPU.

**And a warning about `glyph.log`:** its `fill: MISS i=… cp=U+…` lines are *not* evidence that text
cannot render. The codepoints arrive in aligned runs of 16 (`U+6C30..U+6C3F`, `U+2120..U+212F`)
because a WebKit `GlyphPage` is 16 glyphs wide and the log prints the whole page — these are
ordinary fallback probes into CJK/symbol blocks that a Latin font legitimately lacks. Only misses
are logged (`stubs-other.cpp`, ~line 563), so **healthy Latin text produces no output at all** and
an empty-looking `glyph.log` is the good case. This file misled the 2026-08-12 session for a while.

---

## 4. Symbolizing a UEF stack (the whole reason the last two bugs got found)

`log.txt` gives `UEF: unhandled exception code=0x… at addr=… tid=…`, then 15 raw addresses.
`llvm-symbolizer` **cannot read this MSVC PDB** — it answers `??:0:0`. Do this instead:

```bash
cd Src/harness/x64/Release/Harness
# 1. RVA = addr - ModuleBase(Harness.exe)   (that base is logged at startup)
#    Frames in the 7FFExxxxxxxx range are OS DLLs = exception dispatch, ignore them.
# 2. which .obj owns the RVA:
llvm-pdbutil dump --section-contribs Harness.pdb > /tmp/sc.txt
llvm-pdbutil dump --modules         Harness.pdb > /tmp/mods.txt
# 3. nearest exported name (unreliable for `static` functions — no S_PUB32 record!):
llvm-pdbutil dump --publics Harness.pdb > /tmp/pub.txt
# 4. the actual answer — read the faulting instruction (ImageBase is 0x140000000):
llvm-objdump -d --no-show-raw-insn --start-address=0x140001500 --stop-address=0x1400015a8 Harness.exe
```

**`llvm-pdbutil` prints `section:offset` in DECIMAL** (`0001:540320`), not hex. Parsing it as hex
makes every lookup miss silently. `.text` VirtualAddress is `0x1000`, so `RVA = 0x1000 + offset`.

Step 2 is the high-value one: it tells you `WebCoreDriver.x64.obj` vs `MainPage.xaml.obj` vs a
system stub, which is usually enough to find the code by reading. Step 3 lies about `static`
functions — it silently attributes them to the preceding public symbol, so do not trust a name
unless the contribution's start address matches the symbol's.

---

## 5. Ranked hypotheses for what breaks on a real site

Ordered by (likelihood × cheapness to test). Rung numbers refer to §2.

### 5.1 + 5.2 TLS, certificates, sockets, `internetClient` — **RESOLVED, all working**
Kept here only so nobody re-ranks them at the top. `cacert.pem` is packaged and injected as a blob
at startup (`WebCoreSetCACertBlob`, `CACert loaded: 189462 bytes`); curl/OpenSSL bring their own
TLS 1.3 and the OS Schannel path is unused. Proven end-to-end by `https://news.ycombinator.com`
returning `title=Hacker News` with subresources enumerated. If a *future* site fails to load, the
field to read is `lasterr=[…]`: curl 60 = cert verify failed, 35 = TLS handshake, 6 = DNS. Note that
a loopback `http://` URL to the dev box is **not** a valid test — AppContainer blocks loopback
unless exempted, so a loopback failure is expected and proves nothing.

### 5.3 More empty-JSValue / null-cell derefs (§3.1) — **now the top-ranked risk**
This moves to the front because §0.8 turns JavaScript **on** for the first time on the session path.
Every real site now executes real JS through stubs that have never run it, so expect new crashes
here — and note that they are *progress*, not regression. `evalJS` is guarded; audit anything else
touching `JSC::JSValue` before believing a crash is elsewhere. The bracketing traces inside the
settle tick (`settle: isolatedUpdateRendering ok`, `settle: microtaskCheckpoint ok`) were kept
deliberately for exactly this: with JS on, `isolatedUpdateRendering` runs page script (rAF,
IntersectionObserver), so those two lines localize a crash to the JS side vs. the pump side.

### 5.4 The settle heuristic gives up too early — **partially fixed, re-read this before touching it**
`pumpLoop` stops after 16 consecutive "quiet" 50 ms ticks (~0.8 s) or a watchdog (4–30 s depending
on the call site). As of §0.9 a tick no longer counts as quiet while
`Document::visualUpdatesAllowed()` is false, which is the case that mattered (§3.3.2).
- **Still possible:** a *slow* subresource that does not block painting simply misses the frame. The
  symptom is a partial render with a large `pending=<n>` and `vua=1`.
- **Try:** raise the quiet threshold. Do **not** gate on `pending == 0` — HN shows images sitting at
  status 0 that may never start, so that would trade a partial render for a full watchdog stall on
  every page. And do **not** simply raise the watchdog: that trades a wrong render for a UI freeze.

### 5.5 Hit-test / `extractLinks` on link-dense pages
Known-parked defect: tap-to-link misses. HN (rung 5) has hundreds of anchors and will exercise
`extractLinks` far harder than the local probe page.

### 5.6 CJK / font fallback
Known-parked: CJK renders as tofu even though `simhei.ttf` (9.7 MB) loads. Cosmetic — do not let it
block the ladder. **Before using `glyph.log` as evidence for anything, read the warning at the end
of §3.3**: its `fill: MISS` runs are aligned 16-codepoint `GlyphPage` probes, only misses are
logged, and healthy Latin text produces no lines at all.

### 5.7 GPU compositing
Irrelevant until the ladder is climbed: `g_gpuActive` defaults **false**, so everything above runs
Cairo software rendering. Leave it that way while triaging. Turning it on adds ANGLE, EGL surface
marshalling, and a deadlock class (§6) all at once.

---

## 6. Rules that will cost you hours if you break them

- **`ninja` incremental builds in `build-x64-gpu` are permanently broken.** The tree was
  configured at a different path (`C:\Users\Admin\source\repos\!Vibe\…`), and `.ninja_deps` stores
  *absolute* header paths, so every pre-move object is eternally dirty — ninja will propose ~1528
  steps (~8.5 h at `-j1`) for a one-line header edit. Do **not** let it. Use the narrow-rebuild
  recipe in `PLAN.md` §10 (`ninja -t commands -s <target>`, reconstruct the link response file).
  Never `touch` an object file: ninja marks an output dirty when its mtime exceeds its recorded
  deps-log mtime, so a blanket `touch` makes things *worse*.
- **A cold compile of `WebCoreDriver.cpp` takes ~25 minutes** on this laptop — 2 cores, 3.9 GB
  RAM, ~200 MB free, so the compiler pages (11 % CPU, disk-bound). Batch driver edits; do not
  iterate one `WebCorePortTrace` at a time. Run nothing else while it compiles.
- **`-j1` only.** Parallel compiles will thrash the machine into uselessness.
- **Two copies of the C ABI header** — `Src/port/WebCoreDriver.h` and
  `Src/harness/WebCoreDriver.h`. Adding an export means editing **both**, or the ABI silently
  disagrees.
- **`MinimalTest=true` is a crash-isolation stub**, not a build flag. It compiles
  `MainPage.minimal.cpp`, drops `MainPage.xaml.cpp`, and links **no engine libs**. CLAUDE.md's
  documented harness command still shows it; ignore that. `x64-cycle.ps1` deliberately omits it.
- **The x64 driver scripts are `*-x64.ps1`.** `link-driver-gpu-arm32.ps1` / `compile-driver-gpu-arm32.ps1`
  are the ARM32 ones and hard-fail here.
- **Threading:** present only on the engine thread; the UI thread must never synchronously wait on
  the engine. ANGLE marshals surface create/resize back to the panel dispatcher, so a mutual wait
  deadlocks and `RunOnUIThread`'s timeout calls `std::terminate`.
- **`Frame::Navigate` is permanently banned** (0xc000027b). `App::OnLaunched` assigns
  `rootFrame->Content = ref new MainPage()` directly.
- Measure the appx with PowerShell `.Length`, never `ls -l` — Windows owner names contain spaces
  and shift the columns.
- Keep the directory junction `C:\Users\Admin\source\repos\!Vibe` → `C:\Users\media\source\repos\Vibe`.
  It is load-bearing for the build tree; deleting it resurrects the 8.5-hour rebuild.

---

## 7. When the Lumia 950 comes back (ARM32)

Do **not** re-validate ARM32 before the x64 ladder is climbed — every bug found on x64 is a bug you
would otherwise chase on a device with no debugger. Specifically expect:

- **The same `std::partial_ordering` ABI split** in `build-arm32-gpu`: same compiler, same
  headers, and AAPCS has the equivalent hidden-sret rule. Apply the `ALWAYS_INLINE` header fix and
  re-check the §3.2 invariant *before* deploying.
- `compile-driver-gpu-arm32.ps1` has a `-DBUILDING_WebCore` defect.
- The ARM32 harness link still drags in `WebCoreFull.lib` (3.2 GB) behind `/FORCE:MULTIPLE`; the
  x64 line removed it by exporting four compositing symbols via `webcore-exports.def`. Do the same
  there — it is both hygiene and reclaimed disk.

---

## 8. If you have one hour and want the most progress

Start here, because the three fixes of §0.8–0.10 were **compiled but never observed running**:

1. `pwsh -File Src\tools\x64-cycle.ps1 -Url https://news.ycombinator.com -WaitSec 60`.
   Read the diag line. The three things to check, in order:
   - `js=1/1` — JavaScript is on (§0.8). If it still says `0/0`, the driver did not relink.
   - `vua=1` — WebCore was willing to paint (§0.9). If `vua=0 sheets=0`, the stylesheet still had
     not arrived when the pump gave up; raise the suppression timeout or the quiet threshold.
   - `nonwhite=` — with the white pre-fill in place (§0.10) a real HN render should be a *middling*
     number (text and rules on a light background), not `0` and not the full pixel count.
2. If HN paints: run rung 3 and rung 6, then the configured home page `dzen.ru` (a pure SPA — the
   real test of §0.8). Write all of it into the §2 results table.
3. If HN crashes instead: that is expected progress, not regression — JS is executing for the first
   time. Symbolize it with §4, and check §3.1 first, since `JSC::JSValue` misuse is the known shape.
4. Whatever the last `port-trace.txt` line is, bracket it with two more `WebCorePortTrace` calls,
   pay the one 25-minute compile, and repeat. **One compile, several new traces** — that ratio is
   the whole game on this hardware.
5. Keep `Doc/PLAN.md` and `Doc/Summary.md` current. That is what makes the next session cheap.

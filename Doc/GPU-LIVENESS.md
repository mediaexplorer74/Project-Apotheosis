# The bench never ran the GPU path — the harness's own EGL probe was killing the engine's display

**Found and fixed 2026-09-19, x64 bench, build `0.1.10.13`.** One defect, in the harness, with two
faces: it made every `-Gpu` bench run validate the compositing code *without a live GL context*, and it
made the app fall back to the software path for a reason that had nothing to do with the GPU.

This document exists because the previous state of knowledge was wrong in a way that had already been
believed: `Doc/CLAUDE.md`'s own warning ("the seeded `settings.ini` says `gpudefault=0`, so the bench runs
with compositing OFF unless you pass `-Gpu`") invites the conclusion that `-Gpu` *does* mirror the device.
It did not. Passing `-Gpu` armed compositing; the GL context behind it was dead within one composite.

**This document has three faces, found a day apart, and the later two matter as much as the first.** §1–§3
and §6 are the EGL probe terminating the engine's display (fixed `0.1.10.13`). §4–§5 are the first
success — which was reached on `layertest.html`, a page *built* to have a root layer, and which therefore
did not generalise to real pages. **§7 is why it did not generalise**: the probe's retry key was the url
the harness *requested*, so a load that produced a different document than the one asked for could never
be probed (fixed `0.1.10.19`). Until §7, the bench could present through the GPU only on a purpose-made
page; the first real page did so on 2026-09-19 at 05:53.

## 1. The reading that gave it away

`LocalState\port-trace.txt`, run of 2026-09-18 23:58 (`0.1.10.12`):

```
rb: enter 1368x758
rb: makeCurrent=0 self=000002C13C0C5C40
glctx: eglMakeCurrent FAILED err=0x3001 surface=... context=...
rb: no live GL context -- falling back to Cairo
```

**228 of 228** `rb: enter` attempts in that session read exactly this, with `[GPU] readback failed --
falling back to Cairo` and `[GPU] present: no live GL context -- falling back` repeating to the end of
the log. `0x3001` is `EGL_NOT_INITIALIZED` — not a lost device, not a bad surface: the display itself
says it was never initialized, or was initialized and then terminated.

Two facts then narrowed it to one candidate:

- **The engine's `GLDisplay` object is alive and nothing in WebCore or the port terminates it.** On WIN,
  `PlatformDisplay::sharedDisplay()` deliberately leaks the display
  (`static PlatformDisplay* display = PlatformDisplayWin::create().release();`), `GLDisplay`'s own
  destructor is `= default`, and the only `eglTerminate` reachable from the engine
  (`GLDisplay::terminate()`) is called from `~PlatformDisplay()`, which therefore never runs. A grep of
  the fork and of `Src\port` for `eglTerminate` returns nothing.
- **The engine's `GLContext` still holds a live `ThreadSafeWeakPtr<GLDisplay>`** — the `glctx:` line only
  prints after `m_display.get()` answered non-null — so the failure is inside EGL, not a null wrapper.

The remaining layer was the one that had not been grepped: `Src\harness`. `MainPage.xaml.cpp:5922`,
`:5937` and `:5955` each ended with `if (ini) { eglTerminate(...); }`.

## 2. The mechanism, and why it repeats

`EnableGpu()` (`MainPage.xaml.cpp:5870`) posts a `gpu-init` job that, *before* `WebCoreGpuInit`, replays
WebCore's own EGL sequence as a diagnostic — and its third branch calls
`eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, {TYPE=D3D11})`, which is the
**exact** call `PlatformDisplayWin::create()` makes under `WK_WINUWP`. ANGLE resolves
`eglGetPlatformDisplay*` through a per-native-display cache, so that call hands back the **same ANGLE
`Display` object the engine will use** — and the probe then terminated it.

The repetition is what turned a latent bug into the normal case. `EnableGpu()` is retried **on every new
url** while `!m_gpuOn` (`MainPage.xaml.cpp:~3010`, "the auto-probe is retried on each NEW url, not once
per process"), and `m_gpuOn` only becomes 1 when the first frame's `WebCoreComposite()` returns 0. When
it does not, the probe runs again on the next navigation. Measured in the 23:58 session:

```
23:57:42.590  EnableGpu: m_gpuOn=0 m_gpuPresent=0 m_sessionActive=1
23:57:42.647  EnableGpu: first frame after init: EnableCompositing=0 Composite=-4
23:57:51.060  EnableGpu: m_gpuOn=0 m_gpuPresent=0 m_sessionActive=1
23:57:51.083  EnableGpu: first frame after init: EnableCompositing=0 Composite=-4
23:58:16.061  EnableGpu: m_gpuOn=0 m_gpuPresent=0 m_sessionActive=1
23:58:16.104  EnableGpu: first frame after init: EnableCompositing=1 Composite=-4
23:58:41.713  EnableGpu: m_gpuOn=0 m_gpuPresent=0 m_sessionActive=1
23:58:41.736  EnableGpu: first frame after init: EnableCompositing=1 Composite=-4
```

**Four probes, four `m_gpuOn=0`, up to twelve `eglTerminate` calls** on the engine's live display — and
`m_gpuOn` staying 0 is what kept the loop running. The pattern "the first composite works and every later
one fails" is exactly this: the first present happens before the next probe, and the probe's terminate
ends the context for good.

**A `bt:`/`tm:` corollary that matters for how earlier evidence is read.** The fork's `BitmapTexture` and
`TextureMapper` probes are in the installed DLL, and they *did* fire — in two runs only
(`prelaunch-0918-2053`: `bt:` 88, `tm:` 40; `prelaunch-098`: `bt:` 48, `tm:` 1352), and in the rest of the
GPU runs: zero. That is not "TextureMapper never painted"; it is "the windows in which it could paint were
the windows between a composite and the next probe". A probe that fires 1 % of the time is evidence about
the probe's surroundings, not about the code it was placed in.

## 3. The fix — two halves, both in `MainPage.xaml.cpp`

1. **The probe runs once per process.** A `static std::atomic<bool> s_eglProbed` gate at the top of the
   probe block; a later `gpu-init` job writes
   `EGL probe: skipped -- this process already probed it (EnableGpu is retried per url)` to
   `eglprobe.txt` so that the skip itself is visible rather than looking like a probe that produced
   nothing.
2. **It terminates nothing it did not create.** All three `eglTerminate` calls are gone. An EGL display is
   process-scoped and cheap; WebCore's own WIN path leaks one deliberately. Terminating one this code did
   not create costs a session.

Both halves are needed and neither is sufficient: once-per-process stops the repetition, and
no-terminate stops the first call from being enough to break the engine. The comments at the site say so
in the form the next reader needs — they name the `0x3001`, the four-probe measurement, and the "do not
re-add" rule, because this is precisely the kind of "tidy-up" a later pass would undo.

## 4. Measured, same bench, `0.1.10.13` — 00:32 launch

`gpudefault=1` seeded, `test.html` at startup, then `nav.txt` driving two navigations. Counted over one
session (`port-trace.txt` + `gpuinit-steps.txt`):

| signal | 0.1.10.12 (23:58) | 0.1.10.13 (00:32) |
|---|---|---|
| `eglMakeCurrent FAILED` | 231 | **0** |
| `rb: makeCurrent=0` / `no live GL context` | 228 / 228 | **0 / 0** |
| `tm: bp enter` (TextureMapper paints) | 0 | **178** |
| `bt: bindAsSurface enter` | 0 | **3438** |
| `[GPU] already active … -- reusing context` | — | 2 (the idempotent early-out, working) |
| `EnableGpu: presenting through the GPU surface` | never | **yes** |
| `0xC0000005` (any) | 1, in `~BitmapTexture` | **0** |

The transition, verbatim:

```
00:35:32.479  EnableGpu: m_gpuOn=0 m_gpuPresent=0 m_sessionActive=1
00:35:32.483  EnableGpu: WebCoreGpuInit returned 0
00:35:32.518  EnableGpu: first frame after init: EnableCompositing=1 Composite=0
00:35:32.519  EnableGpu: presenting through the GPU surface
```

**This is the first time the bench has run the device's present path at all**: `Composite=0` means
`gpuPresent()` succeeded, the harness switched `GpuPanel` in and `RenderImage` out, and the driver was
told to direct-present. Everything the GPU line has ever been credited with on the bench was, before
this build, either software rendering or a compositing tree painted into a context that had been killed
by this probe.

> Note the `nonwhite=0/1036944` in the `latediag` lines of that session. On the direct-present path the
> RGBA buffer is *not* filled — the pixels go to the ANGLE swapchain — so a zero paint count there is the
> signature of the GPU path being on, not of a blank page. Reading it as "nothing rendered" was one of
> the ways this defect hid. See the `gpu-direct-off` comment in `MainPage.xaml.cpp` for the same trap in
> the opposite direction (it produced a genuinely blank window when the flag was left on).

## 5. The second face — `Composite=-4` now names its own cause

`WebCoreComposite()` and `WebCoreCompositeReadback()` return `kErrNoView` (-4) for three different
missing things: the frame, its view, or `chrome->rootLayer()`. The harness logs only the number, so
`Composite=-4` in `log.txt` was unreadable — and it is the number that decides whether the bench presents
through the GPU. Both branches now emit a marker first:

```
[GPU] Composite -> kErrNoView: g_gpuActive=1 page=… frame=… view=… root=0 mainFrame=… pageMainFrame=…
```

Measured immediately: on `test.html` and on `dzen.ru` the answer is **`root=0`** — `PortChromeClient::
rootLayer()` returns `m_rootLayer`, which is set only by `attachRootGraphicsLayer()`, i.e. only when
WebCore builds a compositing tree. A page with no promoted layers has no root layer and the harness stays
on software render **by design**; `layertest.html` (the packaged `will-change: translateZ(0)` probe) is
the page that produces one, and it did. `mainFrame` and `pageMainFrame` are printed side by side because
they are two different handles in the port and a divergence between them was a candidate explanation --
measured equal in every case so far.

**Not yet re-observed, and now attributable if it recurs:** the 23:58 pairing
`EnableCompositing=1 Composite=-4` (root non-null one call earlier, missing the next) does not appear in
the 00:32 session. With the marker in place, the next occurrence will say which pointer went null.

## 6. Rules that came out of this

- **A probe that can change what it measures is not a probe.** This one was written to diagnose the GPU
  and it disabled the GPU. Before adding a diagnostic that calls into a subsystem, ask what its cleanup
  does when it unwinds — `eglTerminate` here, and the same shape in the `wgl`/`CGL`/`GLX` families.
- **Grep all three layers.** "Nothing in WebCore or the port terminates the display" was true, checked,
  and useless: the harness is the third layer of this project and it was the one holding the call.
- **A retry loop multiplies a latent bug into the normal case.** The probe was harmless *once*; the
  per-url retry (itself a correct fix for a different defect — see its comment) is what made it fatal.
  When a fix is "ask again later", audit what the retried action mutates.
- **An idle-looking counter can mean the opposite of what it says.** `nonwhite=0` with `Composite=0` is a
  working GPU present; `nonwhite=0` with `Composite=-4` is a page that did not render. The state that
  makes a number readable belongs next to the number.
- **`-Gpu` on the bench was not the device's configuration, and the difference was made by the host, not
  by the engine.** Any GPU claim from a bench run before `0.1.10.13` has to be re-measured. The
  `~BitmapTexture` teardown AV (`Doc/BITMAPTEXTURE-TEARDOWN-AV.md`) is the first such claim: it was
  captured on a session whose GL context had already been terminated, and it has not recurred in the
  first session with a live one.

## 7. The third face: the probe's retry key was the *requested* url, so a real page could never be probed

**Found and fixed 2026-09-19, x64 bench, build `0.1.10.19`.** §4 above recorded the first success
(`Composite=0`) on `layertest.html` — the packaged `will-change: translateZ(0)` probe, a page *built* to
have a root layer. A real page still could not reach the GPU, and the reason was the retry key.

`OnNavDone` gated the probe on `m_gpuTriedForUrl != m_currentUrl`, and `m_currentUrl` is the url the
**harness asked for**, not the document the engine loaded (it is assigned only in `NavigateTo`,
`ApplyEngineFrame` and `SaveActiveTab`; nothing refreshes it from the engine). When two loads of one
requested url produce two different documents, the second is never probed. Measured in a single session
(`0.1.10.18`, GPU armed, `gpudefault=1`), two loads of `https://dzen.ru/` eight minutes apart:

| | 05:39:32 | 05:42:44 |
|---|---|---|
| `after-load` | `url=https://dzen.ru/ … compositing=0` | `url=https://dzen.ru/ … compositing=1` |
| engine's document | `https://sso.dzen.ru/install?uuid=…` | `https://dzen.ru/` |
| `diag` | `body=0 nonwhite=0/1036944` | `nonwhite=547064/1036944` |
| `EnableGpu` | `EnableCompositing=0 Composite=-4` | **no `EnableGpu` line at all** |

The first load took the Yandex SSO path and ended on a document with no compositing tree, so the probe
correctly failed and latched `m_gpuTriedForUrl="https://dzen.ru/"`. The second load was the real dzen.ru
**with a root layer to present**, and was never asked — because both loads share their requested url. A
retry only happens while the GPU is still off, so that one honest failure pinned every later `dzen.ru`
load to software present. Across the whole session: **seven probes, seven `EnableCompositing=0
Composite=-4`**, and every `[GPU]` marker reading `root=0000000000000000`.

This also corrects §5's last paragraph, which reads "on `test.html` and on **`dzen.ru`** the answer is
`root=0`". That was measured on the *failed* dzen.ru — the SSO install page. The real dzen.ru has
`compositing=1` at load and a non-null root layer. The two documents are not the same page and do not
share a root-layer answer.

### The fix

The key is now the **document's** url, not the request's. It costs nothing extra: `finalUrl` is already
read from the engine on the engine thread (`WebCoreGetUrl`, in the same load job that feeds the
empty-page notice) and already arrives on the UI thread marshalled as `finalUrlCopy`, so `OnNavDone` takes
it as a new parameter and no ABI call or cross-thread read is added — the UI thread must never wait on the
engine (CLAUDE.md, Threading). Two log lines make the decision visible, since a *skipped* probe was
exactly what hid this for a whole session:

```
EnableGpu: probing for document <document url> (requested <requested url>)
EnableGpu: probe skipped, already tried for document <document url>
```

### Measured, same bench, `0.1.10.19`

```
05:52:00.514  EnableGpu: probing for document file:///…/test.html (requested file:///…/test.html)
05:52:00.571  EnableGpu: first frame after init: EnableCompositing=0 Composite=-4
              … staying on software present
05:52:31.634  EnableGpu: probing for document https://sso.dzen.ru/install?uuid=3df3e0cb-… (requested https://dzen.ru/)
05:52:31.641  EnableGpu: first frame after init: EnableCompositing=0 Composite=-4
              … staying on software present
05:53:22.507  [STAGE] after-load url=https://dzen.ru/?n=2 rc=0 compositing=1
05:53:22.523  EnableGpu: probing for document https://dzen.ru/?n=2 (requested https://dzen.ru/?n=2)
05:53:22.626  EnableGpu: first frame after init: EnableCompositing=1 Composite=0
05:53:22.626  EnableGpu: presenting through the GPU surface
```

and on the driver's side, in `gpuinit-steps.txt`:

```
[GPU] Composite -> kErrNoView: g_gpuActive=1 … root=0000000000000000   <- every probe before the fix
[GPU] paint path -> gpu direct present                                 <- the GPU path owns the screen
[GPU] direct present -> 0                                              <- the harness toggled this ITSELF
```

Note the first line: the key is demonstrably doing work — the probe for the SSO document and the probe
for `dzen.ru/?n=2` are **different keys** under the same requested url, which is the whole point. The
session stayed up and beating for 3+ minutes on the direct-present path (`beat busy=0 finished=284`
at 05:56:24) with the page painted (`nonwhite=571668/1036944`).

> **`[GPU] direct present -> N` is written by `WebCoreSetDirectPresent`, i.e. by the HARNESS telling the
> driver whether the swapchain panel is on screen — it is not evidence that `eglSwapBuffers` ran.** This
> was mis-read once, in the first draft of this section. The proof that a frame was actually presented is
> `Composite=0` (`gpuPresent` in the driver); the proof of which path owns the screen is the
> `[GPU] paint path ->` marker added in `0.1.10.20`. A `-> 0` line means the harness has just handed the
> screen back to software.

> **What `nonwhite` does and does not say here.** §4's table treats `nonwhite=0` as the signature of the
> GPU path being on, and that reading is conditional. `nonWhite` is computed in `gpuCompositeReadback`,
> i.e. on the readback/software path; on direct present nothing fills the harness's RGBA buffer, so the
> value in the diag is whatever the **last** readback left and stops moving. In the session above it held
> at `571668` from the load until the end. Use `Composite=0` / `[GPU] paint path -> gpu direct present` —
> not `nonwhite` — to tell whether the GPU is presenting.

## 8. The fourth face: the GPU keeps the screen when the next document has no layer to present

Once the GPU owns the screen, nothing gave it back. `paintToRGBA` reads `rootLayer()` itself, so a
document without a compositing tree is painted through Cairo into the harness's buffer — which is
correct — but **every blit site in the harness is guarded by `if (!m_gpuPresent)`**, and that flag was
still `true`. The Cairo pixels were produced and dropped, and the swapchain panel kept showing the
previous document's frame.

**Photographed, not inferred** (`Images/sshot02.png`, bench, 2026-09-19): window content was a fully
rendered dzen.ru — logo, search box, news feed — while the address bar read `https://example.com/` and
the tab title read `Example Domain`. Both of those come from `OnNavDone` and were correct; only the
pixels were stale. The engine's own marker agreed:

```
[GPU] paint path -> cairo (no root layer, or gpu inactive)
```

which is the marker added in `0.1.10.20` for exactly this purpose — see §6.

### Three separate reasons the hand-back blits nothing

Each one alone is enough to leave the window blank, and they had to be fixed in order because each one
hides the next:

1. **The blit at navigation time happens while `m_gpuPresent` is still true.** `ApplyEngineFrame` runs
   from the load job's callback; the re-check that finds `EnableCompositing=0` runs later, from
   `OnNavDone`. So the frame for the new document is offered to a blit that is still disabled.
2. **The live tick is not a repair mechanism.** It blits only when the frame hash *changes*
   (`if (hashCopy == s->m_lastFrameHash) return;`), and the frame in question is already the current
   one. Worse, `StartLiveMode` has five guards (foreground, drawer, action menu, settings, tab
   switcher) and the timer stops itself after 40 static ticks, so "wait for a tick" is a coin flip.
   `0.1.10.23` asks for the frame explicitly instead, in a `gpu-handback-frame` job.
3. **`BlitToBitmap` silently returns while `g_directPresent` is set** —
   `if (g_directPresent.load()) return;`. That flag is the harness's own "the panel is presenting"
   latch; it is set in `EnableGpu`'s success branch and cleared in its **failure** branch, but the
   hand-back is a *third* path and it left the flag set. So even the explicit request blitted nothing.

Point 3 is the one that cost the most, because of how it failed: **0.1.10.22 logged success while doing
nothing.** The hand-back line read

```
EnableGpu: handback frame blitted (1024x694 hash=3cf56188)
```

— a non-zero hash, a non-zero size, and a flat `#F0F0F0` on screen, which is `ContentArea`'s own
background showing through an `Image` whose `Source` had never been written. The logged hash comes from
the engine, so it proves the engine painted; it says nothing about whether the blit ran. Same lesson as
§6's `direct present` flag, one layer down: **a log line at the call site is not evidence that the callee
did the work.**

### The fix

`ReevaluateGpuForDocument()` (new, `MainPage.xaml.cpp`), keyed on the **engine's** document url, not the
requested one — the same distinction §7 is about. On a document with no compositing layer it clears
`m_gpuPresent`, `m_gpuOn`, `m_lastFrameHash` **and `g_directPresent`**, collapses `GpuPanel`, shows
`RenderImage`, tells the driver `WebCoreSetDirectPresent(0)`, and then requests one frame explicitly and
blits it. Clearing `m_gpuOn` re-arms the existing `EnableGpu` path, so a later page that *does* have a
compositing tree gets the GPU back without a second arming path.

### Measured, same bench, `0.1.10.23`

```
06:31:34.457  EnableGpu: first frame after init: EnableCompositing=1 Composite=0
06:31:34.459  EnableGpu: presenting through the GPU surface            <- dzen.ru on the GPU
06:32:38.344  EnableGpu: re-checking document https://example.com/ (requested https://example.com/)
06:32:38.345  EnableGpu: document check: EnableCompositing=0
06:32:38.350  EnableGpu: document has no compositing layer -- handing the screen back to software
06:32:38.393  EnableGpu: handback frame blitted (1024x694 hash=3cf56188)
```

with the window showing example.com rather than the previous document.

> **The same rule as §7, and it generalises.** §7's defect was a *key* that could never match; this one
> is a *flag* that nothing cleared. Both produced a plausible, wrong reading of a healthy session — and
> in both cases the fix was to make the state the harness acts on match the state it reports on. When a
> symptom is "the right thing did not happen and nothing failed", suspect the guard that decides whether
> to act, not the code that would have acted.

## 9. Two sizes, not one: a viewport in CSS px and a surface in device px

### The report

> «Ух ты, двухпальцевым жестом смог на dzen.ru сузить картинку (zoom-out). Браузер не крешнулся… И
> вроде бы в момент начала изменения скейлинга пропорции картинки улучшились ("растянутая морда" ->
> "нормальное лицо")» — и, отдельно, «Переход по ссылкам тоже стал корректнее».

Two observations, one cause. "Стretched face" is a page laid out at the wrong width for the pixels it
is displayed in; the moment a pinch changed the scaling, the two happened to agree and the proportions
looked right; and "следование по ссылкам" is where the wrongness came and went, as §9.3 shows.

### 9.1 The two quantities

`GpuPanel` is a `SwapChainPanel`, and XAML stretches whatever swapchain size it is handed across the
panel's whole area. On the bench the panel is 1368×758 DIP with **`CompositionScale = 2.00`**, so it is
physically 2736×1516. That gives two independent sizes which the first GPU path conflated:

| | meaning | bench value |
|---|---|---|
| **surface** | the ANGLE swapchain's pixel size | 2736×1516 |
| **engine viewport** | what `FrameView` lays out against, in CSS px | must be 1368×758 |

Hardware wants the first. Layout wants the second. **Feeding the physical size to both is the whole
defect**, and it has two opposite faces:

* **surface at DIP size** — the panel magnifies a 1368-px frame across 2736 physical pixels. Every CSS
  pixel becomes 2 device pixels *by upscaling*, so the page is soft. This is the original "растянутая
  морда": the picture is stretched, not laid out.
* **viewport at physical size, no compensation** — the page now lays out against 2736 CSS px. That is
  the desktop layout at *half* size: `contents=2736x12368` where software mode gives `1368x12368` for
  the same window and the same page (measured `0.1.10.25`, dzen.ru/news?n=2, 1368×758 DIP).

The second was my own first attempt at the fix, and it was wrong in the opposite direction. It took a
software-vs-GPU comparison *at the same window size* to see it: without a reference, "the page is
laid out at 2736 px" reads as a success, because that is exactly what the surface was resized to.

### 9.2 Three levers, and why page zoom is the one

* **`Page::setPageScaleFactor`** (what `WebCoreSetPageScale` drives, i.e. pinch) *magnifies* the
  finished layer tree. No relayout: `documentElement.clientWidth` does not move, media queries do not
  re-evaluate. A zoomed page is not a magnified page, and magnifying a too-wide layout leaves it too
  wide.
* **CSS page zoom** (`LocalFrame::setPageZoomFactor`, `WebCoreSetPageZoom`) *re-lays out*: the layout
  viewport becomes `viewport / zoom`, every layer's geometry is multiplied by zoom, and because the
  multiply happens before rasterisation the TextureMapper tiles come out at the physical resolution —
  sharp, not upscaled. Setting `zoom = CompositionScale` therefore gives software's own layout width
  *and* device-pixel rendering.
* **`Page::deviceScaleFactor`** was checked and rejected, on the source rather than on intuition.
  `GraphicsLayerTextureMapper::updateBackingStoreIfNeeded` uses it only for raster resolution —
  `m_backingStore->updateContentsScale(pageScaleFactor() * deviceScaleFactor())`
  (`GraphicsLayerTextureMapper.cpp:586`) — while the layer's own `m_size` is not scaled by it, and the
  root content layer is `rootContentsLayer->setSize(frameView->contentsSize())`
  (`RenderLayerCompositor.cpp:3225`), i.e. still the CSS width. `RenderLayerCompositor::deviceScaleFactor()`
  (`:4575`) is otherwise unused on this platform: its only consumer,
  `contentsScaleMultiplierForNewTiles` (`:4601`), is `#if PLATFORM(IOS_FAMILY)` and returns 1 otherwise.
  So the page would have been rendered crisply **into the top-left quarter of the window**.

### 9.3 Reading `contents=` in the diag

`contentsSize()` is not CSS px — it is **CSS px × zoom**, which is why the same page reports different
numbers per mode and why the numbers below are comparable only as ratios. The chain:
`LocalFrameView::adjustViewSize` takes `renderView->documentRect()` (the *scaled* rect; the function's
own log line prints the unscaled one next to it) and hands it to `setContentsSize`, which stores it
verbatim (`ScrollView::contentsSize` is `return m_contentsSize;`). Measured consistency, viewport
1368×758 DIP / 2736×1516 physical, zoom 2.00: `2736×24736` = 1368 CSS × 2 by 12368 CSS × 2.

### 9.4 Measured, same bench, `0.1.10.26`

```
Viewport[after-resize]: content=1368x758 panel=1368x758 compScale=2.00x2.00 engine=1368x758
ApplyViewportSize: 1368x758 dip, 1368x758 engine (session=1 gpu=0)     <- software, unchanged
[GPU] page zoom 1.00 -> 2.00 (layout 2048x24736 CSS-ish, viewport 1368x758)
ApplyViewportSize: engine rc=0 for 1368x758 dip (2736x1516 px)
Viewport[after-resize]: content=1368x758 panel=1368x758 compScale=2.00x2.00 engine=2736x1516
diag: url=https://dzen.ru/news?n=2 ... contents=2736x24736              <- 2x software's 1368x12368
```

with the window matching software mode's layout exactly and sharper (`Images/shot-gpu26.png` against
`Images/shot-soft25.png`; the pre-fix half-size frame is `Images/shot-gpu25b.png`).

The hand-back is the same arithmetic in reverse — `EnableGpu: document has no compositing layer --
handing the screen back to software`, then `ApplyViewportSize: engine rc=0 for 1368x758 dip (1368x758
px)` and `contents=1368x758` for example.com. The DIP latch (`m_appliedDipW/H`) has to be cleared for
that resize to happen at all: the DIP size never changed, so the early-out would otherwise return while
`kW/kH` were still twice it.

> **A resize is only real if the record it is compared against moves.** `ApplyViewportSize` keeps a DIP
> latch precisely so that XAML's frequent layout passes do not re-drive the engine, and every path that
> changes what the engine's size *means* — arming the GPU, handing back — has to invalidate that latch
> or it silently does nothing. The log said "engine rc=0", which is true and useless: the call it
> describes never happened.

### 9.5 The second face: a navigation resets the zoom, and only one page ever set it

`0.1.10.26` was verified on the page that armed the GPU and shipped. It was still wrong one navigation
later, and the log that caught it is worth reading exactly:

```
07:40:56  Viewport[after-resize]: ... engine=2736x1516
07:40:56  [GPU] page zoom 1.00 -> 2.00 (layout 2048x24736 ...)      <- n=2, correct
07:42:24  EnableGpu: re-checking document https://dzen.ru/news?n=3
07:42:24  EnableGpu: document check: EnableCompositing=1
          diag: url=https://dzen.ru/news?n=3 ... contents=2736x12368   <- n=3, half-size layout
```

Two things are visible at once. **The engine had put the factor back by itself** — the hand-back at
07:42:24 found `page zoom already 1.00` where 2.00 had been established 90 seconds earlier, reproduced
independently at 07:20:03 against a 2.00 set at 07:18:38 — so the reset is caused by the commit of a
new document, not by anything the harness does. And **nothing re-applied it**, because both harness
hooks are blind here:

* `EnableGpu` begins `if (m_gpuOn) return;` — and `m_gpuOn` has been true since the GPU was armed, so
  the arming path that *does* call `PushPageZoom` never runs again.
* `ReevaluateGpuForDocument` is the per-navigation hook, and it opened its UI-thread continuation with
  `if (ec != 0) return;` — "this document is presentable, keep the GPU surface" — which returned before
  the zoom and the resize. The one case that needed the fix was the one case that exited early.

So the honest statement of `0.1.10.26`'s state is: **the first compositing page after arming was
correct, and every compositing page after it was laid out at the physical width.** That is the
"следование по ссылкам" symptom, and it is the same class of failure §8 warns about — a guard that
decides whether to act, returning for the wrong reason.

The fix, `0.1.10.28`, has two halves:

1. **Both branches of `ReevaluateGpuForDocument` now do their job** (the `ec != 0` branch re-applies the
   zoom and re-runs the resize with the DIP latch cleared; the `ec == 0` branch is the hand-back
   described in §8), and the "staying on software" branch of `EnableGpu` now asks for zoom 1.0, since
   that path also leaves the GPU.
2. **The port no longer depends on the harness asking at the right moment.** `WebCoreSetPageZoom`
   records the request in `g_pageZoom` (session state, cleared in `teardownSession`), and
   `apoReassertPageZoom` — called at the top of `gpuPresent` and of `paintToRGBA` — restores it if the
   engine has drifted. Cost: one float compare per frame; the layout it may trigger happens at most
   once per document, because the next frame finds the factors equal. It is deliberately *not* done at
   the loader callbacks: those run after the frames in between have already been presented.
   **This half is unproven.** No `re-asserted` marker appears in any log of the session that
   introduced it, and that is the expected shape rather than a bug: between a document commit and
   `ReevaluateGpuForDocument` the harness presents nothing (the live tick does not blit while a
   navigation is in flight), so the window it guards is not reached by ordinary browsing. It covers
   the one present that *can* land in that window — a resize, i.e. `paintToRGBA` reached before the
   per-navigation hook — and its cost is a float compare otherwise. Treat the `ReevaluateGpuForDocument`
   half above as the fix and this as the belt to its braces; if the DPI invariant is ever seen broken
   again with no `re-asserted` line, this is not the half to look at.

Measured, `0.1.10.28`, the same three navigations that produced the defect:

```
[GPU] page zoom 1.00 -> 2.00 (layout 2048x24736 CSS-ish, viewport 1368x758)   <- n=2 arming
[GPU] page zoom 1.00 -> 2.00 (layout 2736x24736 CSS-ish, viewport 2736x1516)  <- n=3, re-applied
diag: url=https://dzen.ru/news?n=3 ... contents=2736x24736                     <- was 2736x12368
Viewport[after-resize]: content=1368x758 ... compScale=2.00x2.00 engine=2736x1516
```

`Images/shot-gpu28-n3.png` is that third page: proportions identical to software mode, text sharp,
images loaded. Note the second marker's `viewport 2736x1516` against the first's `1368x758` — the
re-application now happens *after* the resize rather than before it, which is harmless: `contentsSize`
reads 2736 either way, since it is 1368 CSS px × 2.

> **A per-navigation hook that returns early for the common case is not a hook.**
> `ReevaluateGpuForDocument` was written to handle the *unusual* case — a document with no compositing
> layer — and returned for the usual one, which quietly made "the usual case" mean "nobody is watching
> this". When adding a once-per-navigation step, decide for both outcomes, even if one of them is a
> no-op today: a `return` in the branch you consider normal is a decision to do nothing there forever.

### 9.6 The ARM32 consequence, and what is not yet measured

`CompositionScale` on the phone is not 2.00 — the Lumia 950 is 1440×2560 physical in a 576×1024 DIP
window, so the factor is **2.5**, and the whole chain above runs with `zoom = 2.5`: engine viewport
1440×2560, layout 576 CSS px wide, tiles rasterised at 2.5 device px per CSS px. That is ~6.25× the
tile pixels of a DIP-sized surface, and unlike the magnified case none of it is skipped work — it is
the price of the page being sharp, paid on a phone. The bench already shows the direction: one
`finishInteractionPaint` on a dzen.ru page measured **+2119 ms** at 2736×1516 against **+1243 ms** at
2048×1388.

None of that timing has been taken on the device. It is worth measuring there before anything else is
built on this, because the failure mode if it is too slow is not a wrong picture but a page that takes
seconds to react to a tap — and the mitigation (a lower device scale, i.e. back to `deviceScaleFactor`,
at the cost of the sharpness) is a decision to make, not a bug to fix.

**What §9 does not claim:** that `zoom = CompositionScale` is the only correct arrangement. It is the
one that keeps layout at DIP width *and* rasterisation at device resolution with a single upstream
lever, and it is verified on the bench. A cleaner arrangement would set the surface and the viewport
independently, which this port cannot do today — `WebCoreGpuResize` drives the EGL surface, the engine
viewport and `kW/kH` from one pair of numbers by construction.

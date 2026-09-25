# The GPU readback death: the process dies inside `beginPainting`, on the first page with a layer

Opened 2026-09-04, x64 bench line. This is the defect that `MEMORY.md` section 5 called "cause NOT
established". The cause now has a location, measured twice; the last link in the chain is named from
reading upstream and is being verified as this is written. Every claim below is marked MEASURED or
NOT VERIFIED, and nothing is promoted between the two without a run.

## Signature

`0.1.9.85` / `.86`, x64, `gpudefault=1` (i.e. `x64-cycle.ps1 -Gpu`), packaged 8-URL sequence.

- The process disappears mid-navigation. No `unhandled.txt`, no `UEF:` line in `log.txt`, no
  `exit-ok.txt`, no `wedgedump-<PID>.txt`.
- `log.txt` stops after `[STAGE] before-load <url>`; the matching `after-load` never arrives.
- Time from load start to death: **under one second** — measured 0.9 s in `.84`, ~1 s in `.85`/`.86`.

The absence of a dump is not evidence of anything. The wedge dumper needs six consecutive stuck
700 ms beats (4.2 s, chosen to clear the 2.5 s background-fetch budget — see
`PUMPLOOP-SILENT-DEATH.md`), and a death inside one beat cannot reach it. **This is a fast fault, not
a hang**, which is the opposite of every earlier silent death in this project. Do not reach for the
hang playbook here.

## What made it findable: lighting the window instead of reasoning about it

The whole stretch of `WebCoreSessionLoad` from `probeSpaModule` to its `return kOK` carried no trace
output at all. `port-trace.txt` ended at `spa: probe rc=0 kick='no-mod'` — a line printed *inside*
`probeSpaModule` — and the next thing anyone would have seen was the harness's `after-load`, which
never came. So roughly twenty statements, including the newest and least-exercised code in the port,
sat in one unlit window about a second wide.

Two sets of markers now cover it, both on the append-and-flush-per-line channel that is the only
diagnostic that survives here:

- `tail:` — in `WebCoreSessionLoad`: probe returned, updateLayout, extractLinks, paintToRGBA
  enter/exit (with `rootLayer=`), writeDiag.
- `rb:` — inside `gpuCompositeReadback`: enter with geometry, `makeCurrent`, gpuPrepare,
  BitmapTexture::create, texture ok with `glGetError()`, docBg, beginPainting, layer paint,
  readPixels.

The cost was one driver compile and one relink per iteration, and it produced the answer in two runs.

## Where, exactly (MEASURED, two consecutive runs)

The trace reaches `rb: docBg ok` and stops. `rb: beginPainting ok` never appears. So the process dies
inside

```cpp
g_textureMapper->beginPainting(TextureMapper::FlipY::No, texture.ptr());
```

in `gpuCompositeReadback` (`Src/port/WebCoreDriver.cpp`). Supporting measurements from the same runs:

| Marker | Reading | What it rules out |
|---|---|---|
| `rb: enter 1024x694` | reached | geometry is the session's own, no mismatch |
| `rb: gpuPrepare enter` → next marker reached | flush + `updateBackingStoreIncludingSubLayers` + animations all complete | the compositing-state flush is innocent |
| `rb: texture ok glErr=0x0000` | reached, clean | `BitmapTexture::create` of 1024x694 with `SupportsAlpha\|DepthBuffer` succeeds; the allocation is innocent |
| `rb: docBg ok` | reached | `view.documentBackgroundColor()` is innocent |
| `rb: beginPainting ok` | **never** | the fault is inside `beginPainting` |

## It is not the artificial probe, and it is not one page

`tail: paintToRGBA enter (rootLayer=N)` prints the deciding condition on every load, and across a
whole run it reads:

- `rootLayer=0` for example.com (twice), news.ycombinator.com, and the startup page — each returns
  through Cairo with a full `nonWhite` and lives.
- `rootLayer=1` **exactly once per run** — and that entry kills the process, every time.

In `.85` that entry was **ya.ru**, a network-served page. In `.86` it was `layertest.html`, the local
probe. Same boundary, same marker, both times. So this is not an artifact of the `file://` load path
(which `MEMORY.md` §5 item 3 had flagged as a gap), and it is not specific to a hand-made test page:
**any page WebCore decides to give its own compositing layer takes this path, and the path is fatal.**

That also explains why the GPU line looked healthy for so long. `Composite` returning -4
(`kErrNoView`, "no root layer") on ordinary sites is the honest answer, not a failure — but it means
`gpuCompositeReadback` was simply never called. The branch had one entry point per run and nobody had
arranged for it to be taken.

## Two answered questions from the previous triage

Both were listed as "NOT VERIFIED / next measurements" in `MEMORY.md` §5. Neither is the cause.

1. **Why no wedge dump fired.** Because it cannot: the death fits inside one heartbeat beat and the
   dumper needs six. Answered by arithmetic, and the arithmetic is in the code
   (`MainPage.xaml.cpp`, `s_stuckBeats >= 6`, 700 ms interval).
2. **The `if (g_gpuActive) return kOK` early-out in `WebCoreGpuInit`.** It now logs, and it prints
   `[GPU] already active at 1024x694 -- reusing context` — the requested geometry matches what the
   surface was created at, so there is no stale-size context. It was suspicious only because it was
   silent: a bare `[GPU] enter` with no following marker reads like an init that vanished. It now
   says which of the two cases it is, and warns explicitly when a differing size is being ignored.

## The first named suspect — RETRACTED by its own probe

The first candidate was that `depthBufferFormat()` dereferences a **null** `GLContext::current()`:

```cpp
GLenum depthBufferFormat()
{
    auto* glContext = GLContext::current();
    if (glContext->version() >= 300 || glContext->glExtensions().OES_packed_depth_stencil)
```

The reasoning was sound — no null check, reached only via `Flags::DepthBuffer`, only on a texture's
first bind, and `GLContext::current()` returns null unless the thread-local `s_currentContext` is a
**Native**-type wrapper (`GLContextWrapper.cpp`), so an ANGLE context made current anywhere on the
engine thread would make it read null. A `WK_WINUWP` guard was added that logs and returns
`GL_DEPTH_COMPONENT16` if the pointer is null.

**The guard never fired, and the process died anyway.** `current()` is not null. The suspect is
cleared. Recorded because the probe was built to be informative either way, and because the correct
answer turned out to be one step further down the same function.

## CONFIRMED cause: `glGetString(GL_VERSION)` returns NULL, and `versionFromString` indexes an empty Vector

Measured, `0.1.9.90`, x64 bench, `-Gpu`:

```
bt: depth genRenderbuffers
bt: depthBufferFormat GL_VERSION=(NULL -- no live GL context)
```

`GLContext::version()` passes whatever `glGetString(GL_VERSION)` returned straight into
`versionFromString`, unchecked (`platform/graphics/egl/GLContext.cpp`):

```cpp
unsigned GLContext::versionFromString(const char* versionStringAsChar)
{
    auto versionString = String::fromLatin1(versionStringAsChar);   // nullptr -> null String
    Vector<String> versionStringComponents = versionString.split(' ');  // -> EMPTY Vector
    Vector<String> versionDigits;
    if (versionStringComponents[0] == "OpenGL"_s) {                 // index 0 of an empty Vector
```

`String::fromLatin1(nullptr)` gives a null string, `split(' ')` on it gives an **empty** `Vector`, and
`versionStringComponents[0]` then reads through a null buffer. `Vector::operator[]`'s bounds check is
`ASSERT_WITH_SECURITY_IMPLICATION`, which is compiled out of a release build — so there is no
diagnostic, just a read at offset 0 of nothing.

That is the whole defect. Every index in that function is unchecked: `[0]`, `[2]` in the "OpenGL"
branch, and the `[0]`/`[1]` digit pair at the end. All four are now guarded under `WK_WINUWP`, and
`depthBufferFormat()` additionally short-circuits to `GL_DEPTH_COMPONENT16` when `GL_VERSION` reads
NULL, so the browser degrades instead of vanishing.

### Verification

With the guards in place, the same sequence now walks the whole path it used to die on:

```
bt: depth format=0x81A5 1024x694      (GL_DEPTH_COMPONENT16)
bt: depth attached
bt: fbo ok
bt: clearIfNeeded ok
bt: bindAsSurface done
tm: bindSurface bindAsSurface ok
tm: bindSurface projection ok
tm: bp done
rb: beginPainting ok glErr=0x0000
rb: layer paint enter
rb: readPixels done
```

The process survived the layer page and every entry after it, and kept running until the maintainer
closed it by hand (`exit-ok.txt` records that manual close, not a self-terminating run). Before the
guard it died within a second of reaching that page, every time.

## The defect this uncovered, and it is the bigger one: there is no live GL context

`GL_VERSION` reading NULL is not a quirk of that one call. Three independent measurements from the
same instant say the engine thread has **no usable GL context at all**, while every API involved
reports success:

| Probe | Reading | Meaning |
|---|---|---|
| `rb: makeCurrent=1` | `GLContext::makeContextCurrent()` returned **true** | the context believes it is current |
| `bt: fbo=0 tex=0` | `glGenFramebuffers` and `glGenTextures` both produced name **0** | no GL object was created |
| `rb: texture ok glErr=0x0000` | `glGetError()` is `GL_NO_ERROR` | nothing reported a failure |
| `GL_VERSION=(NULL)` | `glGetString` returned nullptr | there is no context to ask |

So the entire GPU composite runs against a dead context and reports success at every step. The
readback then returns a buffer of whatever `glReadPixels` left untouched, and — this is the part that
matters — `gpuCompositeReadback`'s emptiness test **passes** on it: `tail: paintToRGBA exit rc=0
nonWhite=710656` on `layertest.html`, i.e. all 710656 pixels differ from the document's white
background, so `contentPx != 0`, so the function returns `kOK` and **suppresses the Cairo fallback**.
The `contentPx` heuristic added on 2026-08-29 cannot tell "a real dark frame" from "a readback of
nothing".

`rb: enter` appears **21 times** in one run against a single `tail: paintToRGBA enter (rootLayer=1)`.
The other twenty come from `WebCoreComposite` on the live tick — so once any page acquires a root
layer, every subsequent tick reads back a dead-context frame.

This is now the open item. It also explains, retroactively, why GPU compositing has never produced a
verified frame on this line: it was never actually running on a live context.

### Why `makeContextCurrent()` lies

`GLContext::makeContextCurrent()` opens with

```cpp
if (isCurrent())
    return true;
```

and `isCurrent()` is `s_currentContext == this` — a `thread_local` pointer in WebCore.dll that is set
by `didMakeContextCurrent()` when `eglMakeCurrent` succeeds. It is never cleared when EGL loses the
context underneath it. So after one successful `eglMakeCurrent` at `WebCoreGpuInit`, every later call
on that thread short-circuits and **never calls `eglMakeCurrent` again**, no matter what happened to
the surface in between.

The leading hypothesis for what happens in between — NOT VERIFIED — is the window surface. The
context is created with a native window (`g_gpuPresentMode` is 1, `gpuinit.txt` says
`WebCoreGpuInit(window) rc=0`), that window is the `SwapChainPanel`, and the harness then decides to
stay on software present and collapses the panel. A collapsed `SwapChainPanel` can have its swapchain
released underneath ANGLE, which would invalidate the EGL surface the context is bound to.

## Two traps this investigation walked into, both worth remembering

**A trace read from a build that did not link.** The first attempt logged `GLContext::current()`
directly from the driver. It compiled — the symbol is declared in the header — and then the harness
link failed with `LNK2019: unresolved external symbol WebCore::GLContext::current`, because that
static is not exported from `WebCore.dll`. `x64-cycle.ps1` threw at its msbuild step, so nothing was
reinstalled and nothing relaunched; the **previously installed build kept running** and its markers
were still sitting in `port-trace.txt`, one revision behind. Reading them looked like a successful run
whose new lines had mysteriously not printed. Check that the appx actually built before believing a
trace, and prefer a probe that cannot fail to link — a guard inside WebCore beats an unexported symbol
read from outside it.

**A probe that silently was not there.** `navseq.txt` entry 1b points at
`LocalState\layertest.html`, but `x64-cycle.ps1` only ever seeded `test.html`, and its own install
step wipes `LocalState`. The file a previous session had placed there by hand was therefore gone, and
the `.85` run failed that entry with `curlcode=37` ("Could not open file"), skipping **the only
sequence entry that reaches the GPU branch**. It failed visibly, in `log.txt` — and the run still
read as a pass at a glance, which is the dangerous part. That run's `rootLayer=1` death happened to
land on ya.ru instead, which is how the network path got covered by accident.

Fixed: `x64-cycle.ps1` now writes `layertest.html` (will-change + translateZ) next to `test.html` on
every cycle, so the entry cannot quietly vanish again.

## Status

**CLOSED — the crash.** MEASURED end to end: the process died inside `TextureMapper::beginPainting`,
on the first and only page per run with a root compositing layer, on both a network page (ya.ru) and
a local one (`layertest.html`), within a second of load start, with no exception and no dump. The
dereference is `versionFromString`'s `versionStringComponents[0]` on an empty `Vector`, reached from
`depthBufferFormat()` → `GLContext::version()` with a NULL `GL_VERSION`. Guarded; the same sequence
now walks the whole path and the browser keeps running.

**Also settled:** the wedge dumper cannot fire on a sub-beat death, and the `WebCoreGpuInit`
early-out is not implicated (it logs `already active at 1024x694 -- reusing context`).

**OPEN, and now the main defect — no live GL context.** MEASURED: `glGenTextures` and
`glGenFramebuffers` both return name 0, `glGetString(GL_VERSION)` returns NULL, `glGetError()` returns
`GL_NO_ERROR`, and `makeContextCurrent()` returns true. The GPU composite therefore runs against
nothing and reports success, `gpuCompositeReadback`'s `contentPx` test passes on the resulting buffer
(`nonWhite=710656/710656`), and the Cairo fallback is suppressed. NOT VERIFIED: why the context is
lost. The `isCurrent()` thread-local fast path, which skips `eglMakeCurrent` forever after the first
success, is the mechanism that hides it; the collapsed `SwapChainPanel` is the leading hypothesis for
the cause.

## Fixes landed

| Change | File | Kind |
|---|---|---|
| Bounds guards on all four unchecked indices | `platform/graphics/egl/GLContext.cpp` (`versionFromString`) | `WK_WINUWP`, upstream latent bug |
| NULL `GL_VERSION` → `GL_DEPTH_COMPONENT16` + trace line | `platform/graphics/texmap/BitmapTexture.cpp` (`depthBufferFormat`) | `WK_WINUWP` |
| `tail:` markers over the post-probe span of `WebCoreSessionLoad` | `Src/port/WebCoreDriver.cpp` | instrument |
| `rb:` markers through `gpuCompositeReadback`, incl. `makeContextCurrent`'s return | `Src/port/WebCoreDriver.cpp` | instrument |
| `tm:` / `bt:` markers through `beginPainting`, `bindSurface`, `bindAsSurface`, `createFboIfNeeded`, `initializeDepthBuffer` | `TextureMapper.cpp`, `BitmapTexture.cpp` | `WK_WINUWP` instrument |
| `WebCoreGpuInit` early-out now says which case it took, and warns on a size it is ignoring | `Src/port/WebCoreDriver.cpp` | diagnostic |
| Seed `layertest.html` into LocalState every cycle | `Src/tools/x64-cycle.ps1` | test infrastructure |

## See also

- `MEMORY.md` §5 / §5a — the session handoff this came out of.
- `PUMPLOOP-SILENT-DEATH.md` — the *hang* family of silent deaths, and why the wedge threshold is six
  beats. Read it to avoid applying its playbook to this defect, which is a different animal.
- `CLAUDE.md` — the standing rules this touched: markers are the only surviving channel; both
  architectures compile the GPU path; every upstream edit is `WK_WINUWP`-guarded and carries an
  `Apotheosis:` comment.

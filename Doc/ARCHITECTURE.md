# Apotheosis -- architecture of the current line

> Written 2026-09-27, replacing `Doc/Archive/Architect.md` (a Phase 1-3 task tracker) and
> `Doc/Archive/Architect_2.md` (a July 2026 RnD report). Both are kept in the archive as history;
> neither describes what the code looks like now. Where they disagree with this file, this file wins.
> Companion documents: [INDEX.md](INDEX.md) (front door), [MVP.md](MVP.md) (acceptance contract),
> [PLAN.md](PLAN.md) (live state), [LESSONS.md](LESSONS.md) (operating rules).

## The thesis in one paragraph

WebKit does not run on Windows 10 Mobile. It has no UWP port, no WinRT integration, and its build
system assumes a POSIX host. So this repository is **not a browser source tree** -- it is a *port
layer plus a shell plus tooling* around an external, gitignored WebKit checkout. The port layer
supplies the platform WebKit expects (networking, crypto, pasteboard, accessibility, fonts, screens)
as UWP-native code, and exposes the result through one narrow C ABI. The shell is an ordinary UWP
app that consumes that ABI. The boundary exists so the two halves can use **different compilers**
(MSVC for the shell, clang-cl for the engine), which is what makes the whole thing buildable on
Windows at all.

## The whole system in one picture

```
+==============================================================================+
|  SHELL -- C# UWP app        Src/Apotheosis/   Apotheosis.csproj (MSBuild)    |
|                                                                              |
|   XAML UI: address bar, history, settings, link overlay                      |
|                                                                              |
|     UI-thread events                Managed queue                engine      |
|     OnImageManipDelta  ----+      +-------------------+      +----------+    |
|     OnPageTapped        -----+---->|  MainPage        |----->| WebEngine|    |
|     OnImageManipCompleted ---+    |  marshals, blits |      | Post()   |    |
|     AutoCmd watcher  ----+        +-------------------+      +----+-----+    |
|                         |                                        |          |
+==============================================================================+
                          |            P/Invoke, 30 C exports    |
+==============================================================================+
|  DRIVER -- WebCoreDriver.dll     Src/port/                            |
|  WebCoreDriver.cpp  5693 lines, clang-cl + lld-link                    |
|                                                                             |
|   every exported entry point is a three-line SEH wrapper:                   |
|                                                                             |
|      int WebCoreScrollBy(int dx, int dy, uint8_t* outRGBA) {                |
|          __try     { return WebCoreScrollByImpl(dx, dy, outRGBA); }        |
|          __except  (ApoSehFilter(GetExceptionInformation()))               |
+==============================================================================+
          |                                   |                              |
+==============================================================================+
|  PORT GLUE                Src/port/       |  PLATFORM STUBS  Src/port/    |
|   PortChromeClient          navigation UI  |   stubs-crypto    TLS/certs   |
|   PortPlatformStrategies   platform policy |   stubs-ax        accessib.   |
|   PortNetworkStorageSession cookies/session|   stubs-font-uwp  fontconfig  |
|   PortStorage               storage quota  |   stubs-gdi-uwp   text draw   |
|   LoadingFrameLoaderClient subresource load|   stubs-network   sockets     |
+==============================================================================+   stubs-pasteboard  clipboard
                                                                            stubs-screen      displays
+==============================================================================+   stubs-sqlite     history DB
|  ENGINE -- external, gitignored          |   stubs-loader / other        |
|   WebKit  webkitgtk-2.52.4 @ 7acdf5e    +=================================+
|   WK_WINUWP patch set, ARM32/thumbv7
|   WebCore + JavaScriptCore + WTF
+==============================================================================+
          |
          v
+==============================================================================+
|  PAINT BACK END                     currently selected at runtime         |
|   Cairo (software)  <-- the working baseline                            |
|   ANGLE D3D11 FL9_3 + TextureMapper  <-- disabled, WebCoreGpuInit rc=-20  |
+==============================================================================+
```

## Why the C ABI is the load-bearing decision

Everything else follows from it. WebKit is built by clang-cl with a UWP toolchain, the shell is built
by MSBuild with MSVC. They cannot see each other's headers meaningfully: different CRT assumptions,
different exception models, different marshalling rules. A `__cdecl` boundary with plain-old-data
arguments is the only place where the two compilers' opinions do not matter.

The cost of that decision is the 5693-line driver and the 30 hand-maintained exports. The benefit is
that the shell is a normal UWP app and the engine is a normal DLL, and either half can be replaced
without touching the other.

## The export surface, and why it is thirty functions and not three hundred

```
EXPORT SURFACE  (Src/port/WebCoreDriver.exports.def, 30 entries)

  TLS / CA          WebCoreSetCACertBlob
  Managed-host      WebCoreSetFontConfigFile  WebCoreSetSehGuard  WebCoreGetSehInfo
                    WebCoreGetFontInfo        WebCoreDumpFontConfig
  Configuration     WebCoreConfigure
  One-shot render   WebCoreRenderHtml
  Session           WebCoreSessionLoad  WebCoreCloseSession
  Interaction       WebCoreClickAt  WebCoreScrollBy  WebCoreSyncLinks
                    WebCoreTypeText  WebCoreKeyAction  WebCoreFocusedEditable
                    WebCoreSetPageScale  WebCoreLiveTick
  Metadata          WebCoreGetTitle  WebCoreGetUrl  WebCoreGetLinkCount
                    WebCoreGetLink  WebCoreGetFrameHash
                    WebCoreGetPendingResourceCount  WebCoreGetLastError
                    WebCoreGetDiag  WebCoreEditDebug
  Find in page      WebCoreFindString  WebCoreFindNext  WebCoreFindClear
  Download          WebCoreDownload
  User agent        WebCoreSetUserAgentMobile  WebCoreSetUserAgentString
  GPU compositing   WebCoreGpuInit  WebCoreEnableCompositing
                    WebCoreGpuLayerInfo  WebCoreSetGpuInitLogFile
```

Nine earlier exports were deliberately removed rather than left to rot: `WebCoreRenderHtmlStub`,
`WebCoreLoadUrl` (superseded by the persistent session), `WebCoreSetCACertPath` (a file-based CA
cannot work inside an App Container), `WebCoreGetPageScale`, `WebCoreSessionPaint` (superseded by
`WebCoreLiveTick`), `WebCoreComposite`, `WebCoreCompositeReadback`, `WebCoreGpuSetFlip`, and
`WebCoreEvalJS`. The comments in the `.def` file record why each one went. An export surface is an
API: every function kept is a promise, and the ones with no caller were promises to nobody.

The export list is also the *only* contract. The shell knows nothing about `WebCore`, and the driver
exposes nothing about XAML. That is deliberate, and it is what made it possible to swap a C++/CX
host for a C# one without the engine noticing.

## Threading: the part that decides whether anything else works

```
   UI thread (XAML)                  engine thread (single, LongRunning)
   ================================   ======================================
   OnImageManipDelta                 Task.Factory.StartNew(Loop, LongRunning)
     -> FreeScrollBy                    -> SetupRuntimeEnv() once
        accumulate dx/dy                -> foreach job in BlockingCollection
     -> PumpScroll  (if !_scrollBusy)      -> job()
        grab accum, clear, set busy        (all P/Invokes happen here)
        Post(job)
                                      returns -> disp.RunAsync(Normal):
                                         if (_opSeq != mySeq) drop the frame
                                         if (rc == 0 && !_gpuPresent) BlitToBitmap
                                         _scrollBusy = false
                                         if more accumulated -> PumpScroll()
                                         else SyncLinksAfterScroll(); StartLiveMode()
```

Three rules make this safe, and all three were learned the hard way:

1. **One engine thread, ever.** `Task.Factory.StartNew(..., LongRunning)` is used instead of
   `new Thread` because .NET Native does not ship `System.Threading.Thread` at all (`ILT0003`).
   The queue is a `BlockingCollection<Action>`; every driver call is a job posted to it. Nothing
   calls the engine directly from the UI thread.

2. **Results come back through the dispatcher, and they carry a sequence number.** `_opSeq`
   increments per navigation. A frame that arrives after a new navigation has started is dropped
   rather than blitted, which is what stops a stale frame from overwriting a fresh one.

3. **Scroll deltas are coalesced, not queued.** `FreeScrollBy` only accumulates. If `_scrollBusy`
   is already set, the delta waits and is merged into the next pump. A fast finger therefore produces
   *fewer* engine calls than input events, not more. Without this the engine queue grows without
   bound and the page appears to seize up -- which is exactly the "smooth, then it stops" failure
   the bench originally reported.

```
WHY COALESCING EXISTS  (design intent -- the ratio is NOT yet instrumented)

  a fast finger should produce FEWER engine calls than it produces input
  events, so the page tracks the finger instead of working through a backlog
  it can never catch up on.

  To measure it, log one line per engine-side WebCoreScrollBy and compare
  that count with the count of FreeScrollBy calls. The autocmd harness logs
  only the UI-side calls, so the coalescing ratio is still an inference from
  the code, not an observation. See "Known gaps" below.

## The present path, and the two mistakes that cost the most time

The engine paints into a caller-supplied RGBA buffer. Everything after that is the shell's problem,
and this is where the project spent a long time getting a browser that flickered.

```
  engine paints                       shell presents
  ---------------                      --------------
  WebCoreScrollBy(dx,dy, outRGBA) --> byte[] rgba  (pinned GCHandle, per call)
                                        |
                            +-----------+-----------+
                            |                       |
                    _gpuPresent == true      _gpuPresent == false   (current baseline)
                            |                       |
                  ANGLE presents into         BlitToBitmap(rgba)
                  a SwapChainPanel                       |
                  (the driver does it)          WriteableBitmap on Image
                                                 |
                                            _lastFrameHash = 0  -> forces
                                            a repaint on the next live tick
```

**Mistake 1: reusing the `WriteableBitmap` across frames.** Writing into a surface that XAML may
still be compositing produces a visible flash. A `WriteableBitmap` on screen is not exclusively
yours; the compositor may hold a reference. Every frame must get a fresh surface.

**Mistake 2: reusing the RGBA array across frames.** This one looked safe and was not. Dispatcher
closures keep a reference to the buffer they were handed, so a buffer can still be read by a queued
frame while the engine is already writing the next one into it. Pooling RGBA frames and pinning them
across the round trip reintroduced exactly the flicker that allocating fresh had fixed. Both
attempts are reverted; each `WebCoreScrollBy` call now gets its own buffer and its own pin, and the
only thing that is reused is the *link buffer*, which is consumed synchronously and never escapes.

The general rule, and it cost two days to learn: **in this app, an on-screen surface and a
dispatcher-visible buffer are both borrowed, not owned.** The engine is the only thing in the process
that may assume exclusive ownership of memory, and only for the duration of one call.

## The one hard constraint: a native unwind across P/Invoke is fatal

This is the single most important fact about the C# line, and it explains why the driver has the
shape it has.

```
  native fault inside WebCoreScrollBy
              |
              v
  exception tries to unwind out of the P/Invoke frame
              |
              v
  mrt100_app!RhpPInvokeExceptionGuard
  mrt100_app!RhpFailFastForPInvokeExceptionPreemp
              |
              v
  FailFast(7)  -->  WER: 0x00001007 in SharedLibrary.dll
                     process gone in under a second
                     NO managed catch can see it
                     no exception object, no return code, no stage line
```

There is nothing to `catch`. The managed side sees a process that simply stops. The C++ harness does
not have this behaviour -- it has its own unhandled-exception filter and wedge dumps -- which is why
identical engine code is a debuggable crash there and an unexplainable death here.

The answer is the guard, and it is opt-in so the native harness line is unchanged:

```c
// Src/port/WebCoreDriver.cpp -- bodies are static *Impl, exports are wrappers.
// A function containing __try cannot also require object unwinding, which is
// exactly why the bodies had to be renamed.
int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    __try     { return WebCoreRenderHtmlImpl(utf8Html, w, h, outRGBA); }
    __except  (ApoSehFilter(GetExceptionInformation())) { return kErrSeh; }
}
```

- `WebCoreSetSehGuard(1)` is called from `SetupRuntimeEnv()` before any engine call.
- Guard **on**: a native fault becomes `rc = -17` (`kErrSeh`) plus a `WebCoreGetSehInfo` line. The
  app survives and can show an error page. This is what the shipping app uses.
- Guard **off** (the default): the filter returns `EXCEPTION_CONTINUE_SEARCH` and the host sees
  precisely the fault it saw before, so the native harness is unaffected.

`clang-cl` accepts `__try`/`__except` on x64 with `_HAS_EXCEPTIONS=0` and `/EHs-c-`. SEH is
independent of C++ exception handling; that independence is the whole trick.

## The managed host cannot set FONTCONFIG_FILE

A short entry, kept because it is the cleanest example of the shell's fundamental limitation.
`Environment.SetEnvironmentVariable` does not reach the native process environment block inside an
App Container, so the engine saw no font configuration at all, fontconfig matched nothing, cairo
answered `CAIRO_STATUS_NO_MEMORY`, and the symptom was "text is missing" -- a font problem
presenting as a memory problem. The fix is to do it natively, exactly as the C++ harness does:

```csharp
WebCoreDriver.WebCoreSetFontConfigFile(confPath);   // _putenv_s inside the driver
WebCoreDriver.WebCoreSetSehGuard(1);                // before the first engine call
```

The driver also derives `APOTHEOSIS_FONTS_DIR` from the `<dir>` element in `fonts.conf`, because the
managed host had no way to know the font directory that fontconfig was about to use.

## Diagnostics: a managed-side change is not a measurement

There is no debugger, no OutputDebugString, and no console in the App Container. Every answer comes
from a file in `LocalState`, and the run is only finished when those files are read.

```
  LocalState\
    stage.txt          last phase + the return code, OVERWRITTEN each time
    setup-trace.txt    append-only breadcrumbs through SetupRuntimeEnv()
    boot.txt           app start
    diag.txt           engine-side page diagnostics
    gpuinit.txt        GPU bring-up attempt and its return code
    gpuresult.txt      GPU result after bring-up
    layertree.txt      GPU layer tree
    jitresult.txt      JIT status
    imedebug.txt       IME / text input
    debug-report.txt   consolidated report
    autodump.txt       raw dump output
    testurl.txt        IN: the smoke-test URL (a bare URL, not `goto <url>`)
    autocmd.txt        IN: a command batch for the scripted controller
    autocmd-log.txt    OUT: per-command results and timings
```

Two rules that keep this honest:

- **`stage.txt` overwrites, so it cannot answer "how far did setup get".** That is what
  `setup-trace.txt` exists for. A swallowed exception inside `SetupRuntimeEnv()` is otherwise
  invisible from the outside and surfaces much later as a page that will not load.
- **A managed-side change is not a measurement.** Only the return codes and these files count as
  evidence. A screenshot proves pixels reached the screen; it does not prove the frame path is
  correct, and a `rc=0` alone does not prove a page painted anything.

### The scripted controller

`Src/Apotheosis/MainPage.AutoCmd.cs` is the operational answer to "how do you test this without a
person standing at the bench". A 250 ms watcher on `autocmd.txt` runs a batch of commands on the
engine thread and appends per-command timings to `autocmd-log.txt`.

```
  goto <url>                 navigate
  scroll <dx> <dy>           one engine-path scroll step, timed
  swipe <dy> <steps> <gapMs> a finger: drives FreeScrollBy on the UI thread at ~60Hz
  scrolln <dx> <dy> <n>      n engine-path steps, timed individually
  wait <ms>                  sleep on the engine thread
  hash                       log the frame hash
  diag                       log the driver page diagnostics
```

`swipe` exists because `scroll` is not sufficient. `scroll` calls `WebCoreScrollBy` directly and
therefore measures only engine plus present. A real drag goes
`OnImageManipDelta -> FreeScrollBy -> PumpScroll -> WebCoreScrollBy -> BlitToBitmap`, including the
coalescing and the link sync, and *that* is the path a user exercises. `swipe` runs on the UI thread
so the real merge logic executes, and it awaits between steps rather than blocking, because a
blocking sleep would freeze the dispatcher and the measurement would describe a stalled UI instead of
a swipe.

## What is actually measured, and what is not

Everything below was produced on the x64 dev box on 2026-09-27 against `ya.ru`. None of it is
device data.

```
MEASURED -- scrolling cost per frame, engine path (scrolln, software present)

  after cold paint   173  ->  121  ->  88  ->  43  ->  11-13 ms
  the first frames are expensive; the steady state is ~12 ms

MEASURED -- stationary frames, software present

  7 consecutive samples, all hash 0x4ab8c4bd
  no frame-to-frame flicker oscillation reproduced

MEASURED -- swipe, real touch path (swipe 4800 200 16, 200 steps x 24px)

  total 5913 ms, 29.6 ms per step effective
  per-step cost by quarter of the drag:
      Q1  28.5 ms      Q2  31.6 ms      Q3  27.1 ms      Q4  30.6 ms
  FLAT. No degradation with distance, and the last quarter is no worse
  than the first. The "smooth then it seizes up" failure did NOT
  reproduce once the full touch path was driven.

  requested 16 ms per step; observed ~30 ms
  => ~33 effective fps, of which ~12 ms is engine and the rest is the
     software present path (BlitToBitmap + XAML composition)

MEASURED -- content, after scrolling 4800 px deep into the feed
  text, icons, avatars, product photos and video thumbnails all present;
  Cyrillic glyphs correct; lazy-loaded images resolved at depth.
```

Read those numbers together and the conclusion is specific: **the cost is in the software present
path, not in the engine, and it does not grow with scroll distance.** The engine has already done its
work by 12 ms; the remaining ~18 ms is spent getting a bitmap onto the screen. That is the case for
fixing GPU presentation, and also the case against optimising the engine further.

## Build lines

Two independent lines. They share the source tree and the SDK and **nothing else** -- not objects,
not generated bindings, not `build.ninja`, not the CMake cache.

```
  x64 (dev machine)                        ARM32 (Lumia 950, W10M)
  -----------------------------------     -----------------------------------
  . Src/setenv.ps1                        . Src/setenv.ps1
  . Src/port/x64-uwp-env.ps1              . Src/port/arm32-uwp-env.ps1
                                          Toolchain-ARM32-UWP-clang.cmake

  cmake -S . -B build-x64-gpu \          cmake -S . -B build-arm32-gpu \
        -G Ninja                                  -G Ninja
        -DCMAKE_TOOLCHAIN_FILE=...                -DCMAKE_TOOLCHAIN_FILE=...
  ninja -C build-x64-gpu -j1            <engine archives ALREADY BUILT>
        (hours; only when WebKit changes)      WebCore.lib JavaScriptCore.lib
                                             WTF.lib PAL.lib

  pwsh Src/port/build-driver-dll-       same shape, -Arch arm
       arm32.ps1 -Arch x64
       (minutes)                             (minutes)

  MSBuild /restore /p:Configuration=    same shape
       Debug /p:Platform=x64
       Src/Apotheosis/Apotheosis.csproj
       (minutes)
```

The engine archives for ARM32 are already built, so a driver rebuild on the ARM line does **not**
require the multi-hour WebKit build. See [BUILD-ARM32.md](BUILD-ARM32.md) and
[BUILD-X64.md](BUILD-X64.md) for the details and the known ARM traps.

## Honest status: what is proven, what is not

| | |
|---|---|
| **Proven on x64** | C# UWP appx builds, installs, runs; engine round trip; persistent session; live ticking; software present is visually correct; scrolling is flat and stable; scripted diagnostics work end to end |
| **Proven on Lumia 950** | previously: WTF + JSC with JIT, WebCore + Cairo software rendering of real sites, persistent touch/click/scroll, ANGLE direct-present. The C# line itself has **not** been run on the device |
| **Not working** | GPU presentation: `WebCoreGpuInit` returns `-20` and GPU present is disabled. The software path is the MVP-safe baseline |
| **Not verified** | ARM32 for the C# line, the ARM32 App Container ABI, the ARM JIT under the new shell, Cyrillic glyph fallback quality |

The rule from [INDEX.md](INDEX.md) applies without exception: **x64 results are necessary and not
sufficient.** The ARM32 App Container, the ABI, the JIT and the font stack all differ, and each has
already cost this project a separate bug.

## Known gaps in this document

Stated so the next reader does not have to guess which parts are measured:

- The coalescing ratio in the threading section is inferred from code, not instrumented.
- The ~30 ms per step of `swipe` includes the `await Task.Delay(16)` floor and the dispatcher
  round trip. It is an upper bound on the true per-step cost, and the split between present and
  engine is approximate.
- No device numbers appear anywhere above.
- The `-20` from `WebCoreGpuInit` is still unexplained. Its implementation has not been located.

## Risk register

| Risk | Assessment |
|---|---|
| The `WK_WINUWP` patch set is not reproducible from this repo | **Still the largest risk.** WebKit is gitignored; the patch series is not in git. Mitigation: `PATCH-INVENTORY.md` in Phase 1 of the old tracker. Everything else is secondary |
| GPU present stays broken | Acceptable for v1. [MVP.md](MVP.md) explicitly scopes software presentation as the MVP-safe path and GPU repair as out of scope |
| Device validation of the C# line | Mandatory before calling v1 done. Not started |
| Unrelated deleted harness files | Ownership unresolved; unrelated to this work and left untouched |
| Cyrillic glyph fallback artifacts | Cosmetic, present in screenshots, not yet triaged |



```


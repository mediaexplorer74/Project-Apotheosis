# The dzen.ru silent death

Status: **CLOSED — the killer is named, reproduced and explained end to end.**

Measured 2026-09-18 on the x64 bench (`build-x64-gpu`, `Harness` 0.1.9.105, ProcessArchitecture x64,
Windows 11 10.0.29634). Every figure below is read from `LocalState` of package
`EdgeHTMLReborn.Harness_edmb40rfkwsbg`, from the `cdb` log, or from the sources named inline.

## 1. The answer

`FontCache::systemFallbackForCharacterCluster()` — a **fork** stub in `Src\port\stubs-font-uwp.cpp:27` —
built a `FontPlatformData` from `description.computedSize()` **without checking for zero**:

```cpp
auto platformData = FontPlatformData(description.computedSize(), false, false);
return fontForPlatformData(platformData);
```

`FontDescription::m_computedSize` is `0` until style resolution sets it (`FontDescription.h:166`), and
`FontDataCacheKeyTraits::emptyValue()` **is** `FontPlatformData(0.f, false, false)`
(`FontCache.cpp:96-100`). So a zero computed size produces a `FontPlatformData` that is *byte-identical
to the hash table's own EMPTY sentinel*, and `HashTable::validateKey()` refuses exactly that with a
**release** assert, which is `WTFCrashWithInfo` → `abort()` → **process exit code 3**.

Scroll dzen.ru → some text is measured with a zero-sized `FontDescription` → the sentinel goes into
`m_fontDataCaches->data.ensure(...)` → release assert → `abort()`. It is not a memory error, not a
stack overflow, not a platform kill, and not upstream's code failing on its own — it is one unchecked
zero in the UWP font stub meeting an upstream invariant that is *stricter than a debug assert*.

## 2. The measurement chain

Each line was measured; together they leave no gap.

| Step | Evidence |
|---|---|
| The process died by `abort()` | `cdb` attached to pid 9416: `Last event: 24c8.1058: Exit process 0:24c8, code 3`. `0x24c8` = 9416. Code **3** is the CRT's `abort()` exit, not `0xC0000409` fail-fast, not `0xC00000FD`, not `TerminateProcess` |
| …because 0.1.9.105 had just disabled the fail-fast conversion | `DEATHNET: _set_abort_behavior -> prev=0x2` — `_CALL_REPORTFAULT` **was** on, i.e. before this build an `abort()` became a Watson fast-fail that runs no user handler. That is one full explanation of the earlier "silent" deaths, and it is why nothing was ever logged |
| The abort was reached from WebKit | `SIGABRT: tid=10184 -- abort() reached the CRT signal handler` at `14:49:50.557`, then a 40-frame `ABRT #n` stack in `log.txt` |
| The aborter is `WTFCrashWithInfo` | frame `#3 = WebCore+0x22ad`; `u WebCore+0x2290` shows `WTFCrashWithInfo [Assertions.h @ 986] … call qword ptr [WebCore!_imp_abort]` — it calls `abort()` directly |
| …called from the font cache | `ln WebCore+0x1b756b5` → `FontCache.cpp(264)+0x319`, i.e. inside `FontCache::fontForPlatformData` (symbol begins `WebCore+0x1b75370`) |
| Le site is a *release* assert on an EMPTY key | `ub WebCore+0x1b756b5 L16` decodes the sequence: `call HashTraitsEmptyValueChecker<FontDataCacheKeyTraits,0>::isEmptyValue<FontPlatformData>` → `test al,al` → `je <normal path>` → `lea rdx,[file]` / `lea r8,[assert]` / `mov ecx,216h` (line **534**) / `mov r9d,11h` → `call WTFCrashWithInfo`. **Falls through to the crash exactly when the key IS empty** |
| The assert names itself | the string at `r8` reads `void WTF::HashTable<WebCore::FontPlatformData, … WebCore::FontDataCacheKeyTraits>::validateKey(const ValueType &) [Key = WebCore::FontPlatformData …`; the string at `rdx` is `C:\Users\media\source\repos\Vibe\Apotheosis\build-x64-gpu\WTF\Headers\wtf/HashTable.h` |
| The second template argument is `0` | `WTF/HashTraits.h:302-304`: `HashTraitsEmptyValueChecker<Traits, false>::isEmptyValue(value)` is `value == Traits::emptyValue()` — the parameter is `Traits::hasIsEmptyValueFunction`, and `FontDataCacheKeyTraits` does not define one |
| …so "empty" means "equals `emptyValue()`" | `FontCache.cpp:96-100`: `static NeverDestroyed<FontPlatformData> key(0.f, false, false);` |
| …and only a zero size can satisfy that | `FontPlatformData.h:429-440` — `operator==` compares `platformIsEqual(other) && m_isHashTableDeletedValue && m_size && m_syntheticBold && m_syntheticOblique && m_isColorBitmapFont && m_orientation && m_widthVariant && m_textRenderingMode`. Every other field is defaulted identically by both the stub's 3-argument construction and `emptyValue()`; **`m_size` is the only one that can differ**, so `isEmptyValue()` is true iff `m_size == 0` |
| …and the constructor keeps that consistent | `FontPlatformData.cpp:176-185` (the fork's own comment says so): a zero size is deliberately left with a **null** `m_scaledFont`, so a `memset`-zeroed bucket compares equal to it (`emptyValueIsZero = true`, `FontCache.cpp:93`) |
| `computedSize()` really can be 0 | `FontDescription.h:49` returns `m_computedSize`; `FontDescription.h:166` initialises it to `{ 0 }`; it is only ever raised by the ~35 `setComputedSize()` call sites in `WebCore/style` and `WebCore/rendering` |

## 3. The exact code path

```
TextUtil::width
  FontCascade::width
    FontCascade::widthForSimpleTextSlow
      WidthIterator::advance
        WidthIterator::advanceInternal<SurrogatePairAwareTextIterator>
          FontCascade::glyphDataForCharacter
            FontCascadeFonts::glyphDataForCharacter
              FontCascadeFonts::glyphDataForVariant
                FontCascadeFonts::glyphDataForSystemFallback
                  SystemFallbackFontCache::systemFallbackFontForCharacterCluster
                    FontCache::systemFallbackForCharacterCluster    <- Src\port\stubs-font-uwp.cpp:27
                      apotheosisSetForcedBundledFontName("simhei.ttf")
                      FontPlatformData(description.computedSize(), false, false)   <- size 0
                      apotheosisSetForcedBundledFontName(nullptr)
                      FontCache::fontForPlatformData                 <- FontCache.cpp:258
                        HashTable::ensure -> validateKey             <- HashTable.h:534
                          WTFCrashWithInfo -> abort()                <- exit code 3
```

Above `TextUtil::width` the stack is ordinary layout: `InlineItemsBuilder::build` →
`InlineFormattingContext::minimumMaximumContentSize` → `LineLayout::computeIntrinsicWidthConstraints`
→ `RenderBlockFlow::tryComputePreferredWidthsUsingInlinePath` → `RenderBlockFlow::computeIntrinsicLogicalWidths`
→ `RenderBlock::computePreferredLogicalWidths`, and from there a **six-frame cycle that repeats exactly**
in the captured 40 frames:

```
RenderBlock::computePreferredLogicalWidths
  RenderBox::minPreferredLogicalWidth
    RenderBlock::computeChildIntrinsicLogicalWidths
      RenderFlexibleBox::computeChildIntrinsicLogicalWidths
        RenderBlock::computeChildPreferredLogicalWidths
          RenderFlexibleBox::computeIntrinsicLogicalWidths
            RenderBlock::computePreferredLogicalWidths      (next level)
```

That cycle is **not** a bug and not the cause — it is the ordinary nested-flexbox intrinsic-width
recursion, one iteration per nesting level of the DOM (dzen.ru's cards are deeply nested flex). Its
only role here is to be the deep stack the font fallback eventually runs on. **Do not read it as
unbounded recursion**: the capture stops at 40 frames, the cycle is simply still going.

## 4. The timeline, and the one thing still open

From the fatal 0.1.9.105 session (launched 14:42:00; dzen.ru rendered at 14:44:24):

```
14:44:18.711 .. 14:44:21.596   beat-stuck #2..#6  job=nav-load loading=1     <- the only stall in the session
14:44:21.596                   WEDGE: no progress for 6 beats -- dumping engine tid=10184
14:49:46.464                   ManipDelta dy=-1.0
14:49:48.414                   ManipDelta dy=-1.5
14:49:49.925                   last heartbeat: busy=1 pending=0 finished=815 job=live-tick tickstep=2
14:49:50.557                   SIGABRT: tid=10184
14:49:50.597                   exit code 3
```

Three facts to take from it:

* **The abort is 2.1 s after the last gesture and 0.6 s after the last heartbeat.** There is no
  evidence of a stall at the end: `beat-stuck` appears **21 times in the whole session, all of them
  between 14:44:18 and 14:44:21**, and never again. `finished` advanced 775 → 815 over the last
  minute. So the engine was making progress right up to the abort, and the crash is a *fast* path
  through layout→font-fallback, not a timeout. `live-tick` is what keeps re-laying-out dzen.ru's
  changing feed, which is why the crash needs it as a prerequisite — a static page never re-enters
  `computeIntrinsicWidthConstraints` for new nodes.
* **The reported ~30 seconds of a black window are not dated by any instrument.** `heartbeat.txt`
  holds exactly one line, rewritten each beat, so it has no history; and nothing else records
  presentation. What *is* recorded is that `gpuLogMarker` wrote `[GPU] readback failed -- falling
  back to Cairo` **550 times** in this 8-minute session, while the page nevertheless rendered
  normally before the scroll. Note also that **`[GPU] present failed rc=%d` appears nowhere** — so no
  direct present was ever attempted (`Src\port\WebCoreDriver.cpp:858-869`: the readback branch is
  only reached when `g_gpuDirectPresent` is false *or* when a present failed and logged its own
  marker). Whether the black window is the SwapChainPanel simply not being fed, the frame being
  genuinely empty, or something else is **open** — and it is a *presentation* question, independent
  of the crash. The instrument that would settle it: a per-beat present line (mode, size,
  `nonwhite`) in a file that is appended to rather than rewritten.
* **`gpuLogMarker` reaches the debugger.** `[GPU] readback failed …` appeared live in the attached
  `cdb`'s output, which means `gpuLogMarker` also calls `OutputDebugString` — and *that* is the
  `VEH-routine: code=0x40010006` (`DBG_PRINTEXCEPTION_C`, raised inside `KERNELBASE`) the first-chance
  handler counted thousands of times. The two observations are the same thing.

`live-tick` is necessary but not sufficient: the 14:22 session that survived also ran it. What makes
**dzen.ru** special is that its feed produces an element whose measured text arrives with a
zero-sized `FontDescription` — see §1.

## 5. The fix

`Src\port\stubs-font-uwp.cpp` must never hand the empty sentinel to the font cache. Two guards, both
mirroring invariants that already exist elsewhere:

```cpp
RefPtr<Font> FontCache::systemFallbackForCharacterCluster(const FontDescription& description, const Font&, IsForPlatformFont, PreferColoredFont, StringView)
{
    // Apotheosis 2026-09-18: computedSize() is 0 until style resolution sets it
    // (FontDescription.h:166), and FontDataCacheKeyTraits::emptyValue() IS
    // FontPlatformData(0.f, false, false) -- FontCache.cpp:96-100. Building the platform data from a
    // zero size therefore hands fontForPlatformData() the hash table's own EMPTY sentinel, which
    // HashTable::validateKey() refuses with a RELEASE assert (HashTable.h:534) -> WTFCrashWithInfo ->
    // abort(). Measured 2026-09-18: scrolling dzen.ru aborted the process with exit code 3 from
    // exactly this call. With no size there is no fallback face to offer, so decline.
    float size = description.computedSize();
    if (!(size > 0.0f) || size != size)
        return nullptr;

    extern void apotheosisSetForcedBundledFontName(const char*);
    apotheosisSetForcedBundledFontName("simhei.ttf");
    auto platformData = FontPlatformData(size, false, false);
    apotheosisSetForcedBundledFontName(nullptr);
    // A face that could not be built leaves m_scaledFont null. That is not the empty sentinel (the
    // size is non-zero), but caching a font-less platform data is still wrong: decline instead.
    if (!platformData.scaledFont())
        return nullptr;
    return fontForPlatformData(platformData);
}
```

`return nullptr` is the honest answer here and the caller already handles it: it is what
`FontCache::createFontPlatformData` in the same file returns unconditionally (line 22-25).

**This is a class, not an instance.** Any other fork code that constructs a `FontPlatformData` from a
CSS-derived size must carry the same guard, because `emptyValue()` is a *size* comparison and the
release assert is not conditional on `ASSERT_ENABLED`.

## 6. Which instruments found it, and which never could

**What named the killer, in order of decisiveness:**

| Instrument | What it delivered |
|---|---|
| `cdb` attached externally | the **exit code** — `3`, i.e. an `abort()`, which no in-process channel can report. This is the single measurement that re-aimed the whole investigation |
| `DEATHNET: _set_abort_behavior -> prev=0x2` (0.1.9.105) | proved that before this build every `abort()` became a Watson fast-fail running **no** user handler — the mechanism behind a whole class of "silent" deaths |
| `signal(SIGABRT, …)` + a 40-frame walk (0.1.9.105) | the stack, at the moment of death, flushed line by line |
| `WebCore.pdb` + `cdb -z` (`ln`, `u`, `ub`, `da`) | every frame named, plus `ecx = 0x216` (line 534) and the two string arguments, which is what turned "the font cache crashed" into "`validateKey` refused an empty key" |

**What was watching the wrong thing — do not trust it again without fixing it:**

* **The `wtfcash` tracer in `WTF/wtf/Assertions.cpp` cannot see this class of death at all.** It hangs
  off `WTFCrash()`. Every `ASSERT`/`RELEASE_ASSERT` in this tree goes through **`WTFCrashWithInfo`**
  instead — `Assertions.h:966` (the register-passing form, for targets with `CRASH_ARG_GPR*`) and, on
  x64, the `#else` branch at **`Assertions.h:982-988`**, which is literally
  `inline void WTFCrashWithInfo(int, const char*, const char*, int) { CRASH(); }` and on this build
  compiles to `call abort`. `apoCrashTrace()` is never reached. **Zero `wtfcash` lines in every saved
  session is therefore not evidence that `WTFCrash` did not fire — it is evidence that the tracer was
  instrumented on the unused twin.** If that channel is kept, hook `WTFCrashWithInfo` too.
* **The JSC watchdog is exonerated by counting.** `apoArmJsWatchdog()` is called from the top of every
  `WebCoreLiveTick`; the 14:17–14:27 session logged `js watchdog arm` **1479 times** and
  `js watchdog fired` **0 times**. Whatever `Watchdog::startTimer` does with 1479 re-arms, it never
  reached the callback, so no `TerminatedExecutionException` was thrown into the interpreter.
* **The wedge dumper is not a crash detector.** It fires on 6 missed beats. The killer took 0.6 s
  after a normal beat, so no dump was ever going to appear, and none did.

## 7. Negative results worth not repeating

* **Synthetic mouse input does not reach this app's XAML stack.** `SetCursorPos` and `SendInput` both
  verifiably move the cursor, and a full press-move-release produces **zero** `ManipulationDelta`
  lines, while the maintainer's own drag on the same page in the same session produces them at once.
  `InjectTouchInput` returned FALSE with `err=0`. `Src\tools\synth-scroll.ps1` exists and is unproven.
  A scripted destination is not a scripted gesture — the same lesson as `nav.txt`.
* **`Get-Process … MainWindowHandle` is 0 for a UWP app.** It owns no top-level window; the frame
  belongs to `ApplicationFrameHost` and the app owns a child of class `Windows.UI.Core.CoreWindow`.
* **`SetProcessDpiAwarenessContext` is a no-op once the host has a context.** `pwsh` does, so
  `GetWindowRect` stays virtualised (measured: 2748x1739 reported on a 1824-pixel-tall desktop). Use
  `SetThreadDpiAwarenessContext(-4)` and `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS=9)`.
* **The nested-flexbox frame cycle is not recursion gone wrong.** See §3. It repeats because it is
  one iteration per DOM level, and the capture stops at 40 frames.
* **`AddVectoredExceptionHandler` is not available to an App-Container build** — declared in
  `<errhandlingapi.h>` only under `WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP |
  WINAPI_PARTITION_SYSTEM)`. Measured: `C3861: identifier not found`. ntdll exports the same function
  as `RtlAddVectoredExceptionHandler` (kernel32's own implementation calls into it) and it links via
  `#pragma comment(lib, "ntdll.lib")`. The first-chance handler proved its worth by exposing the
  `DBG_PRINTEXCEPTION_C` stream (§4), but it did **not** see the fatal event: an `abort()` is not an
  exception.

## 8. Where each diagnostic actually lives

Grepping a marker in the wrong module reads as "this code never ran". Measured by string-scanning the
packaged appx:

| Marker | Module |
|---|---|
| `TERMINATE: tid=`, `beat-stuck`, `WEDGE: no progress`, `js watchdog arm`, `UEF:`, `SIGABRT:`, `DEATHNET:` | `Harness.exe` |
| `WTFCrashWithInfo`, the whole `WebCore::FontCache` stub | `WebCore.dll` |
| `wtfcash: begin tid=%lu frames=%u` | `JavaScriptCore.dll` |

The port layer *other than the font stub* is linked from `WebCoreDriver-gpu-x64.lib` into
**`Harness.exe`**, not into `WebCore.dll` — which is why the engine thread's stack in an earlier
wedgedump showed `Harness.exe+13f38e` / `Harness.exe+1162e` frames. `stubs-font-uwp.cpp` is the
exception: it defines `WebCore::` symbols that `FontCache.cpp` calls, so it is compiled **into
WebCore**, and fixing it means a WebCore rebuild, not a driver relink.

## 9. How the death net got here

| Build | What it added | What it bought |
|---|---|---|
| ≤ 0.1.9.103 | `std::set_terminate`, `SetUnhandledExceptionFilter`, the wedge dumper, `beat-stuck` | nothing for this killer: it reaches neither terminate nor an unhandled exception, and it is not slow enough to wedge |
| 0.1.9.104 | `_set_invalid_parameter_handler`, `beat-stuck` reset on recovery | nothing for this killer |
| **0.1.9.105** | `RtlAddVectoredExceptionHandler` (first-chance), `signal(SIGABRT, …)`, `_set_abort_behavior(0, _WRITE_ABORT_MSG \| _CALL_REPORTFAULT)`, `_set_purecall_handler` | **everything** — the `SIGABRT` line and the 40-frame stack are what named this death, and disabling `_CALL_REPORTFAULT` is what turned a silent fast-fail into a diagnosable exit code 3 |

Every line the net writes goes to `log.txt`, which `LogWrite` opens, writes and closes per line, i.e.
it is flushed by construction. That is why the last line of `log.txt` really is the last thing the
harness said.

## 10. Verification on 0.1.9.106 / .107, and the things that are NOT the crash

The fix was rebuilt into `0.1.9.106` and the same gesture was repeated on the bench. **The crash is
gone:** two full passes down dzen.ru's feed and back, no `SIGABRT`, no exit code 3, no `WTFCrash` /
`RELEASE_ASSERT` string anywhere in the session, and `platformInit:` lines in `glyph.log` carry real
non-zero sizes (9.3, 10.7, 13.3, 16.0, 18.0, 22.0, 24.0, 32.0, `isFT=1`) — the guard did not disable
normal font creation. What follows are the *different* phenomena that a reader of the raw log will
otherwise mistake for the fix having failed — §10a a handled fault that fakes a hang under a debugger,
§10b-§10c the stall that is a real bug of its own, §10d the interpreter tier question, §10e the
JS-stack probe of `.108`/`.109` and the three measurements it made possible (including the finding
that the freeze is armed by a gesture, not by a URL), and §10f the root cause that probe led to —
Web Storage in this port accepted every write and stored nothing, which is what made dzen.ru's
settings-sync module unable to converge. **§10f now carries the confirmation run: the same gesture on
`0.1.9.110`, which did not freeze.**

### 10a. `stack copy failed` in a wedgedump is a handled fault, and under a debugger it fakes a hang

`WriteWedgeDump` copies 16 KB of the engine thread's stack (`uintptr_t words[2048]`,
`MainPage.xaml.cpp:1006-1007`) through `CopyStackWords`, whose `memcpy` sits inside `__try`/`__except`
(725-734). When the window runs into an unmapped or guard page the copy raises a **first-chance**
access violation (`c0000005`), the handler returns 0, and the dump records `stack copy failed`.

Measured: **four of the nine dumps in LocalState** (`10992`, `3640`, `5124`, `8456`) carry that line
and are otherwise complete — the process survived every one. It is not a defect and not a crash.

It cost a false hang on 2026-09-18. `cdb` was attached during the scroll; the first-chance AV stopped
**every thread of the process**, so the app looked hard-frozen — including the address bar, which the
maintainer reported as "О, браузер капитально завис" — and the wedgedump that was being written never
appeared, because the handler never got to run. `Doc/PLAN.md` §0b carries the timeline.

**Rules:** if a dump says `stack copy failed`, read the `unwind:` chain above it and move on. When
debugging near the wedge path, either run **without** a debugger or suppress the stop with
`sxd c0000005`. And never end a `cdb` session with `Stop-Process` — terminating a debugger kills the
debuggee with it; use `.detach`.

### 10b. The post-fix stall is real, and the JIT A/B refuted the interpreter explanation

With the crash fixed the feed still stalls for seconds at a time. The engine thread's stack was
sampled twice — once by the wedge dumper and once live, under a debugger, from a genuine freeze with
the UI thread healthy (`finished=682` frozen for 60+ s, `log.txt` silent since `15:29:12`, address bar
still usable). Both samples are pure JavaScriptCore:

* **Wedge dump, 15:10:41** — `SourceElements::emitBytecode` → `FunctionNode::emitBytecode` →
  `BytecodeGenerator::generate` → `UnlinkedFunctionExecutable::unlinkedCodeBlockFor` →
  `ScriptExecutable::newCodeBlockFor` → `prepareForExecutionImpl`, reached from `llint_virtual_call`
  ← `llint_entry`. That is lazy bytecode compilation happening **on the LLInt's own stack**, so every
  level of JS recursion pays a full compile.
* **Live hang, 15:29:48** — the engine thread in `slow_path_enter+0x6a`
  (`CommonSlowPaths.cpp:926`) ← `llint_entry+0x44b2`, and nothing else: the LLInt is generated code
  with no unwind info past its entry, so 2 frames is all the walker can ever get. `slow_path_enter` is
  trivial and non-blocking (it writes the scope register), so that sample proves the thread was
  **executing**, not waiting. All 28 other threads were parked: 12 in
  `KtWaitForAlertByThreadId`/`RtlSleepConditionVariableSRW`, the rest in XAML/DirectManipulation/compose
  waits, plus `libpas`'s scavenger. **No lock was held by anyone.** There is no deadlock to find here.

The material fact was that *that* session — and every bench session before it — ran with
`JSC_useJIT=false`, forced by the packaged `Src\harness\Assets\navseq.txt` (`jit=0`):

```
navseq: jit=0 from the package -- JSC_useJIT=false, the engine will interpret
```

The switch was deliberate (`MainPage.xaml.cpp:3760-3776`) and its comment claimed the x64 bench cannot
JIT because of DEP/ACG while the Lumia can — a claim inherited from the `.77` era and never
re-measured. **The experiment that settles it was run the same day, and it refuted the conclusion.**
`navseq.txt` now ships `enabled=0` with the `jit=0` line removed (JIT on, autoplay silent), the manifest
went to `0.1.9.107`, and the same gesture was repeated:

```
navseq: Assets\navseq.txt present but enabled=1 is absent -- not playing
```

dzen.ru rendered (`contents=1024x2096 body=1 nonwhite=302013/710656`, `js=1/1`), and the engine then
**wedged exactly as it had on the interpreter**: `finished` froze at `376` at `15:41:23.809`, six
`beat-stuck` lines, a WEDGE at `15:41:27.457`, and `WATCHDOG: … engineBusy=1 finished=376` every 40 s
afterwards. The JIT is not the remedy and interpreter speed is not the cause. §10c names what is.

### 10c. What the freeze actually is: a page JS runaway the watchdog fires on and does not stop

The wedged process from §10b was left alive and sampled repeatedly. Five independent channels agree,
and the arithmetic ties them together.

**1. The stack, top to bottom.** `dps` over the engine thread's stack, symbolised against the binaries
in `build-x64-gpu\bin` (`dps` resolves every word, so it is a *scan*, not a walk — treated as evidence
here only because the coherent run below was independently reproduced by a real `k` unwind of its
inner half, and because it agrees with two counters read from other files):

```
ntdll!RtlUserThreadStart
  KERNEL32!BaseThreadInitThunk
    Harness!WebEngine::loop                                    MainPage.xaml.cpp @ 1538 / 1535 / 1554
      Harness!WebCoreLiveTick+0x61                             WebCoreDriver.cpp @ 3738
        JavaScriptCore!WTF::RunLoop::cycle+0x13                RunLoopGeneric.cpp @ 473 / 471
          JavaScriptCore!WTF::RunLoop::runImpl+0x35c           RunLoopGeneric.cpp @ 410
            WebCore!ThreadTimers::sharedTimerFiredInternal+0x14b   ThreadTimers.cpp @ 212
              WebCore!CallableWrapper<lambda Timer.h:165>::call
                WebCore!WindowEventLoop::didReachTimeToRun+0x51    WindowEventLoop.cpp @ 233
                  WebCore!EventLoop::run+0x265                 EventLoop.cpp @ 339
                    … a WebCore task …
                      WebCore!EventTarget::dispatchEventForBindings   EventTarget.cpp @ 255
                        WebCore!jsEventTargetPrototypeFunction_dispatchEvent  JSEventTarget.cpp @ 293
                          JavaScriptCore!llint_entry               (JS frames)
                            WebCore!EventDispatcher::dispatchEvent  EventDispatcher.cpp @ 247
                              EventDispatcher::dispatchEventInDOM   EventDispatcher.cpp @ 117
                                EventContext::handleLocalEvents    EventContext.cpp @ 100
                                  EventTarget::fireEventListeners   EventTarget.cpp @ 317
                                    EventTarget::innerInvokeEventListeners  EventTarget.cpp @ 398
                                      JSEventListener::handleEvent  JSEventListener.cpp @ 198 **and** @ 240
                                        JavaScriptCore!llint_entry  (JS frames)
                                          JSC::runInternalMicrotask  JSMicrotask.cpp @ 900
                                            Document::processSpeculationRules  Document.cpp @ 4953
                                              MicrotaskQueue::performMicrotaskCheckpoint
```

Three things fall out of it. The outermost WebCore frame is `WebCoreLiveTick` **inside the first
`RunLoop::cycle()` of a tick** — line 3738-3739, the `kTickRunLoopCycle1` iteration. A page **timer**
(`ThreadTimers::sharedTimerFiredInternal` → `WindowEventLoop::didReachTimeToRun`) is what started the
JS. And `JSEventListener::handleEvent` appears **twice, at two different call sites**, i.e. two
listener invocations nested inside each other, entered from `dispatchEventForBindings` — the *bindings*
entry point, which only page JS reaches. **The port never calls `dispatchEvent` from C++** (grep of
`Src/port/*.cpp`).

**2. The tick step says the same thing from another file.** `WebCoreLiveTick` sets `g_liveTickStep`
before each stage (`WebCoreDriver.cpp:3700-3712`), and the harness writes it to `heartbeat.txt` every
2 s — deliberately, since it is a plain integer load that keeps working while the engine is wedged:

```
2026-09-18 15:53:29.023 beat busy=1 pending=2 finished=376 job=live-tick tickstep=1 stage=WE-job:lambda-done
```

`tickstep=1` is `kTickRunLoopCycle1`. **`job=live-tick` with `tickstep=1` means one live tick entered
its first `RunLoop::cycle()` and has not returned** — 12 minutes of `heartbeat.txt` writes, all of them
`s=1`. That is the freeze, described exactly: the engine thread is inside the tick, so no paint, no
layout, no `finished` increment, and no later tick can ever run.

**3. It is not a deadlock and not a stack overflow.** All other threads are parked (XAML, Direct
Manipulation, compose, `libpas`'s scavenger, `libcurl`'s scheduler in `multi_winsock_select`), and no
lock is held by anyone. RSP was sampled minutes apart and moved *up* by 928 bytes (`0x0affc9d8` →
`0x0affcd78`): the stack is oscillating inside ~1 KB, not growing. The whole chain is ~5 KB deep.

**4. It allocates without bound.** This is the measurement that changes the character of the bug. The
`WATCHDOG` lines carry the memory figure, and `finished=376` never moves:

| time | memory |
|---|---|
| 15:46:48.662 | 2 281 780 KB |
| 15:52:13.838 | 3 094 996 KB |
| 15:59:36.820 | 3 973 020 KB |

2 281 780 KB → 3 973 020 KB in 768 s = **~2.2 MB/s sustained, ~130 MB/min** (peak rate between the first
two rows: 2.5 MB/s). The process was left running until the figure was stable and then stopped
deliberately at `15:59`, after **18 minutes** in the same tick (`heartbeat.txt` still `tickstep=1`), at
3.79 GB working set and 971 s of CPU — it was heading for the 12.5 GB ceiling. Runaway JS execution that
never returns, not a slow one.

**5. The watchdog fires — and the script keeps running.** `gpuinit-steps.txt` arms the watchdog at the
top of every live tick (`WebCoreDriver.cpp:3732`, `apoArmJsWatchdog`) with a 15 s limit, and its last
two lines are:

```
[BUILD] js watchdog arm: vm=000001C39D03B000
[BUILD] js watchdog arm: wd=000001C39E0A1BC0
js watchdog fired: runaway script terminated
```

File mtime of that last line: **15:41:38.861**. The last tick that ever completed ended at
**15:41:23.809** (`finished=376`). The next tick armed the watchdog and entered `RunLoop::cycle()`; the
fire landed **15.05 s later**. The arming semantics are therefore confirmed to the tenth of a second —
and the callback that logs `runaway script terminated` returned `true`, i.e. JSC was told to terminate.
**`gpuinit-steps.txt` has not been written since.** No second fire, no marker, no return: the engine
kept executing the same JS for the next 20 minutes.

`Watchdog::shouldTerminate` returning `true` makes `VMTraps` take the `NeedTermination` path
(`VMTraps.cpp:488-498`): `vm.setHasTerminationRequest()` and `vm.throwTerminationException()`, which —
unlike ordinary exceptions — only **sets a pending JS exception** (`VM.cpp:1032-1044`). There is no C++
unwind in it. A pending exception stops JS frames, which unwind through the interpreter normally; it
cannot unwind the WebCore C++ frames in the middle of this cycle, and any C++ re-entry point that
clears the exception instead of propagating it re-enters JS with the loop still running. **Which
clearance point swallows it is not yet identified** — that is the open question, and it is a narrow one
now: the suspects are the exception-clearing calls WebCore makes around listener invocation
(`JSEventListener::handleEvent`, `JSExecState::didLeaveScriptContext`, `Microtasks.cpp:105`, and
`reportException` in the dispatch path).

**So the freeze is: page-authored JS enters a synchronous `dispatchEvent`/microtask cycle inside a
timer callback; the tick never returns; the watchdog correctly detects the runaway and its termination
request does not take effect.** Both halves matter — without a working watchdog a runaway would at
worst be a transient stall, and without the runaway there would be nothing to stop.

**6. The tier set is identical on both lines, so this was never an interpreter-vs-JIT story.** Both
`build-x64-gpu\CMakeCache.txt` and `build-arm32-gpu\CMakeCache.txt` say `ENABLE_JIT=ON`,
`ENABLE_DFG_JIT=OFF`, `ENABLE_FTL_JIT=OFF`, `ENABLE_C_LOOP=OFF` — **LLInt + Baseline JIT, no DFG, no
FTL, on the bench and on the phone alike.** The claim in `CLAUDE.md` that `build-x64-gpu` includes FTL
is wrong and has been corrected. `jitresult.txt`'s Thumb-2 text notwithstanding, the *tier* split that
`Doc/HARFBUZZ-ICU-DIVERGENCE.md` listed as the remaining divergence most likely to mislead an x64
rendering experiment does not exist; the JIT on/off switch does, and it is now off the table as an
explanation for this bug.

### 10d. LLInt on Windows 10 Mobile: present, and in assembly

The obvious follow-up question — "does the phone even have an LLInt?" — has a definite answer, from
generated artifacts rather than from documentation. `ENABLE_C_LOOP=0` on both lines, so the LLInt is
**not** the portable C loop: it is offlineasm-generated code
(`WebKit\Source\JavaScriptCore\offlineasm\arm.rb`). `build-arm32-gpu\JavaScriptCore\DerivedSources\LLIntAssembly.h`
is 6 031 174 bytes, generated 2026-08-23, and its guard names the target outright:

```
#if !OFFLINE_ASM_X86_64 && OFFLINE_ASM_ARMv7 && !OFFLINE_ASM_ARM64 && … && !OFFLINE_ASM_C_LOOP …
OFFLINE_ASM_GLOBAL_LABEL(vmEntryToJavaScript) // LowLevelInterpreter.asm:911
    "push { lr } \n"     "mov r7, sp \n"     "vpush.64 {d14, d15} \n"     "subs r3, r7, #128 \n"
```

ARMv7, `JSVALUE32_64` (`!OFFLINE_ASM_JSVALUE64`), Thumb-2 instructions, `vmEntryToJavaScript` present —
the same symbol that appears in every stack sample in §10c, and the reason a stack walk stops 1-2
frames deep there: the LLInt is generated code with no unwind info past its entry. The tier set above
explains the rest: `JSVALUE32_64` cannot host the FTL at all, and this build does not enable the DFG,
so the phone's JavaScript executes in the LLInt and the Baseline JIT.


### 10e. The JS-stack probe, and three measurements it made possible (2026-09-18)

§10c named the freeze's *shape* from a stack scan, but a `dps` word scan can only ever see the C++
frames: the LLInt is generated code with no unwind info past its entry (§10d), so the JavaScript
that is actually looping has never been named. `0.1.9.108`/`.109` add a probe that does name it, and
this section records what it proved and — more usefully — what it refuted.

**The probe.** `apoWalkJsStack` (`Src\port\WebCoreDriver.cpp`) walks `vm.topCallFrame` with
`JSC::StackVisitor::visit` and writes one `gpuLogMarker` line per frame — `<tag> #N url:line:col ::
functionName`, with native frames labelled. It is called from two places:

* `apoDumpRunawayJsStack`, inside the watchdog's `setTimeLimit` callback — the one hook guaranteed to
  run on the engine thread *during* a runaway, with the API lock held, before termination is
  requested. Once per process; the first fire is the interesting one.
* `WebCorePort::portDumpJsStack`, the self-test, called from `PortChromeClient::addMessageToConsole`
  when `LocalState\jstack.txt` exists (a file rather than a build flag, for the usual reason: it has
  to arm an already-installed build, and WDP cannot write LocalState on the Lumia). It exists because
  the watchdog probe can only fire during a hang, and "produced no output" would otherwise be
  indistinguishable from "the freeze did not happen".

`VM::topCallFrame` is the right anchor and it is maintained on the paths that matter: the LLInt's
`doVMEntry` (`LowLevelInterpreter64.asm:277`) stores the new frame into it on entry and restores the
previous one on exit, so while JavaScript is executing it is live.

**Two link facts, both measured, both of which will bite the next person.** `Frame::hasLineAndColumnInfo()`
is declared *without* `JS_EXPORT_PRIVATE` (`StackVisitor.h:112`) and is therefore not exported from
`JavaScriptCore.dll` — using it fails the link with `LNK2019`, and the fix is not to drop the check
but to inline its upstream body, which is literally `return !!codeBlock();`
(`StackVisitor.cpp:503`). `functionName()`, `sourceURL()` and `computeLineAndColumn()` *are* exported
and link fine. And `computeLineAndColumn()` is safe on a frame with no `codeBlock()` anyway — it
returns `{ }` early.

**Verified end to end, on a page built for the purpose.** `LocalState\jstacktest.html` (a hand-written
page, not part of the appx) logs from three nesting depths. Real output, `0.1.9.109`:

```
[JS] console/log jstacktest.html:10:16 jstack-probe: reached depth=1
jstack: vm=00000288CE03B000 topCallFrame=000000E8620FB750
jstack #0 <native> log
jstack #1 …jstacktest.html:10:16 :: innerMost
jstack #2 …jstacktest.html:13:14 :: middleLayer
jstack #3 …jstacktest.html:16:16 :: outerLayer
jstack #4 …jstacktest.html:31:15 :: probeListener
jstack #5 <native> dispatchEvent
jstack #6 …jstacktest.html:33:23 :: global code
jstack: logged 7 frame(s)
```

So function names, source URLs, line:column and the native-frame labels all work, and a
page-dispatched `dispatchEvent` nests exactly as §10c's stack says it does.

One negative result comes with it, and it is worth keeping: **`addMessageToConsole` is not reliably a
JavaScript context.** The first message that fired the self-test was a CSP violation emitted from C++,
and there `topCallFrame` was `0`. Console output *originating in JS* arrives with a live frame (as
above) — `FrameConsoleClient::addMessage` passes `JSExecState::currentState()` — but a message that
WebCore itself generates does not. A probe anchored here must tolerate an empty stack.

**Measurement 1: the watchdog DOES terminate a plain infinite loop.** `LocalState\runaway.html` runs
`while (true) i++` inside a `setTimeout` — the same entry path §10c recorded
(`ThreadTimers::sharedTimerFiredInternal` → a page timer), reduced to one frame. The watchdog fired and
the probe named it:

```
js runaway: vm=00000288CE03B000 topCallFrame=000000E8620FCDD0
js runaway #0 runaway.html:12:22 :: neverReturns
js runaway #1 runaway.html:19:17 :: runawayTimer
js runaway: logged 2 frame(s)
```

`topCallFrame` was **non-null** during the runaway, so the anchor is live in the case it was built
for. And then the loop *stopped*: `heartbeat.txt` returned to `busy=0 … tickstep=10` within seconds,
the page rendered (`nonwhite=18629`), memory stayed flat at 286 MB and the process went on ticking.
This corrects the framing of item 0a in `Doc/PLAN.md`: the open question is not "the watchdog fires
and nothing happens" — for a single-frame loop the requested termination works exactly as designed.
Whatever swallows it on dzen.ru does so because that loop crosses something a single-frame loop does
not.

**Measurement 2: a dispatch-plus-microtask mirror does NOT freeze.** `LocalState\dispatchcycle.html`
rebuilds §10c's cycle in the smallest form that should reproduce it: a listener queues a microtask,
the microtask dispatches the same event again, and WebCore's microtask checkpoint at the end of the
listener should make the nesting synchronous. It does not freeze. The page renders, `err=0`, `js=1/1`,
no watchdog fire, and the tick returns to `tickstep=10`. So `queueMicrotask` scheduled from inside a
listener is **not** re-drained as unbounded synchronous nesting in this build — the cycle that §10c
sampled is either task-chained (and therefore not a freeze on its own) or it depends on the
`runInternalMicrotask` → `processSpeculationRules` → `performMicrotaskCheckpoint` path, which is
WebCore's own microtask and not one page JavaScript can queue. The §10c stack's "two
`JSEventListener::handleEvent` call sites" remain solid; this only removes the cheapest explanation
of how they got there.

**Measurement 3: dzen.ru does not freeze without user interaction.** Two independent runs on
`0.1.9.109`, one of them in a freshly restarted process, navigated to `https://dzen.ru/` and were left
alone. Both rendered (`nonwhite` ≈ 301 000/710 656 and contents 1024x2096 with `bodyKids=64`,
`scripts=49`), `finished` climbed to 611 and then stopped moving, `tickstep` stayed 10, memory stayed
flat for five minutes, and no watchdog ever fired. Both runs were driven only through `nav.txt` —
that is, the port's own navigation path, with no gesture of any kind.

**The runaway is armed by interaction.** That is the honest conclusion, and it reframes the
reproduction: the freeze cannot be reached from a URL alone, which is why every scripted attempt so
far has missed it. The gesture the maintainer performed before the `.106`/`.107` freezes — a scroll,
and in one case a window resize immediately before the tick that never returned — is part of the
reproduction, not incidental to it. A test harness that drives `NavigateTo` and waits will not see
this bug; that limitation is already recorded for the bench in `CLAUDE.md` ("a scripted destination is
not a scripted gesture").

**A navigation change observed the same day, already documented elsewhere.** `https://dzen.ru/` now
lands in a redirect to `https://sso.dzen.ru/install?uuid=…`, and from there on to
`https://dzen.ru/?is_autologin_ya=true&sso_failed=blocked&uuid=…`, which is then cancelled
(`didFailLoading type=Cancellation`) and left blank (`nonwhite=0`, `body=0`). The page only renders on
the **second** navigation in a session. This is the same defect `Doc/PLAN.md` records under "dzen.ru
blank white page (SSO redirect loop)", with the same unproven cookie hypothesis; it is repeated here
only because of what it costs — the first `nav.txt` write after a launch is expected to be spent on
it, so a reproduction script must navigate twice.

### 10f. The root cause: this port's Web Storage was a silent no-op (2026-09-18, fixed in 0.1.9.110)

§10e ended with the runaway *named* but not *explained*: the probe showed a
`document.dispatchEvent` driven cycle inside dzen.ru's settings-sync module, with React's
`markUpdateLaneFromFiberToRoot` (`zi`) on the stack when the trap fired, and the watchdog's requested
termination not sticking. The question it left open was why a loop like that could not converge. The
answer is not in the page.

**The measurement.** `LocalState\lstest.html` is a five-line round-trip probe: write a key, read it
back, overwrite, read, remove, read, and the same for `sessionStorage`. On `0.1.9.109`:

```
ls: typeof=object length=0
ls: set+get=null length=0
ls: key(0)=null
ls: sessionStorage get=null
```

On `0.1.9.110`:

```
ls: typeof=object length=0
ls: set+get="abc123" length=1
ls: overwrite get="second"
ls: absent key=null
ls: after removeItem=null
ls: key(0)=null
ls: sessionStorage get="xyz"
```

Every line is now correct, including the two negative controls (`absent key` and `key(0)` after
`removeItem` must be `null`). The API was present, did not throw, and stored nothing.

**The mechanism.** The port never installed a `StorageNamespaceProvider`, so `Page` kept the one
`pageConfigurationWithEmptyClients()` puts there — `EmptyStorageNamespaceProvider`
(`WebCore/loader/EmptyClients.cpp:560`, installed at `:1293`). Its `EmptyStorageArea` is:
`length()` → 0, `item()` → `{ }`, `setItem(LocalFrame&, …)` → empty body, `contains()` → false. That
is the whole defect: **a write is accepted and discarded, and the next read cannot tell.** It is the
`SILENT LIE` class of stub — worse than an error, because the page has no way to detect it. The
fork's own comment at the enable site (`WebCoreDriver.cpp`, the DOM Storage block) records why
`setLocalStorageEnabled(true)` was turned on in the first place: SPA bundles raise
`ReferenceError("Can't find variable: localStorage")` without it. Enabling the *setting* made the
object exist; nothing supplied the *storage*.

**Why it is a synchronous infinite loop, not a slow one.** dzen.ru's settings-sync module
(`dzen-desktop-second.modern.bundle.js`, minified) is a read-after-write loop:

```js
u = e => { c() || new Promise(t => { o.p.setItem(i, JSON.stringify(e)), t() }).then(() => { document.dispatchEvent(s) }) }
p = () => { if (c()) return null; const e = o.p.getItem(i); return e ? d(e) : (f({settings:l,isFirstSet:!0}), l) }
f = e => { let {settings:t,isFirstSet:n} = e; const r = n ? null : p(); if (!r) return void u(t); … }
```

`p()` cannot return a value while `setItem` discards one, so `f()` always takes the
`isFirstSet` branch, which calls `u()` immediately, which writes (discarded) and dispatches the event
that re-enters the module. Its exit condition is *"the write is visible to the next read"* — the one
thing the port could not do. No released browser loops here, which is why this never reproduced for
anyone else and why the search kept landing on the page instead of the port. The loop is
`dispatchEvent`-driven and synchronous, which is exactly what §10c sampled and what §10e measured.

**The fix.** New port source `Src/port/PortStorage.{h,cpp}`: `PortStorageArea` (one origin's
`HashMap` + `Vector` in insertion order, with quota accounting against the namespace's quota —
5 242 880 bytes for both local and session storage), `PortStorageAreaMap` (origin → area, refcounted
so two namespaces that must share storage share it), `PortStorageNamespace`, and
`PortStorageNamespaceProvider`. Wired with one line at **all four** `Page::create` sites in
`WebCoreDriver.cpp` (`buildSession`, `WebCoreRenderHtml`, the top-level fetch path, and the C-ABI
self-test) — one line missed is one page with storage still lying.

Three decisions worth recording:

* **In-memory, and it says so.** The areas do not survive an app restart. This port's sqlite3 is a
  stub (`Src/port/stubs-sqlite.cpp`: every `sqlite3_open_v2` returns `SQLITE_CANTOPEN`, because
  libsqlite3 is unavailable under `WINAPI_FAMILY_APP`), so there is no database to persist into.
  A storage API that answers honestly about what it holds is worth more than one that reports success
  and forgets — and a page cannot tell the difference until the app restarts.
* **Storage events are dispatched properly.** Writes call
  `StorageEventDispatcher::dispatchLocalStorageEvents` / `…SessionStorageEvents`, which excludes the
  source window by identity. This is also the third confirmation that the `storage` self-loop is not
  the freeze mechanism (§10e refuted it by measurement; the event never reaches the writer).
* **The log is capped per area** (first 8 writes, then every 500th). A looping page writes without
  bound, and `gpuinit-steps.txt` is append-only on a device with little free space.

**What it changed on dzen.ru.** The run below writes what the broken build could never write:

```
storage: namespace local quota=5242880 session=1
storage: set origin=https://dzen.ru key=__ls_tracer_test len=16 writes=1
storage: del origin=https://dzen.ru key=__ls_tracer_test oldlen=16 removes=1
storage: set origin=https://dzen.ru key=tracer-device-id len=36 writes=3
storage: set origin=https://dzen.ru key=__pcode_page_visit_info_storage__ len=56 writes=1
storage: set origin=https://dzen.ru key=discover_recommend_heads len=2 writes=4
storage: set origin=https://dzen.ru key=dzen_pro_flow len=9 writes=5
storage: set origin=https://dzen.ru key=ludca len=128 writes=6
storage: set origin=https://dzen.ru key=beerka len=5 writes=7
storage: set origin=https://dzen.ru key=rb_sync_id len=9 writes=2
storage: set origin=https://dzen.ru key=ad-host-value len=9 writes=3
storage: set origin=https://dzen.ru key=pageThemeInfohttps://dzen.ru/ len=46 writes=6
```

`ludca` is the settings key the module above writes (`i`), `beerka` its experiments key,
`tracer-device-id` a UUID, and `__ls_tracer_test` dzen.ru's own availability probe — written and
removed in one turn, which is how a page asks "does this API actually work?" and gets an answer. The
run stayed at `busy=0` with a flat heartbeat.

**A link fact that cost a build, and belongs with §10e's two.** The port links WebCore's **import
library**, so only `WEBCORE_EXPORT`ed symbols exist at link time — an accessor that is plainly visible
in a WebCore header can still be unavailable. `WebCore::Frame::page()` is exactly that:
`LNK2019: unresolved external symbol "public: class WebCore::Page * __cdecl
WebCore::Frame::page(void) const"`. Its neighbours `Document::page()` and `LocalFrame::page()` are
not exported either. The replacement is `Page::forEachPage(const Function<void(Page&)>&)`, which *is*
exported; the session-storage dispatch walks it and takes the first page, and nothing is held between
calls, so there is no pointer to go stale. The same list is what `Doc/UNKNOWN-EXPORTS.md` records.

**What this does and does not close — and the confirmation run.** It removes the only mechanism found so
far that makes the §10c cycle non-convergent, and it makes the loop's exit condition reachable. §10a's
question — which C++ clearance point swallows the watchdog's termination — is now *less* load-bearing: if
the loop can exit on its own, a swallowed exception need not be explained for the freeze to be gone.

**Confirmed 2026-09-18 on `0.1.9.110`, by the maintainer's three touchscreen swipes down the dzen.ru feed
(17:57:56–17:58:00) — the same gesture that froze `.109`.** Read from `LocalState` afterwards, not from
feel:

| Signal | `.109`, after the gesture | `0.1.9.110`, after the gesture |
|---|---|---|
| `heartbeat.txt` | frozen: `tickstep=1`, `busy=1`, no `finished` advance | idle: `busy=0 pending=0 tickstep=10`, `finished` advanced 134 → 954 |
| `js watchdog fired` | present; the probe named the page loop | **absent for the whole session** |
| `storage: set origin=https://dzen.ru` | absent — the write was a no-op | present, 14 keys (above) |
| crash / `SIGABRT` / wedgedump from the gesture | one recorded | none |

Two honest qualifications. **First, the engine still stalled twice during the gesture** — `beat-stuck
#1–#2`, `job=live-tick`, `finished` stuck at 556 and at 392 for roughly 0.7 s each. In both cases
`finished` then **resumed advancing**, which is the known lazy-bytecode-compile scroll stall (§10b's
family), not the runaway; the frozen case is the one where it never resumes. **Second, it is one run**, and
the gesture is not mechanically repeatable — the bench is script-driven and a scripted navigation cannot
reach this path. Treat the table as strong evidence that the storage fix removed the trigger, not as proof
that no freeze remains; `jstack.txt` stays in LocalState so the probe remains armed.

One more measurement is worth keeping because it is easy to misread as a regression: the session contains
**one `WEDGE` dump, at 17:56:45, during the navigation** — six beats with `job=nav-load`, `finished` stuck
at 134. It cleared itself in two seconds (`[STAGE] after-load rc=0 compositing=1` at 17:56:47), and older
sessions show the same two `WEDGE` lines per run, so it is pre-existing load-time work rather than anything
introduced here. It does not have a root cause yet and is not claimed to be benign — only not new.

**Two side findings, recorded because they mislead.** `Src/port/stubs-crypto.cpp`'s header comment
claims it provides `PAL::CryptoDigest` and that `computeHash()` returns an all-zero digest. It does
neither: the file contains no `CryptoDigest` definition at all, and the real
`PAL/pal/crypto/win/CryptoDigestWin.cpp` (BCrypt) is compiled. The comment also points at
`port/undef-crypto.txt`, which no longer exists. A stale comment is cheap to fix and expensive to
believe — the all-zero digest it describes would be a genuine security defect, and it sent this audit
after a bug that was not there. Related, and positively confirmed: `cryptographicallyRandomValues`
ends in `wtf/RandomDevice.cpp`, whose `WK_WINUWP` branch uses `BCryptGenRandom`, so
`crypto.getRandomValues` is cryptographically sound here (`RandomDevice.cpp:40-41`, `:112-118`).


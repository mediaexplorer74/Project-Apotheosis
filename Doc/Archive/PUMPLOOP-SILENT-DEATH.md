# The crash inside pumpLoop: death with no exception, no dump, no UEF

Open defect as of 2026-08-21. Localised to a single call and a single source line; the mechanism is
still unproven. Written down because the locating took three days of dead ends, and because two of
the arguments that felt decisive along the way turned out to prove nothing.

## Signature

The process disappears. No minidump, and no `UEF:` line from the harness's unhandled-exception
filter, even with crash collection armed and confirmed (`{"CrashDumpEnabled":true}`, and the deploy
script now arms it automatically after every install so it cannot be forgotten). Three separate
crashes produced nothing between them.

```
15:56:45.653  [STAGE] after-load url=https://ya.ru rc=0 compositing=0     <- load #1 succeeded
15:56:45.669  EnableGpu: WebCoreGpuInit returned 0                        <- GPU becomes active
15:56:45.716  EnableGpu: no presentable GPU frame -- staying on software
15:56:45.815  NavRetry: replaying parked navigation https://ya.ru         <- load #2 starts
15:56:45.815  NavigateTo mem 142584/1572864 KB level=low
15:56:45.837  [STAGE] before-load https://ya.ru                           <- last harness line
              SL: DocumentWriter feed done                                <- last driver marker
```

## What the absence of a UEF line does and does not mean

It means nothing about where the fault is, and reading it as evidence cost part of a day.

The harness installs `SetUnhandledExceptionFilter` on the engine thread itself
(`MainPage.xaml.cpp`, inside `WebEngine::loop`), and that filter is thorough: exception code, the
faulting data address, every loaded module's base, and a `CaptureStackBackTrace`. So the filter does
cover the thread that died. Two things still bypass it:

- **`__fastfail`.** `0xC0000409` (STATUS_STACK_BUFFER_OVERRUN, i.e. a `/GS` cookie or a CFG check)
  is delivered through a path that never consults the unhandled-exception filter. This project has
  already been bitten by exactly that code once, during `frame->init()`, which is why
  `buildSession` sits inside `#pragma strict_gs_check(push, off)`.
- **An outright kill.** `TerminateProcess` by the platform raises no exception at all.

Note also what a UEF line would *not* have distinguished: `WTFCrash()` in a release build executes
`*(int*)0xbbadbeef = 0`, so **every `RELEASE_ASSERT` in WebKit surfaces as a plain access
violation**. The filter logs the faulting data address specifically to separate those two, which is
the right design — but only when it runs at all.

## Excluded by evidence, not by argument

- **The platform reaped a suspended app.** `App::OnSuspending` writes `exit-ok.txt`, and a directory
  listing of LocalState after the crash shows **no `exit-ok.txt`**. `Window::Current->VisibilityChanged`
  is logged on every transition and there is no `foreground=0` line either. The app never lost
  visibility and never suspended.
- **Memory.** The harness logs `AppMemoryUsage` on every navigation. Entering the fatal load it read
  **142584 KB of a 1572864 KB limit, level=low** — 9 % of the cap. ya.ru cannot claim the remaining
  1.4 GB in the 20 ms that followed.
- **The garbage StringImpl from the earlier investigation.** The `APOTRACE` probe around
  `StringImpl::convertToASCIILowercase` was added when a device crash arrived there with a 16-bit
  string claiming roughly 1.2 GB. It did not recur: the last probe block before this death is
  complete and healthy — `len=9 is8bit=1 first=image/gif`.

## Where, exactly

Two independent instruments agree, and they agree on the *second* load.

`gpuinit-steps.txt` shows load #1 reaching `SL: pumpLoop done` and load #2 stopping at
`SL: DocumentWriter feed done`, with the next statement being:

```cpp
pumpLoop(*localMainFrame, &g_session->load.mainDone, /*allowEarlyStopWithoutNav*/ true,
         /*settleCapTicks*/ 40, /*watchdog*/ 5.0, /*pageForRendering*/ page.ptr());
DBG_STAGE("pumpLoop done");
```

`port-trace.txt` then narrows it from that one call to a region inside it. The last
`pump: timers armed, entering RunLoop::run()` is at trace line 2623 of 2925 and has no matching
`pump: RunLoop::run() returned`, so **the death is inside `RunLoop::run()`, about 300 trace lines
in.** Those 300 lines are loader traffic: the new document's subresources being scheduled and
retired while `inflight` stays around 7.

The pump was one tick from finishing normally when it died — the last settle tick recorded
`loading=0 vua=1 quiet=15`, and the stop condition is `quiet >= 16`.

## The last frame, by name

The trace's `APOTRACE` blocks carry the return address of whoever asked for a lowercase string, and
those addresses are resolvable **after the fact, with no rebuild and no device access**, because the
harness already logs every module base at startup:

```
ModuleBase(Harness.exe)   = 00CE0000
LoadPackagedLibrary(JavaScriptCore.dll) = 668F0000
LoadPackagedLibrary(WebCore.dll)        = 60040000
```

The last block reads `APOTRACE String::lower this=18FADB04 impl=24D64520 caller=6005AEF3`. Subtract
the WebCore base, clear the Thumb bit, add the PE preferred `ImageBase`, and ask the symbolizer:

```
0x6005AEF3 - 0x60040000 = 0x1AEF3 -> clear Thumb bit -> 0x1AEF2 -> + ImageBase 0x10000000
llvm-symbolizer --obj=build-arm32-gpu\bin\WebCore.dll --demangle 0x1001AEF2
  WebCore::ResourceResponse::ResourceResponse(class WebCore::CurlResponse&)
  WebKit\Source\WebCore\platform\network\curl\ResourceResponseCurl.cpp:109
```

Line 109 is the MIME-type line, which is what the probe was reporting on:

```cpp
setMimeType(extractMIMETypeFromMediaType(httpHeaderField(HTTPHeaderName::ContentType)).convertToASCIILowercase());
```

Three details make this recipe reusable, and each one silently produces nonsense if missed: ARM32
call sites are **Thumb**, so the recorded return address is odd and the real address is one lower;
`llvm-symbolizer` wants the address relative to the PE **preferred** `ImageBase` (0x10000000 here),
not the runtime base and not a bare RVA — a bare RVA answers `??:0:0`; and the PDBs must be the ones
that went into the deployed appx (here `WebCore.dll` dated two days before the package, with no
engine rebuild in between, so they matched).

## What makes load #2 different from load #1

The same URL, in the same session, one second apart: the first completes, the second dies. Three
things changed in between, and they are not independent — all three are consequences of load #1
finishing, and all three are triggered from `MainPage::OnNavDone`:

1. **`WebCoreGpuInit` succeeded**, so `g_gpuActive` is true and `buildSession` now passes
   `setAcceleratedCompositingEnabled(true)`. Load #2 is the first session on the device ever built
   with accelerated compositing on. (Force-compositing stays off — that is the separate,
   already-fixed defect in `Doc/BLANK-SECOND-LOAD.md`.)
2. **The load is a replay of a parked navigation.** `NavigateTo` arrived while the engine was busy
   (`loading=1`), parked it, and `NavRetry` replayed it after the GPU callback. So `WebCoreSessionLoad`
   runs `teardownSession` while the previous page still has requests in flight.
3. **An update check is running concurrently.** `OnNavDone` calls `CheckForUpdate(false)` once, which
   spawns a `std::thread` that calls `WebCoreDownload` — a driver C ABI entry point — off the engine
   thread, against the project rule that all C ABI calls are serialised there. `update.json` is
   timestamped 15:56:45, the same second as the fatal load. `WebCoreDownload` is self-contained
   (its own `curl_easy` handle, no WebCore objects), so this is a rule violation of unclear
   consequence rather than a demonstrated cause — but it is a thread nobody accounted for, running
   during the window.

Suspect 2 has the clearest mechanism: `teardownSession` does call `stopAllLoaders()` and then cycles
the RunLoop four times to drain callbacks, so the author anticipated it — but the loader is **our**
`PortPlatformStrategies` scheduler holding raw `ResourceLoader*` in per-host queues, not WebCore's
default, and whether its cancellation actually retires everything in flight is unverified.

## Next steps

Ordered by information per device cycle, which over this phone's WiFi is about twenty minutes each.

1. **Get one uncontaminated reproduction.** The 0.1.9.30 session that produced all the evidence above
   had `enabled=1` in the packaged `navseq.txt`: the address player was driving the browser while the
   maintainer typed URLs by hand, so player navigations and manual ones overlapped, which is itself a
   way to manufacture suspect 2. It also had no `keepawake` line. 0.1.9.33 ships `enabled=0` and
   `keepawake=1`. Reproduce by hand from the tile, screen on, and confirm the crash survives without
   the player.
2. **Switch the suspects off one at a time.** All three are reachable from the harness alone, which
   means a fast build and no engine rebuild: skip `CheckForUpdate`, refuse the parked replay until
   the loader has drained, or leave the GPU off (`gpudefault=0`). Whichever one makes the crash
   disappear names the region. Note the delivery constraint: the switch has to ship in the package,
   because Device Portal cannot write LocalState.
3. **Only then instrument WebCore.** A marker after the `setMimeType` line and one after each
   `teardownSession` step would split the remaining window, but it costs a WebCore rebuild, and the
   evidence above did not need one.

Do not reach for dumps or exception filters again for this defect: the two things that produced every
fact in this document were per-line flushed markers and after-the-fact symbolisation of addresses that
were already in the logs.

## The clean reproduction (0.1.9.33), and what it moved

Reproduced by hand from the tile, screen on, address player disabled (`enabled=0`) and `keepawake=1`
actually delivered for the first time. The crash survived both changes, so **the player was not the
cause** — but the point of death moved, and that is the informative part:

| | 0.1.9.30, player running | 0.1.9.33, hand-driven |
|---|---|---|
| load #2 | died **inside** `pumpLoop` | completed: `SL: pumpLoop done`, `exit rc=0` |
| GPU | `Composite=-12`, stayed on software | `EnableCompositing=1 Composite=0`, **presenting through the GPU surface** |
| last symbolised frame | `ResourceResponse::ResourceResponse(CurlResponse&)` | `ParsedContentType::setContentTypeParameter` |
| when | inside the pump | ~196 trace lines **after** the last pump returned |

Both last frames are HTTP response-header handling, and in both the string being handled was intact
(`len=9 first=image/gif`, then `len=7 first=charset`; `words[0]` of a `StringImpl` is its refcount and
read 6 and 4 — no corruption visible at the probe).

One further detail in the clean run: the last two `APOTRACE` blocks report `this=283FF8C4`, while
every earlier block in the same run reports `this=18AFxxxx`. Those are 250 MB apart, far beyond the
engine thread's 16 MB stack, so **the final activity was on a different thread**. That is not by
itself an anomaly — `CurlRequest` parses headers on a worker thread by design and marshals the result
back through the main RunLoop — but it does mean the last code to run was not on the thread the
driver serialises everything else onto.

`MainPage`'s `latediag` timer was checked and cleared: it posts through `WebEngine::instance().post`
and calls `WebCoreGetDiag` on the engine thread, as the C ABI requires.

## The current hypothesis, and the instrument built to test it

Between loads **nothing pumps WTF's main RunLoop.** `pumpLoop` has returned and the engine thread is
asleep on its own job queue. A response that finishes after `WebCoreSessionLoad` returned is therefore
parsed on the curl worker, marshalled into the main RunLoop's queue, and sits there until something
next cycles the loop — the next `pumpLoop`, a `pumpQuick`, or `teardownSession`'s own four cycles. If
any of it then refers to a Page, Frame or FrameView that teardown has freed, the result is a
use-after-free whose timing depends on the allocator and the memory model. That last part is also why
the bench cannot reproduce it: x86-64 is strongly ordered and usually gets away with what ARM32's weak
ordering does not.

`analyze-loader-trace.ps1` on the clean run reports **18 loads started and never retired**, including
`ya.ru`, `counter`, `click` and `rythm-feed.js`. Consistent with the hypothesis, not proof of it: some
of those may simply have been cut off when the process died.

The instrument (driver + loader strategy, no WebCore rebuild) is a **session generation counter**:

- bumped once per `teardownSession`, after the session is really gone;
- stamped on every scheduler line as `g<n>`;
- each load remembers the generation it started in, in a `HashMap<void*, unsigned>` keyed by loader
  address, and `remove()` compares. Entries are taken on retire, so a recycled loader address cannot
  be confused with its previous owner — the pairing trap already recorded in CLAUDE.md;
- a mismatch prints `loader: -<ptr> g1 h=1 STALE started-in-g0`, which is the evidence, stated inline
  rather than left to offline pairing;
- plus `loader: MARK <tag> gen=N started=N inflight=N pending=N hosts=N` at `pumpLoop done`,
  `teardown enter`, `teardown after stopAllLoaders` and `teardown after drain` — so "how much was
  still running when the session died, and did stopping the loaders plus four cycles retire it" is a
  number rather than a guess.

`analyze-loader-trace.ps1` was updated in the same change, because the new `g<n>` field sits exactly
where its regex expected the resource name. The field is optional in the pattern so traces captured
before this build still parse — verified against the 0.1.9.33 trace, where resource names still come
back as `click` and `ya.ru` rather than `g0 click`.

## What the generation counter actually showed (0.1.9.34), and the trigger it exposed

The instrument refuted its own hypothesis, which is worth stating plainly:

```
loader: MARK pumpLoop done                 gen=0 started=29 inflight=12 pending=3
loader: MARK teardown enter                gen=0 started=29 inflight=12 pending=3
loader: MARK teardown after stopAllLoaders gen=0 started=31 inflight=0  pending=0 hosts=1
loader: MARK teardown after drain          gen=0 started=31 inflight=0  pending=0 hosts=0
```

- **First half confirmed:** the load is declared finished with **12 of its requests still in flight**.
- **Second half refuted:** `stopAllLoaders()` retires all twelve, the four RunLoop cycles leave the
  scheduler empty, and **not one `STALE` line appears** in the whole trace. Every `+`/`-` after the
  teardown carries `g1`. Nothing the scheduler knows about crossed a session boundary.

An incidental oddity: `started` goes 29 → 31 *during* `stopAllLoaders`. Cancelling a load calls
`remove()` → `scheduleServe()`, which starts pending ones — so teardown briefly starts new requests
before retiring them. Harmless here, but it means "stop everything" is not monotonic.

The death is again inside `RunLoop::run()` of the second load, and this time the last settle tick
**completed in full** — `tick enter`, `isolatedUpdateRendering ok`, `microtaskCheckpoint ok`,
`tick exit (quiet=14)`. So the fault is not in the pump's own callback; it is in something the
RunLoop dispatched between our ticks: a DOM timer, script execution, or a marshalled loader callback.

### The trigger: every address-bar navigation loads the page twice

Visible in the harness log all along, and missed because both lines read identically:

```
19:18:11.951  NavigateTo: url=https://ya.ru pushHistory=1 loading=0   <- runs
19:18:11.979  NavigateTo: url=https://ya.ru pushHistory=1 loading=1   <- 28 ms later, parked
19:18:11.983  -> parked https://ya.ru
19:18:16.108  NavRetry: replaying parked navigation https://ya.ru     <- and this one kills it
```

One tap produces two `NavigateTo` calls for the same URL 28 ms apart — far too fast to be two taps.
The first loads the page; the second is parked by the "engine is busy" branch, which exists for a
request aimed *elsewhere* during a slow load, and `ScheduleNavRetry` then replays it. So the fatal
load was **redundant**: the user asked once and got two full fetches and two session builds.

It also explains the timing that made this look like a GPU defect. `NavRetry` fires from the
`EnableGpu` callback, 68 ms after `WebCoreGpuInit` returns, so the replayed load is always the first
session built with `setAcceleratedCompositingEnabled(true)`. The GPU work did not create the duplicate
— the two `NavigateTo` lines are four seconds *before* `EnableGpu` runs — but it decided *when* the
duplicate runs.

### Why the bench never reproduced it, correctly this time

Not luck. **The bench is driven programmatically and the phone is driven by hand.** `x64-cycle.ps1
-Url`, the `nav.txt` watcher and the packaged `navseq.txt` player each call `NavigateTo` exactly once;
the double-fire needs a UI event, which the bench never generates. The precondition simply never
occurred there.

That is a sixth divergence between the two lines, and unlike the five in
`Doc/HARFBUZZ-ICU-DIVERGENCE.md` it is not in the build at all — it is in the **input path**. A defect
that needs a real tap cannot be found by a script that fakes the destination but not the gesture.

### The change

`MainPage::NavigateTo` now drops a request that is a duplicate of the load already running (same URL,
`m_loading` true, not `about:home`) instead of parking it, and logs a `CaptureStackBackTrace` first so
the doubling handler is named rather than guessed — the harness logs its own module base at startup,
so those addresses resolve against `Harness.pdb` offline.

The narrow test matters: parking still works for a request aimed somewhere else during a slow load,
which is what it was added for, and a deliberate repeat of the current page goes through `Reload()`,
not through here.

What this does **not** establish: whether the crash is *caused* by the second session build or merely
*revealed* by it. If dropping the duplicate makes the crash disappear, the fault underneath is still
there, waiting for any other path that builds a second session — a link tap, a redirect, history
navigation. The double-load was the trigger, and fixing a trigger is not the same as fixing a defect.

## Dropping the duplicate did not stop it (0.1.9.35)

The guard worked — `NavigateTo: duplicate of the load in progress -> dropped https://ya.ru`, no
`NavRetry`, and `gpuinit-steps.txt` shows exactly **one** session load instead of two — and the process
still died. So the second session was a red herring: a real defect, now fixed, but not the cause.

What is left is the pattern that has held in every run:

```
loader: MARK pumpLoop done gen=0 started=30 inflight=9 pending=0 hosts=3
```

The load is declared finished with **9 requests still in flight** (12 and 11 in the other two runs).
After that the engine thread leaves the pump, the loader keeps starting and retiring requests, MIME
types keep being lowercased, and ~118 trace lines later the process is gone. The two symbolised callers
are the same as before: `ResourceResponse::ResourceResponse(CurlResponse&)` and
`ParsedContentType::setContentTypeParameter`.

## The bench, finally in the phone's configuration, survives

Two tooling defects had been hiding this and both are now fixed (see items 6 and 7 in
`Doc/HARFBUZZ-ICU-DIVERGENCE.md`): the bench was driven only by scripts, never by a gesture, and
`x64-cycle.ps1` seeded `gpudefault=0`, so compositing had never been on for any bench result.

With `-Gpu` and a hand-typed URL the bench ran the same sequence:

| | Lumia, 0.1.9.35 | bench, 0.1.9.37 |
|---|---|---|
| navigations | one (duplicate dropped) | one |
| `WebCoreGpuInit` | returned 0 | returned 0 |
| first frame | `EnableCompositing=0 Composite=-12` | identical |
| present path | software | software |
| outcome | **dead** | alive, `rs=C`, page painted |

So the crash is **not** about enabling the GPU, and it is not reproducible on x64 even with the GPU
path armed. The measurable difference is state, not code: at the same moment the bench is at `rs=C`
with `pending=1`, and the phone is at `rs=I` with `pending=10`. The device is slow enough that the
pump's quiet counter (16 ticks, 0.8 s) expires while a third of the page is still loading; the bench
finishes first. Every death so far has happened inside that window — the one where the loader runs and
nothing pumps the RunLoop.

## What is left, and the test now running

Ruled out by evidence today: app suspension, the memory cap, a garbage `StringImpl`, the parked-navigation
replay, the second session build, stale scheduler callbacks, and enabling the GPU. What remains:

1. **The JIT.** It works on the Lumia (the probe returns 42 through both RW→RX and RW→RWX) and is
   blocked by DEP/ACG on the bench, so the device executes compiled code where the bench interprets.
   That is now the largest known difference. A fault in generated code would also explain the silence:
   it bypasses the unhandled-exception filter, and this OS build produces no dump.
2. **ARM32 itself** — weak memory ordering against x86-64's strong ordering, which forgives races the
   device does not.

0.1.9.38 tests the first directly: `jit=0` in the packaged `navseq.txt` sets `JSC_useJIT=false` before
`JSC::initialize()` runs, so the same build interprets instead of compiling. If the crash disappears the
fault is in or around generated code; if it survives unchanged, the JIT is exonerated and (2) is next.
The switch costs real speed and is marked in the file for reversion — it is a diagnostic, not a setting.

## 2026-08-22: it is a HANG, not a crash, and it is in the live tick

Two measurements settled what three days of tracing could not, and both were cheap.

### The process outlives the last trace line by 4 to 7 seconds

The harness writes `heartbeat.txt` from a 2-second UI-thread timer, and the crash-verdict code at the
*next* launch reports its contents. Lining those up against the logs:

| run | last log line | last heartbeat |
|---|---|---|
| 15:56 | 15:56:45.8 | 15:56:50.0 |
| 18:09 | 18:09:49.7 | 18:09:56.4 |
| 20:04 | 20:04:16.2 | 20:04:20.8 |

So the trace does not stop because the process dies; it stops because the **engine thread** stops
answering, and the process is killed several seconds later. An unhandled exception cannot produce that
— it would take the process down at once, and the UI timer could not have beaten seven more times.
This also finally explains the absent `UEF:` line and the absent dump without appealing to
`__fastfail`: the likely killer is a timeout somewhere reacting to the wedged thread, and
`std::terminate` under `_HAS_EXCEPTIONS=0` is an immediate `abort`.

**Every earlier conclusion of the form "it died here" was therefore reading a hang as a fault.** The
symbolised frames (`ResourceResponse`, `ParsedContentType`) are where the engine thread went quiet, not
where it crashed.

### The heartbeat now describes the engine, from the thread that is still alive

`heartbeat.txt` carries `busy` / `pending` / `finished` / `job` / `tickstep` / `stage`, all read by the
UI thread from atomics the engine already published. Deliberately **no job is posted to the engine to
prove it is alive**: the live-mode tick skips itself while `pending() > 0`, so a probe that queued one
beat every 2 s would suppress a real code path.

Read it like this:

- `busy=1` and `finished` not advancing — wedged **inside** a job, and `job=` names which.
- `busy=0` with `pending>0` — the thread is gone while work piles up behind it.
- `busy=0`, `pending=0` — idle, nothing to be stuck on.

Two crashes in a row, with `jit=1` and a single navigation, reported the same thing:

```
beat busy=1 pending=0 finished=8 job=live-tick stage=WE-job:lambda-done
beat busy=1 pending=0 finished=6 job=live-tick stage=WE-job:lambda-done
```

**The engine hangs inside the `live-tick` job.** Not GPU init, not the diag snapshot, not the resize.

### Two false trails from the same morning, recorded because both felt conclusive

1. **The resize path.** `EnableGpu` calls `ApplyViewportSize()` immediately after deciding to stay on
   software present, and CLAUDE.md warns that the GPU resize marshals into the panel dispatcher and
   that a timeout there calls `std::terminate` — a perfect fit. It is wrong: `ApplyViewportSize` logs
   `ApplyViewportSize: WxH session=... gpu=...` **before** posting anything, and that line is absent
   from both dying logs. It returned early (the size had not changed) and posted no job.
2. **`stage=` as the name of the stuck job.** Only jobs that call `WriteStage` update it, and the
   live tick does not, so it reported `WE-job:lambda-done` from an earlier navigation job. That is what
   pointed at the resize in the first place. Fixed by labelling all nineteen `post()` call sites --
   label first in the argument list, because putting it after the lambda would have meant editing
   nineteen closing `});`.

### What is instrumented now, and what each answer would mean

`WebCoreLiveTick` is seven distinct operations, and the step is published as a plain integer
(`WebCoreGetLiveTickStep`, declared in **both** copies of the ABI header) rather than as marker lines:
the tick runs several times a second, and per-step file I/O on the device's flash would alter the
timing being measured. The harness prints it as `tickstep=`.

| tickstep | operation | what a hang there would mean |
|---|---|---|
| 1–3 | `RunLoop::cycle()` | a queued loader callback never returns — this is where the curl responses that ended the trace are dispatched, so it is the leading candidate |
| 4 | `isolatedUpdateRendering` | rAF / IntersectionObserver / script |
| 5 | frame, view, document lookups | should be instant; a hang here would be extraordinary |
| 6 | `performMicrotaskCheckpoint` | promise or module evaluation |
| 7 | `updateLayoutIgnorePendingStylesheets` | layout |
| 8 | `countPendingResources` | should be instant |
| 9 | `paintToRGBA` | Cairo paint |

Shipped in 0.1.9.44. Also in that build: the startup overlay reads its version from
`Package::Current->Id->Version` instead of the literal `v0.1.8.25` frozen there roughly a hundred
builds earlier — which had been showing the wrong version on screen during exactly the
deploy-and-reproduce loop that relies on it to confirm which package is running.

## 2026-08-22, evening: the wedge is on RunLoop::m_loopLock

Two hypotheses died with clean negative results first, both worth keeping:

- **The curl scheduler's main-thread wait** (`startOrWakeUpThread` → `m_thread->waitForCompletion()`).
  Probed directly; **zero `curlsched:` lines** in the whole session, so the wait was never entered — the
  worker was always live and the early return always won.
- **A lock conflict on `m_multiHandleMutex`.** Dead by inspection: the worker polls with
  `selectTimeoutMS = INT_MAX` but does **not** hold that mutex across the poll, so `wakeUpThreadIfPossible`
  from the main thread cannot block on it.

Then the probe moved down into WTF's `RunLoop` itself: paired `rl: task N enter/exit` around every
dispatched function, and `rl: timer <description> enter/exit` around every firing timer (timers carry a
description; dispatched functions do not, so the index is their identity). The counter is a file static,
**not** a `RunLoop` member — adding a member would change `sizeof(RunLoop)` while `WebCore.dll` is
compiled against the old header, which would be memory corruption introduced by a diagnostic.

The run with that probe reported something different:

```
beat busy=1 pending=0 finished=3 job=nav-load tickstep=0 stage=before-load https://ya.ru
```

`job=nav-load`, not `live-tick`; `tickstep=0`, so no live tick was running at all. Every one of the 202
dispatched tasks was paired. The trace's last line is `pump: RunLoop::run() returned` and
`gpuinit-steps.txt` stops at `DocumentWriter feed done` — `SL: pumpLoop done` never appeared. That pins
the wedge to this window, which is four statements wide:

```cpp
    RunLoop::run();
    WebCorePortTrace("pump: RunLoop::run() returned");   // last line in the trace
    settle.stop();                                       // <-- takes m_loopLock
    watchdog.stop();                                     // <-- takes m_loopLock
}                                                        // timer destructors
DBG_STAGE("pumpLoop done");                              // never ran
```

`RunLoop::TimerBase::stop()` is `Locker locker { m_runLoop->m_loopLock }; stopWithLock();`. So the engine
thread blocks acquiring `m_loopLock` while another thread holds it — and the other party is the **curl
worker**, which calls into the main RunLoop to deliver completions. That closes the circle on every
earlier symbolisation: `ResourceResponse::ResourceResponse(CurlResponse&)` and
`ParsedContentType::setContentTypeParameter` both run on that worker. We had been looking at the other
side of the contention without recognising it as a side.

Note the observer effect, which is itself evidence: two file appends per dispatched task changed the
timing enough to move the manifestation from `live-tick` to `nav-load`. A defect that relocates when
timing changes is a race, not a logic error.

**Next step (not yet done):** trace `m_loopLock` acquisition and release with thread ids on both sides —
engine thread and curl worker — to name the holder. If confirmed, the fix is on our side rather than
upstream's: `pumpLoop` stops a repeating timer from the same thread that just left `RunLoop::run()`,
while the worker is still delivering into that loop. Stopping the timers *before* leaving the loop, or
draining the worker first, both avoid the window.

## 2026-08-23: m_loopLock exonerated; both deaths land inside the shared-timer batch

0.1.9.47 carried the instrument the previous section asked for: every acquisition of `m_loopLock`
goes through an RAII wrapper that tries first and writes an `rllock:` line only when it blocks,
naming site, blocked tid and last recorded owner (file-static atomic — no `RunLoop` member, for the
usual `sizeof` reason). Uncontended acquisitions cost nothing extra. Plus per-statement markers in
the `pumpLoop` tail (`pump: settle.stop enter/done`, `watchdog.stop done`).

Two hand-driven reproductions, same package. The results:

1. **The lock is innocent.** Exactly one `rllock: BLOCK` in the whole session —
   `site=wakeUp tid=6040 owner=4940 waited<1ms` early in load #2, i.e. the engine briefly met the
   curl worker's microsecond-long hold inside `dispatch()`→`wakeUp()`. It resolved immediately.
   Neither death shows any block at all. The previous section's hypothesis — the worker holding
   `m_loopLock` while the pump stops its timers — is dead, and so is the lost-wakeup variant:
   the engine thread never even reached for the lock when it went quiet.
2. **The pumpLoop-tail wedge is gone.** Both runs printed `settle.stop done` *and*
   `watchdog.stop done`, and both wrote `loader: MARK pumpLoop done`. Run #2 even drained its
   loader completely (`inflight=0 pending=0 hosts=0`) after the pump had already returned. Whatever
   wedged at those statements under 0.1.9.46's timing does not exist as such — the manifestation
   moved, which is the race showing itself again.
3. **New signature, twice identical:** the last line of *both* launches is
   `rl: timer MainThreadSharedTimer enter` with no matching exit. Run #2 got one complete
   `APOTRACE` charset-lowering block inside that fire before going quiet. The heartbeat verdict
   over run #1 reads `busy=1 finished=6 job=live-tick tickstep=3` — tickstep 3 is the
   `RunLoop::cycle()` group at the top of `WebCoreLiveTick`, which is where timers fire. So the
   engine dies **inside `ThreadTimers::sharedTimerFiredInternal()`, while it is working through a
   batch of due WebCore timers**, in live mode, after the load has fully succeeded.

### What MainThreadSharedTimer actually is here

Upstream's Windows implementation (`platform/win/MainThreadSharedTimerWin.cpp`) drives itself from
a hidden HWND and `WM_TIMER`; PlatformWinUWP.cmake does not build it — the engine thread has no
window and no message pump. What runs instead is the port's replacement in `Src/port/stubs-other.cpp`:
a file-local `RunLoop::Timer` literally described `"MainThreadSharedTimer"`, bound to
`RunLoop::mainSingleton()`, calling `MainThreadSharedTimer::fired()` →
`ThreadTimers::sharedTimerFiredInternal()`. One `rl: timer MainThreadSharedTimer enter/exit`
therefore wraps an *entire batch*: every due `WebCore::TimerBase` — DOM timers carrying site JS
(`setTimeout`), `ScriptRunner`, `DeferrableOneShotTimer`s across loading and layout — fired back to
back in heap order. WebCore timers carry no description, so nothing above that loop can say which
member of the batch hung.

### The instrument now shipped

`ThreadTimers.cpp` (WK_WINUWP-guarded) pair-traces each `item->timer().fired()`:

```
wtimer: enter #17 obj=24C81A40 vtbl=63A12345 rep=0.0ms left=4
wtimer: exit  #17 ...
```

`vtbl` is read *before* `fired()` (a callback may stop or delete its own timer) and resolves
offline through `??_7ConcreteClass@@6B@` to the exact class; `obj` disambiguates instances,
`rep` separates repeating DOM timers from one-shots, and a final `enter` with no `exit` names the
batch member that died. Cost is two file appends per WebCore timer fire — same class of observer
effect as the probes before it, so expect the window to move again.

### Two side observations from the same traces

- The ghost `StringImpl` from the ARM32 launch crash lives on: `impl=66A11F58`,
  `[refcount climbing 0x1f→0x509] [0] [6688a7ad] [ec889e1c]`, length 0, passed to lowercase by
  `WebCore::ContentSecurityPolicy::updateSourceSelf` (symbolized precisely at
  ContentSecurityPolicy.cpp:288 via the PDB recipe). It is ref'd thousands of times per session and
  has not faulted since the guard landed, but it is unexplained and deserves its own write-up.
- Port-trace lines interleave mid-write at the moment the pump exits (engine and curl worker
  appending concurrently, no cross-thread lock on the channel). Cosmetic — but it proves both
  threads were running simultaneously at that instant, which is worth remembering when a garbled
  line looks like evidence.

Open: caller `5E7AB6C7` (the recurring in-timer charset lowering) did not symbolize — the PDB
recipe returned COFF-proximity junk for two of three addresses this time. Re-derive it from a map
or a fresh PDB match before trusting any frame it reports.

## 2026-08-23, later: the dying timer is a plain WebCore::Timer, and a wedge snapshot is armed

The `wtimer:` probe worked first try. Two hand reproductions of 0.1.9.48, same signature as before
(`busy=1 job=live-tick` heartbeat, load completes, drain reaches `inflight=0 hosts=0`, then silence),
and this time the trace names the batch member:

```
rl: timer MainThreadSharedTimer enter
wtimer: enter #128 ... exit          # two members fired fine
wtimer: enter #129 obj=6200FF88 vtbl=61BB5E3C ... exit
wtimer: enter #130 obj=23B31510 vtbl=61BB5E3C rep=0.0ms left=31
<end of trace>                       # no exit: the wedge is inside member #130
```

Resolving `vtbl=61BB5E3C`: the DLL symbol table is stripped and no map is produced for WebCore, so
the vtable was resolved through its *contents* — read slots 0/1 out of the deployed DLL's `.rdata`,
subtract the preferred image base, and look both code addresses up among PDB publics:
`??_GTimer@WebCore@@UAAPAXI@Z` and `?fired@Timer@WebCore@@EAAXXZ`. So the class is exactly
**`WebCore::Timer`** — the Function-carrying concrete timer used all over WebCore — and since
`Timer::fired()` just calls `m_function()`, the wedge is inside whichever WTF::Function some owner
gave it. Dozens of candidates own plain Timers; nothing above the call can say which. The second
vtable seen in batches (`61C03148`) resolved directly to **`WebCore::EventLoopTimer`** — that is the
WindowEventLoop task pump, and it explains the recurring charset-lowering blocks inside shared-timer
fires (event-loop tasks carry fetch/XHR response propagation).

Guessing the owner is not necessary either. The harness now takes a **wedge snapshot**: the
heartbeat already detects `busy=1` with an unchanged `finished` counter; on the first such beat the
UI thread suspends the engine thread (`OpenThread`+`SuspendThread`), captures PC/LR/SP plus 512
stack words via SEH-guarded copy, classifies every word against the module bases captured at DLL
preload, resumes the thread and writes `LocalState\wedgedump.txt`. The platform allows several
seconds of wedge before killing the app, so the first stuck beat lands while the frame is still
live. Offline, PC/LR and the stack words resolve against the logged module bases exactly like the
APOTRACE return addresses do. The snapshot lives entirely in the harness — no engine rebuild — and
is one dump per process by design.

Shipped in 0.1.9.49 together with the identification above. Next reproduction should produce a
wedgedump whose PC line says what member #130's function was actually doing: spinning in JS (a
runaway site callback), parked in a lock, or something else again.

## 2026-08-23, evening: the engine is ASLEEP, and the sleep itself is now the suspect

The wedge snapshot worked exactly as designed, and what it caught rewrites the picture twice.

**0.1.9.49's dump**: PC/LR both inside one system image near `0x77C00000`, zero classified words in
the scanned stack — the v1 dump only knew three module bases, so everything else was invisible.
**0.1.9.50's dump** fixed that (VirtualQuery walk + PE-header module names + raw word dump) and
produced a fully resolvable chain:

```
PC  ntdll/ucrt region (syscall)          SP=18CFD490, engine tid confirmed
    WTF::ThreadCondition::timedWait(Mutex&, WallTime)   ThreadingWin.cpp:386
    WTF::ParkingLot::parkConditionallyImpl               ParkingLot.cpp:601
    WTF::Condition::waitUntilUnchecked<Lock>             Condition.h:204
    WTF::RunLoop::runImpl                                RunLoopGeneric.cpp:334
        (= the populateTasks call site; populateTasks is inlined)
    ... driver/live-tick frames (Harness.exe+...) further up
```

So the engine thread is **not spinning and not stuck inside a timer callback — it is asleep in the
RunLoop's own condition wait**, `m_readyToRun.waitUntil(m_loopLock, sleepUntil, ...)`, the normal
idle park. No `rllock:` BLOCK anywhere near the end either: our lock probe sites are clean.

That leaves two facts in tension:

1. The trace ends mid-batch (`wtimer: enter #138` … nothing), suggesting the engine died inside
   member #138's callback;
2. The dump shows it parked *after* the batch loop, back in populateTasks.

Both cannot be literally true. The raw stack settles one half: there are **no** outer
fired()/sharedTimerFiredInternal/runImpl frames above the parked frame — no nesting happened. And a
mode fact kills another suspect: live-tick's `cycle()` runs RunMode::Iterate, whose populateTasks
**never waits at all** — the wait only exists in Drain mode, i.e. inside a `pumpLoop`
`RunLoop::run()`. A parked-in-Drain thread contradicts `job=live-tick tickstep=1` unless the job
label/tickstep is stale from an earlier moment.

What would make all observations true at once:

- the batch really finished (the missing `wtimer:` exit is a **lost trace write**, not a skipped
  line — `apoTraceWTFired` silently drops its output when `fopen` fails, which on this flash is not
  hypothetical);
- the loop returned to populateTasks, armed a wait for the next scheduled timer,
- and **never woke up**: overslept deadline (monotonic→wall clock conversion inside
  `ThreadCondition::timedWait` against this kernel) or a lost wakeup with `m_pendingTasks` never set.

0.1.9.51 ships the instrument that decides it: `rlsleep:` lines written around every Drain-mode
wait — `arm planned=Xms sched=N` before (only for sleeps > 1 s or infinite, so steady ticking stays
quiet) and `woke planned=X slept=Y sched=N pend=P stop=S` after (always for long sleeps, sampled 1-in-64
for short ones). A final `arm` without its `woke`, with the armed deadline visible right there, is
the smoking gun; the planned/slept pair quantifies any clock overshoot directly.

## 2026-08-23, late night: the wait is unreachable from where the heartbeat says we are

0.1.9.51's run answered half the questions and sharpened the rest:

- `rlsleep:` works and shows pumpLoop's drain waits behaving perfectly: `planned=-41.6ms` (timers
  already overdue), woke after 0.4–0.5 ms, `stop=1` on the final pass, clean `pumpLoop done`. No
  oversleeping, no lost wakeup in any wait we cover.
- The live mode has **zero** `rlsleep` coverage *by design*: `cycle()` runs `RunMode::Iterate`,
  whose `populateTasks` skips the condition wait entirely. The window where every death happens is
  exactly the mode the instrument cannot see.
- `rllock:` shows one benign transient all session (`wakeUp` vs `timer.stop`, resolved in ≤3 ms).
- And the wedge snapshot still puts the engine thread **parked inside
  `populateTasks → Condition::waitUntil → ParkingLot`** — re-symbolized against the current build,
  the chain is coherent: `ThreadCondition::timedWait ← parkConditionallyImpl ←
  Condition::waitUntilUnchecked ← populateTasks(inlined) ← runImpl`.

That parking spot is reachable **only with `runMode == RunMode::Drain`** — it is a runtime branch,
and `cycle()` passes `Iterate`. Attempt #2's only pumpLoop had already returned cleanly (its MARK is
in the trace), and no `pump:` entry appears afterwards. A parked Drain wait therefore cannot belong
to the engine's normal control flow at that moment.

The one structure that makes every observation true simultaneously is a **nested `RunLoop::run()`
entered from inside the batch member's callback**: the outer `fired()` never returns (missing
`wtimer:` exit ✓), the inner loop is a fresh Drain runImpl whose `populateTasks` parks waiting for
tasks only the blocked-outer world could produce (✓ dump), the job label stays whatever the outer
job set (✓ `job=live-tick tickstep=2`), and no lock probe fires because nothing contends (✓).
Which callback nests a Drain loop is unknown — but its class is already identified per run via the
wtimer vtable, so the nesting site is one symbol away once confirmed.

0.1.9.52 adds the two probes that decide it: a depth counter in `runImpl`
(`rlsleep: nest depth=N mode=M` on any re-entry) and the thread id in every `wtimer:` line. An
`rlnest` line immediately before the cutoff confirms nesting and names its mode; its absence sends
the investigation back to the raw-word stack with a proper `.pdata` unwind as the next instrument.

## 2026-08-24: nesting confirmed — but the counter was contaminated; probes hardened

0.1.9.52's first run produced eleven consecutive `nest` lines (#61–#71, one per live tick) right up
to the final mid-batch cutoff — apparently proof that the whole live mode runs inside a never-exited
outer loop, whose first entry was even `mode=drain` (line 27735, inside task 0 of the session
build). But the depth counter was a **file static shared by all threads**, and every WTF background
thread owns its own RunLoop: worker threads entering *their* loops bump the same counter and log
phantom nests. The tid added to `wtimer:` lines in the same build confirms those traces' timer
firings are engine-tid — the nest lines carry no tid yet, so nesting is **half-proven**: real on
some thread, unattributed.

Also ruled out this round: `DeferredWorkTimer::runRunLoop()` (the one upstream nested
`RunLoop::run()` inside JSC) is called only from jsc.cpp's CLI shell, never from WebCore or this
port. If nesting is real, its entry point is ours to find among the driver's own pump/cycle call
sites reached from timer callbacks.

0.1.9.53 hardens the instruments so the next run is decisive:

- the nest depth is **`thread_local`** now — only genuine same-thread re-entry logs;
- `tid=` added to every `rlsleep:` line;
- both trace writers (WTF's and ThreadTimers') switched from fopen/fclose-per-line to one
  persistently-open, mutex-guarded handle with per-line fflush. This kills three birds: transient
  open failures can no longer silently DROP lines (the leading explanation for the missing wtimer
  exits), concurrent appenders no longer interleave mid-line, and the hot path loses an
  open+close per event.

Reading the next reproduction:

- `nest depth≥2 tid=<engine>` right before the cutoff → nested Drain loop confirmed; walk back to
  its first occurrence for the entering callback.
- no nest + missing exit STILL → the lost-write theory dies too (persistent handle), and the
  remaining explanation is that `fired()` genuinely never returns while the parked-in-populateTasks
  snapshot was taken from a DIFFERENT wedge episode than the trace tail being read — at which point
  the two-attempt file-sharing of port-trace.txt/wedgedump.txt must be pinned down first (attempt
  boundaries are visible as SetCACertBlob/VM::create restarts mid-file).

## 2026-08-24, evening: caught SPINNING. The disease is unbounded site JavaScript; it has a leash now

The 0.1.9.55 dump added the missing instrument — a sibling-thread sweep — and its very first catch
rewrote the case file. The engine thread's PC sat **inside the JSC interpreter**, with a deep
runtime call stack under WebCore rendering-update frames: the thread was not blocked anywhere. It
was **executing site JavaScript without end**.

That reading dissolves the contradictions that had accumulated:

- The "parked in libcrypto/libcurl" chain of 0.1.9.54 was **stale frames below live ones**: the
  driver's own synchronous main-document fetch runs libcurl+libcrypto on the same engine stack
  during nav-load, and those words survive long after the call returned. The live top of that stack
  was JSC code, same as this round.
- `PC=77c014e7`, read as "a syscall wait", is simply the generic wait stub where **every idle
  thread in the process sits** — half the sibling threads showed exactly it in the sweep. A suspend
  landing there catches a transient internal wait, not the steady state.
- The parked-in-populateTasks snapshots were likewise transient moments (allocation slow paths)
  inside what was always the same long-running script execution.
- Every symptom fits one disease: heartbeat `busy=1` forever because pumpLoop's settle/watchdog
  timers cannot fire while the interpreter spins; no UEF and no dump because no fault occurred; the
  platform reaping the app seconds later; the x64 bench surviving because a healthy desktop network
  does not drive heavy bundles' retry loops into unbounded territory.

### The fix

WebKit embedders have a first-class mechanism for exactly this: **`JSC::Watchdog`**. buildSession
now arms it once per process, after `Page::create`:

```cpp
vm.watchdog()->setTimeLimit(WTF::Seconds(15), callback);
```

The callback writes `js watchdog fired: runaway script terminated` into gpuinit-steps.txt; the
running script is terminated with an interruption while the page and the engine thread both
survive. One upstream change was required: `Watchdog::setTimeLimit` had no DLL entry point (nothing
inside JavaScriptCore calls it across modules), so `runtime/Watchdog.h` gained a WK_WINUWP-guarded
`JS_EXPORT_PRIVATE`.

Shipped as 0.1.9.56 together with the full probe kit (wtimer/rllock/rlsleep/wedgedump v5 with the
sibling-thread sweep). Expected outcomes of the next reproductions:

1. Clean sessions — the watchdog never fires because the runaway loops were timing-dependent;
2. `js watchdog fired` marker + browser **alive** — the leash works and the culprit class is named
   in the surrounding trace lines;
3. Still `CRASHED` — another face exists, and the wedgedump machinery is now sharp enough to
   name whatever it is within one device cycle.

## 2026-08-24, night: 0.1.9.56's regression, and the Watchdog arming trap

Outcome 3 arrived immediately, plus a brand-new failure mode the fix itself introduced: **the
browser now died instantly at the start of the ya.ru navigation** — where previously content was
visible for seconds first. Root cause, straight from `Watchdog.cpp`:

`startTimer` computes its deadline **from the moment it is called**, and nothing restarts it unless
JS actually enters through a path that calls `enteredVM()`. Armed once outside any JS execution,
the deadline is *absolute wall time*: home loaded fine within its budget, and fifteen seconds after
arming every script on earth was already past-deadline — so the very first script of the next
navigation got terminated instantly, and a termination arriving inside module parsing under
`_HAS_EXCEPTIONS=0` takes the process down without ceremony.

The correct integration is therefore **re-arming per script-execution window**, not set-once.
`apoArmJsWatchdog()` (WebCoreDriver.cpp) calls `setTimeLimit(15 s)` guarded by `commonVMOrNull()`,
and is invoked:

- after `Page::create` in buildSession — covers document feeding, parser scripts, microtasks;
- at the top of every `WebCoreLiveTick` — covers rAF/JS/promise work each tick carries.

Re-arming is cheap by design (`startTimer` keeps an existing earlier deadline instead of
restarting) and self-correcting: between windows the stale deadline can terminate at worst the
first moments of the next window's script, which the fresh arm immediately fixes for the rest.
Shipped as 0.1.9.57.

One more trap for the record: `Watchdog::setTimeLimit` had no DLL entry point (nothing inside JSC
cross-module ever called it), so `runtime/Watchdog.h` carries a WK_WINUWP-guarded
`JS_EXPORT_PRIVATE` — without it the driver link fails with LNK2019 against JavaScriptCore.dll.

## 2026-08-24, late: the watchdog era begins — and the wedge turns out to wear a font

0.1.9.60/61 closed the arming story with ground truth from gpuinit-steps:

- `wd=00000000` on every arm across every build before 0.1.9.61: **the watchdog was never created**
  for the common VM (`getIfExists()` on a lazy property nothing had initialized), so every build
  since .56 ran unprotected regardless of intent;
- .56's instant-nav-death was `setTimeLimit` called through that null (AV write at garbage+0x10,
  PC inside Watchdog.cpp:46) — the UEF raw-stack upgrade in .58 is what finally made the PC
  attributable;
- 0.1.9.61 exports and calls `VM::ensureWatchdog()` out-of-line (WK_WINUWP-guarded in VM.h/.cpp),
  arms successfully (`wd=1C53F118` in gpuinit-steps), and the marker protocol is live.

Then the first armed reproduction delivered a clean, fully-symbolised wedge — and it is **not
JavaScript**: the engine parked in a native wait underneath

```
WidthIterator::advanceInternal / commitCurrentFontRange
 → FontCascade::glyphDataForCharacter        (FontCascade.cpp:453)
   → FontCascadeFonts::glyphDataForCharacter (FontCascadeFonts.cpp:569)  -- glyph-level fallback
     → FontSelector ref-deref → Condition/ParkingLot → syscall wait
```

during a live-tick cycle, ~5 s after content first painted. The watchdog cannot fire here by
design: no JS is executing, so no check runs. The sibling-thread sweep shows every other thread in
innocent OS waits — nobody legitimately holds a font-stack lock — which points at a self-deadlock
or lost wakeup inside the FontCache/fontconfig layer on this kernel (the x64 bench is immune:
warm font cache, different font stack shape).

Next instrument (designed, not yet built): paired probes around FontCache's lock and the
glyphDataForCharacter fallback entry, same pattern as rllock — name the contended site and the last
owner, or show zero contention and move the hunt to fontconfig's own locking.

## 2026-08-25: THE OBSERVER KILLED THE PATIENT. The wedge was our own font probe

The 0.1.9.61 wedge snapshot symbolised to something no disease-theory predicted:

```
PC = apotheosisWebTrace                       (TextBreakIterator.cpp, compiled into JSC)
stack literal = "gdc c=U+%04X page=%u v=%d"   (the FontCascadeFonts probe's own format string)
```

The engine thread was parked **inside our own per-character font tracer**, mid-write.

`apotheosisWebTrace` dates from the font epic (added by an external AI assistant during that
investigation) and was wired into `gdc`/`gdv`/`adv`/`drawText`/`brk` — i.e. it fired **once per
character during text shaping**, each call doing `getenv` + `fopen(path,"a")` + write + `fclose`
on APOTHEOSIS_GLYPH_LOG. Its header comment claimed "guarded by the environment variable so zero
overhead when unset" — but the harness exports APOTHEOSIS_GLYPH_LOG unconditionally at startup,
so the guard never guarded anything. During ya.ru's layout that is hundreds of open/close cycles
per second on the single engine thread, racing the curl worker's and driver's writers on other
files of the same trace family.

Everything the week produced now reads as one meta-fact: **per-character file I/O inside layout
was itself the wedge**, and its enormous timing distortion is the best explanation for why every
manifestation relocated whenever a probe was added or removed — live-tick vs nav-load vs
populateTasks-park, "crypto block" vs "JS spin" were all real moments, but sampled from a patient
already poisoned by the observer.

### What changed (0.1.9.62)

- `apotheosisWebTrace` runs **only when `APO_TRACE_TEXT=1`** is explicitly present in the
  environment (a deliberate font-debugging session); default builds pay one atomic load per
  character and nothing else.
- When enabled, it writes through one persistently-open, mutex-guarded handle instead of
  reopening per line.
- The gdc/gdv/adv/drawText/brk call sites stay in place under the same gate for targeted future
  use.

### Re-triage required

Every conclusion drawn while this probe ran needs re-confirmation under clean timing before being
treated as a live defect: the populateTasks park, the FontSelector deref chain, the ResourceResponse/
ParsedContentType frames, even the frequency of deaths at all. The honest current state is:

1. One proven structural defect fixed for real along the way: the double-NavigateTo trigger
   (dropped duplicate), plus the Watchdog infrastructure (now correctly created via
   ensureWatchdog + re-armed per window) which remains valuable insurance against genuine runaway
   scripts;
2. The original crash family (UEF AVs, e.g. the StringImpl-deref at second session build) may
   still exist underneath and will finally be observable without observer noise — the upgraded UEF
   raw-stack dump and the wedgedump machinery stay armed for exactly that.

Next reproduction decides: if ya.ru now loads and survives repeatedly, the defect list shrinks to
what the clean-timing runs actually show; whatever still crashes gets chased with the same
instruments, this time measuring the patient and not the probes.

## 2026-08-25, evening: the unwind named it. WebCoreSessionLoad, parked in its own curl

0.1.9.65 shipped instrument #7 — a REAL stack walk inside the wedge snapshot, via
`RtlLookupFunctionEntry`/`RtlVirtualUnwind` over the ARM32 `.pdata` tables (raw word scans had
three times produced plausible but wrong chains: live frames interleaved with stale words from
earlier calls on the same engine stack). One reproduction later, the `unwind:` section drew the
chain with no ambiguity left:

```
#0-2   syscall wait (ntdll/KERNELBASE/ucrt-level)
#3-6   libcurl.dll  (perform/wait internals)
#7     WebCoreSessionLoad            <-- the driver C ABI of the navigation itself
#8     nav-load job lambda           (MainPage.xaml.cpp)
#9-11  WebEngine::loop / dispatch
```

**The engine thread was blocked inside the driver's own synchronous main-document fetch** —
`curl_easy_perform` for https://ya.ru run directly on the engine thread by buildSession. The
stall source is the phone's documented habit (deploy scripts have suffered it since August): W10M
power-save drops the WiFi radio mid-transfer; no RST, no FIN arrives, the socket just goes
silent, and a blocking read waits indefinitely.

`CURLOPT_TIMEOUT=30` could never save it, for two stacked reasons: the platform reaps an
unresponsive foreground app within seconds (the 4–7 s zombie window the heartbeat era measured),
and LOW-speed silence is exactly what that timeout measures last. Every earlier "face" of the
wedge — fonts mid-fallback, populateTasks parks, JS interpretation, libcrypto frames — was simply
the neighbourhood the engine happened to be walking when the guillotine fell; the raw-stack scans
kept mixing those live frames with stale words from the SAME stack's earlier synchronous fetch,
which is why each reading contradicted the last.

### The fix (0.1.9.66)

Fail fast, shorter than the killer's patience, in buildSession's fetch handle:
`CONNECTTIMEOUT 3 s`, `LOW_SPEED_LIMIT 1 B/s` sustained `LOW_SPEED_TIME 2 s` (the true stall
detector — silence IS sub-byte throughput), total `TIMEOUT 8 s`, plus `TCP_KEEPALIVE`. A stalled
fetch now returns `CURLE_OPERATION_TIMEDOUT`/low-speed abort, `buildSession` records the net
error, and the harness renders its error page — the app lives, the user retries. The longer-term
cure (move the main-document fetch off the engine thread entirely) is noted as future work; it
changes session-build sequencing and deserves its own cycle.

Also shipped along the way: heartbeat cadence 2 s → 700 ms so the wedge snapshot wins its race
against the killer, and the UEF handler dumps 256 raw stack words around the faulting SP (its
first version compiled itself away behind a WK_WINUWP guard that does not exist in the harness).

### Re-triage ledger after a clean run

If ya.ru now survives repeatedly at 0.1.9.66+, then: the per-character font probe (fixed .62),
the missing watchdog creation + one-shot deadline trap (fixed .61/.57 semantics), the duplicate
navigation trigger (fixed earlier), and the synchronous-fetch stall (fixed here) together explain
the full observed history — and the remaining open items are only the StringImpl-deref AV at
second session build (needs one clean repro under the new UEF raw-stack logging) and the
long-term async-fetch redesign.

## 2026-08-25: status summary after seven instruments and eleven builds

### Fixed along the way (each verified on device)

| Fix | Build | Mechanism |
|-----|-------|-----------|
| Per-character font probe disabled | .62 | apotheosisWebTrace fired fopen/fclose per glyph during layout — massive timing distortion |
| JSC Watchdog created + armed | .61/.66 | ensureWatchdog() exported from JSC.dll (WK_WINUWP guard); setTimeLimit(15s) re-armed per window |
| Persistent curl handle | .70 | CA store (121 certs, 189 KB PEM) parsed once per process instead of once per navigation; connection/TLS reuse |
| Background main-document fetch | .71 | Engine thread waits max 2.5 s on a condition variable; transfer runs on dedicated thread |
| Fail-fast curl timeouts + progress abort | .66/.67 | CONNECTTIMEOUT 3s, LOW_SPEED 1B/s×2s, progress-abort after 2.5s stall |

### Still open

**Instant silent death (~+2–3 s of content render).** Persists across ALL above fixes including
useJIT=false (.73) and CheckForUpdate disable (.74). Signature: no exception, no dump, no
wtfcash, heartbeat stops. Every wedgedump unwind caught a benign moment (bounded fetch wait,
job queue idle) — the actual termination leaves no user-mode trace whatsoever. Prime suspects
remaining: external TerminateProcess (platform PPM, TDR/GPU device-removal cascade), or a
fastfail path that bypasses every user-mode hook on this kernel build.

### Instruments built this week (all remain armed)

1. **wtimer** — paired enter/exit around each WebCore timer fire, with tid, vtable identity, batch position
2. **rllock** — contention-only probe on RunLoop m_loopLock (try-lock fast path, owner tracking)
3. **rlsleep** — arm/woke pairs around every Drain-mode populateTasks wait
4. **wedgedump v6** — UI-thread suspension of engine thread: PC/LR/SP/R0-R12, .pdata-based real stack unwind, raw stack words, sibling-thread sweep, per-PID file naming
5. **UEF raw-stack** — 256 words around faulting SP on any unhandled exception
6. **wtfcash** — WTFCrash hook writing backtrace before stomp
7. **heartbeat 700 ms** — faster stuck-beat detection

## 2026-08-24: the unwind names it — synchronous TLS on the engine thread

The per-PID dump (`wedgedump-<PID>.txt`, 0.1.9.69) removed the last ambiguity: one reproduction, one
dump, `wedgedump-5296.txt`, and its tail of `port-trace.txt` belong to the same death. The unwind,
symbolised bottom-up against `Harness.exe` (ImageBase 0x400000):

```
MainPage.xaml.cpp:1071   thread start lambda        <- the engine thread
MainPage.xaml.cpp:1230   WebEngine::loop()          <- the job-queue drain
MainPage.xaml.cpp:2345   a posted job
                         WebCoreSessionLoad         <- the C ABI navigation entry
  libcurl  +0170d7 ... +050017   (9 frames)
  libssl-3-arm  +00f0fb
  libcrypto-3-arm  +13d5cd ... +014803   (10 frames, several repeating)
  4 system frames, PC=77c15ced
```

`heartbeat.txt` for the same moment: `busy=1 pending=0 finished=7 job=live-tick tickstep=3`. The trace
ends with `rl: timer MainThreadSharedTimer enter` followed by `wtimer: enter #161 tid=2376 rep=0.0ms
left=23` — **entered, never exited**.

So the engine thread is inside `curl_easy_perform`, inside the TLS stack. The repeated libcrypto frames
around `+0148xx` and the register file (`R0=c7 R1=c6 R2=c7 R5=545` — small lengths, not pointers) read
as recursive ASN.1 / certificate-chain work rather than a wait.

**This is not a deadlock and not memory corruption. It is synchronous, CPU-bound TLS work holding the
one thread the whole engine is serialised on.** That distinction matters more than it sounds: while
OpenSSL is *computing*, no curl timeout can help. `CURLOPT_TIMEOUT` counts wall clock but cannot preempt
a call already inside libcrypto; `CURLOPT_LOW_SPEED_*` needs bytes to be flowing to notice they are not;
and `CURLOPT_XFERINFOFUNCTION` is not guaranteed to be invoked during a handshake. The fail-fast budget
introduced in 0.1.9.65 (8 s total, 3 s connect, 2.5 s stall, plus our own 8 s ceiling on our own clock)
is therefore both **too long** — the platform's frozen-app killer reacts in less — and **unable to fire**
in this particular window.

### A near miss worth recording

`WebCoreDriver.cpp` sets `CURLOPT_TIMEOUT, 8L` in the `WK_WINUWP` block and `CURLOPT_TIMEOUT, 30L`
about twenty lines later. On one handle the last `setopt` wins, so that reads exactly like the fail-fast
design being silently overwritten — a one-line bug and a satisfying answer. **It is not one:** the 30 s
call sits inside the `#else`, so it never applies to this port. Checked before reporting; recorded here
because the next reader will see the same two lines and reach for the same conclusion.

### Two hypotheses closed by measurement, not argument

- **`CurlRequestScheduler::startOrWakeUpThread` → `waitForCompletion()`** — probed directly, **zero**
  `curlsched:` lines in a whole session. The wait is never entered.
- **`RunLoop::m_loopLock` contention** — the earlier reading of the four-statement window after
  `RunLoop::run()` returned. Superseded: the thread is not blocked on a lock at all, it is executing.

### Next steps, cheapest first

1. **The CA store size.** `WebCoreSetCACertBlob` injects the full Mozilla bundle. Verification cost grows
   with the number of candidate issuers OpenSSL walks, and ARM32 here has no hardware crypto. Trimming
   the bundle is minutes of work and one deploy to test.
2. **Measure instead of infer.** `CURLOPT_DEBUGFUNCTION` with timestamps around the handshake, to learn
   how many milliseconds the chain verification actually costs on this CPU.
3. **Structural: take the document fetch off the engine thread.** Then TLS cannot block the engine at
   all, and this whole class of death goes away. Not a patch — a design change, to be planned as one.

## VERDICT (0.1.9.77): DEFECT CLOSED ON ARM32 — see the postscript below before relying on this

ya.ru loads fast and **survives indefinitely** at 0.1.9.77. No crash, no hang, no silent death.

### Root cause (composite -- three defects stacked)

1. **Synchronous curl on the engine thread.** WebCoreSessionLoad fetched the main document with
   curl_easy_perform directly on the only engine thread. When the W10M radio dropped mid-transfer
   (power-save), the blocking read waited forever.
2. **No CA-store reuse.** A fresh curl handle per navigation meant OpenSSL re-parsed 189 KB of PEM
   (121 certificates) every time -- seconds on ARM32 without hardware crypto.
3. **JIT execution in AppContainer.** Heavy JS bundles pushed hot loops through JIT-generated code;
   executing from a dynamically-provisioned page inside the AppContainer terminated the process
   instantly and silently (no exception, no dump).

Each alone was survivable; together they guaranteed death within seconds of navigating to any
real-world site.

### Fix stack (0.1.9.77)

| Change | Effect |
|--------|--------|
| Background fetch thread | Engine never sits in a transfer |
| Fail-fast timeouts + progress abort | Stall detected within 3 s |
| Persistent handle + CA cache | Parse once, reuse connections/TLS sessions |
| useJIT=false unconditional | Interpreter-only, no code-page kills |
| Watchdog armed per window | Runaway scripts bounded at 15 s |

The first navigation may still show an error page if the radio drops during initial TLS handshake;
a retry succeeds because the connection cache holds a live session.

### Instruments remain armed

All seven diagnostic tools stay in place for future investigations: wtimer, rllock, rlsleep,
wedgedump v6 (.pdata unwind + sibling threads + per-PID naming), UEF raw-stack dump,
wtfcash hook, heartbeat 700 ms. They cost nothing when things work and are invaluable
when they do not.

---

# Postscript, 2026-09-03: the x64 line disagrees, and the instrument was lying

The verdict above stands **for ARM32**: ya.ru loads and survives there. It does **not** generalise. The
x64 line had not been compiled for 37 consecutive builds (.41 through .77 were ARM-only), and when it
was finally rebuilt today the app still died — during a navigation to dzen.ru, with a wedge dump taken.

Read the rest of this postscript before using any address from a dump written by .77 or earlier.

## The wedge dumper misattributed addresses, and a diagnosis was built on it

Three defects in the diagnostic itself, all found on x64 and all present on ARM32 too:

1. **The module walk stopped at `0x80000000`.** Correct for a 32-bit user space, useless on x64 where
   the DLLs sit near `0x00007FFD_xxxxxxxx`. `ApoBuildImageList` therefore found **zero** modules,
   `ApoFindImage` always returned null, and every frame fell through to the guessing heuristic below.
2. **The heuristic guessed by a fixed 64 MB window.** `a - g_baseWebCore < 0x4000000` while WebCore's
   x64 image is `0x2CB2000` (44.7 MB) — so 19 MB of whatever followed WebCore in memory was reported as
   WebCore, at offsets past the end of the module.
3. **Addresses were printed as `(unsigned)`,** dropping the high word. WebCore at `0x7FFDE85F0000` and
   JavaScriptCore at `0x7FFDEB2B0000` both render as `0xE8…`/`0xEB…` and become indistinguishable.

Combined, they produced a stack that read as WebCore frames and symbolised into plausible WebCore
functions. **The morning's conclusion — a spin inside `MicrotaskQueue::performMicrotaskCheckpoint` — is
retracted.** What gave it away was arithmetic, not insight: the top frame was `WebCore+3900163`, an
offset larger than WebCore's own image. An offset that cannot fit inside the image is not inside it.

Reattributing the same frames against the bases logged at startup (`log.txt` carries
`ModuleBase(Harness.exe)` and `LoadPackagedLibrary(...)` for the very run that crashed):

| printed | actually |
|---|---|
| `WebCore+3900163` | `JSC+C40163` → `JSC::VM::updateStackLimits` (VM.cpp:1173) |
| `WebCore+3506d67` | `JSC+846D67` → `JSC::JSLock::willReleaseLock` (JSLock.cpp:206) |
| `WebCore+3b440ac` | `JSC+E840AC` → `WTF::RunLoop::runImpl` (RunLoopGeneric.cpp:410) |

So the thread was **releasing** the JS lock on the way out of a checkpoint, not spinning inside one.
Note what follows for the fix table above: `useJIT=false` cannot be the reason ARM survives, because
x64 has no working JIT at all (DEP/ACG blocks it) and x64 wedges anyway. That entry treated a symptom.

Fixed in .83: the walk covers the whole user address space; the fallback window is the real
`SizeOfImage` read from the module's own PE header; one shared `ApoFormatAddr` prints `%p` at full
width and prints a bare address rather than guessing when nothing matches; and the stack scan uses
pointer-sized slots, because reading an x64 stack as 32-bit words split every return address in half
and matched nothing while appearing to succeed.

## What the corrected instrument actually shows

A fresh dump (.83, wedge during dzen.ru) unwinds cleanly, and the picture is different again:

```
#3  Harness+11332c   _Cnd_timedwait_for_impl        (CRT sharedmutex.cpp:58)
#4  Harness+50eb     std::condition_variable::wait_until
#5  Harness+b376f    the WE-job lambda              (MainPage.xaml.cpp:2461 → WebCoreSessionLoad)
#6  Harness+101a4c   WebEngine::loop                (MainPage.xaml.cpp:1346)
#7  Harness+8e5d9    the engine thread's start lambda
```

The engine thread is inside `WebCoreSessionLoad`, blocked on a **condition variable** — the handoff to
the background fetch worker that .77 introduced (`ApoFetchChannel`, `cvDone`). Whether that wait is
bounded decides everything: if it is, the wedge detector fired on a slow load and the process was
killed by something else; if it is not, this is a genuine hang and it lives in the .77 fetch channel.

> **Answered 2026-09-18 — it is bounded, and the first arm is the right one.** At the time of that
> stack the wait was a fixed **2 500 ms**; since `0.1.10.0` it is a progress-aware stall detector
> (2 500 ms re-armed while bytes arrive, **9 500 ms** ceiling). Either way the engine thread cannot park
> on a fetch forever, so a `WEDGE` there is the detector firing on a slow load — confirmed directly:
> `Doc/TOPLEVEL-FETCH-BUDGET.md` §7b shows a `WEDGE: no progress for 6 beats` one second before
> `after-load … rc=0` for a page that was loading correctly. That false positive is fixed; read §7b
> before reading any `WEDGE` line as a hang.

## Open on x64, in priority order

1. ~~Establish whether `ApoFetchChannel`'s `cvDone.wait` is bounded.~~ **Answered 2026-09-18: it is**
   (see the note above). A `WEDGE` on this stack is the detector firing on a slow load, not a hang;
   `Doc/TOPLEVEL-FETCH-BUDGET.md` §7b has the direct measurement and the fix.
2. Run the packaged sequence end to end (example, HN, the layer probe, dzen) on x64 and see which entry
   kills it.
3. The paint fixes of .82/.83 are confirmed on a page that forces a compositing layer
   (`will-change: transform`), which no site in the earlier list does at this viewport — Composite
   correctly returns -4 for them, so `gpuCompositeReadback` was never called and the fix could be
   neither confirmed nor refuted by those runs. The layer probe is now entry 1b in the sequence.

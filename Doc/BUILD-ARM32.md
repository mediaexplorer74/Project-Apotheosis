# ARM32 build line — Lumia 950 / Windows 10 Mobile

> **Current (rewritten 2026-09-26).** The target is no longer "make the native harness run on
> ARM" — it is "ship the **C# UWP app** on a real Lumia 950 under W10M". See [INDEX.md](INDEX.md) for
> the phase framing and [MANAGED-HOST.md](MANAGED-HOST.md) for what the managed host requires from
> the driver. The C++/CX harness build record that used to follow this file now lives in
> [Archive/2026-08-12-arm32-harness-build.md](Archive/2026-08-12-arm32-harness-build.md).

The single most expensive fact about this project is that rebuilding WebKit for ARM32 costs half a
day on this machine. The driver line does **not** require it, because the engine archives are

## Status measured 2026-10-03 (night, latest) — the `.25` run: the AV is gone, the process still dies, and the death is a release assert

The maintainer's three attempts on the deployed `.25` ("мгновенный краш / белое окно / контент на
секунду-две, потом краш") pulled as `20261003-224203`: `port-trace.txt` 155,278 bytes / 4,366 lines,
plus `mem.txt` and `stage.txt`. **Two** `DEATHNET: installed` blocks for three launches, so **one launch
never reached engine init** — that is the "instant crash", and it left no trace at all.

| Attempt | Block | What the trace says | Verdict |
|---|---|---|---|
| 1 — instant crash | *none* | no `DEATHNET`, no `started` line | died before engine init |
| 2 — white window | lines 1–348 | an `sso.dzen.ru/install` load, `tail: paintToRGBA exit rc=0 nonWhite=0`, then `VEH: … SharedLibrary.dll+0x363B4D op=1 faultaddr=0x0` and `CoreUIComponents.dll+0xD4347` | **not an engine death** — a null *write* in the .NET Native runtime and in XAML, the same platform pair that ended `.23` §1 and `.24` A |
| 3 — content, then crash | lines 349–4366 | real dzen.ru (react + `dzen-desktop` bundles + `avatars.dzeninfra.ru` + ads), painted `nonWhite=265380/777600`, `STALL-WATCH fired`, then `ABRT:` | **an engine death**, but a different one |

### The `.25` fix did its job: no engine-side VEH in either process

Those two lines are the **only** `VEH:` entries in the whole pull, and both are the platform pair. The
access violation reading `0x8` inside `Document::notificationClient()` — the fault that killed both
`.24` fatal sessions — **did not recur**. Two processes, three launches, zero engine faults. The
supplement fix is confirmed against the site it was built for.

### What replaced it: a release assert, named to the instruction

The trace ends:

```
wtimer: enter #215 tid=1000 obj=0A5D87D0 vtbl=61229FC8 rep=0.0ms left=84
ABRT: abort() reached the CRT signal handler tid=1000
ABRT stack (3 frames):
  ABRT #0 WebCoreDriver.dll+0xDDB9
  ABRT #1 ucrtbase.dll+0x39DDF
  ABRT #2 WebCoreDriver.dll+0xECEB
```

Base `0x5F9D0000` again, proved by two exact hits (`0x61229FC8 − 0x1859FC8`, `0x6123541C − 0x186541C`).
Frame #0 is the death net's own frame (`ApoPortAbortHandler` at RVA `0xDD74`, so `+0x45`); #1 is the CRT
that invoked it; **#2 is the return address into our own DLL, RVA `0xECEB`** — and RVA `0xECD0` in the
`.25` map is **`?WTFCrashWithInfo@@YAXHPBD0H@Z`**, the four-argument release-crash helper that
`WTF/wtf/Assertions.h` defines `inline` and that `llvm-nm --defined-only` shows as `T` in
**`WebCoreDriver.arm32.obj`** itself. Its body is `blx` through the IAT slot `0x1184B85C`, which the
import table proves is **`abort`** (`ImportAddressTableRVA: 0x184B858`, `_beginthreadex` first and
`abort` second, 4 bytes per ARM entry).

So the engine reached a **`RELEASE_ASSERT`/`CRASH()`** on the real dzen.ru — not a fault. It fires
**inside a one-shot `WebCore::Timer` callback** (`??_7Timer@WebCore@@6B@`, entering `fired()` and never
exiting), ~16 s in. The last engine work before it is MIME content-type parsing:
`ParsedContentType::setContentTypeParameter` (RVA `0x857310`, reached at `0x8573B7`) calling
`String::lower("charset")` — **the same last-completed work as the `.23` and `.24` deaths**.

Read `wtimer` correctly: it is emitted in `WebCore/ThreadTimers.cpp::sharedTimerFiredInternal` around
`item->timer().fired()`, so `obj` is the `TimerBase` address, `vtbl` is read *before* the call, and
**`left = m_timerHeap.size() - 1` is the batch depth, not a countdown and not milliseconds**. The
`obj=0A5D87D0` that dies appears **21 times** in the session, twenty of them clean; that is either one
long-lived housekeeping timer or a recycled allocation slot, and this trace cannot separate them.

### What was still unknown, and the `.26` instrument that closes it

`WTFCrashWithInfo(line, file, function, counter)` **spills all four arguments into its own frame**
(`push.w {r11,lr}`; `sub sp,#0x10`; `str r3,[sp,#0xc]`; `str r2,[sp,#8]`; `str r1,[sp,#4]`; `str r0,[sp]`),
and they are still on the stack while the CRT signal handler runs — which makes them readable without
a WebKit rebuild and without a debugger on the device.

`ApoPortScanForAssert()` in `Src/port/WebCoreDriver.cpp` is the reader. It is called from **both**
witnesses — `ApoPortAbortHandler` and the `0xC000001D` slot of `ApoPortDeathNet`, so a `RELEASE_ASSERT`
that traps and one that aborts are named by the same code. It walks up from its own frame and looks for
the arguments **by shape, not by address**: a `file` is a pointer the loader attributes to an image that
reads as printable NUL-terminated ASCII **ending in a source suffix** (`.cpp`, `.hpp`, `.mm`, `.cc`,
`.cxx`, `.h`, `.c`), and a `function` is the same one word later, with the pair's own `line` and
`counter` bracketing it. Every read is bounded by that image's own `SizeOfImage` (`e_lfanew` at `+0x3C`,
the optional header at `+24`, `SizeOfImage` at `+56` of it), so a string in the last page of an image is
never read past its end. The scan logs **every** hit it finds (capped at four), not the first, so a false
positive cannot hide the real one, and it dumps the words around each hit so the reading can be checked
against the raw stack.

Shape rather than a symbol range, because there is **no symbol to take**: `WTFCrashWithInfo` is `inline`
in `WTF/wtf/Assertions.h`, is emitted into `WebCoreDriver.arm32.obj` itself, and this TU includes no WTF
header at all. A port-only change that works on both architectures.

On a hit the log carries `assert(frame#N): file=… function=… (word=… word2=…)` for a frame-anchored
match and `assert(shape): file=… function=… (word=… word2=…)` for a shape-only one; on a miss it says
`assert-scan: no argument triple in N words from …` and dumps raw words around every captured frame
either way — a shape the scan does not recognise is still evidence, and the scan runs once, on the way
down. The `ABRT stack (3 frames)` limit is the known one; the count is printed for exactly this reason.

**Side finding.** `mem.txt` peaks at `used=215,5MB pct=14 level=low` of 1536 MB, so the quota remains
excluded as a cause.

## Status measured 2026-10-09 — the assert reader was dead code, and `.28` is the fix

`0.2.3.26` was deployed (`ME.Apotheosis_0.2.3.26_arm__cj4nrm0qt9s1y`, HTTP 202 in 20 s) and `0.2.3.27`
after it (HTTP 202 in 184 s). The maintainer reproduced on both. **The assert reproduced identically
every time**: `ABRT #2 WebCoreDriver.dll+0xECEB` (`.25`), `+0xEEC5` (`.26`), `+0xEECD` (`.27`) — the
same `+0x1B` inside `?WTFCrashWithInfo@@YAXHPBD0H@Z` in every build. The death is deterministic and
its class has never changed.

**What did not happen is the naming.** `.26` wrote

```
ABRT assert-scan: no argument triple in 512 words from 0A7CEAB0
```

and `.27` wrote `no argument triple in 1024 words from 09CBE888` followed by a 384-line wide window —
**in which every single line held the same word**. That is not a stack; it is one value printed 384
times. The scanner was reading a single location, 384 times over.

**The cause was the reader itself, not the window size** — which is what this section first claimed,
from the `.26` log alone, and it was wrong. The walk was written as

```cpp
void* anchor = nullptr;
void** p = (void**)&anchor;      // p[i] for i > 0 reads past the object: undefined behaviour
```

`llvm-objdump` on `WebCoreDriver.arm32.obj` shows the 384-iteration window loop loading **always
`[sp,#0x20]`** — the one slot holding `anchor`:

```
162: movs  r5, #0x60          ; i = 96
16e: ldr   r0, [sp, #0x20]    ; the same word, every iteration
184: adds  r5, #1
186: cmp.w r5, #0x1e0         ; 480
```

clang is entitled to that: an out-of-bounds subscript on a promoted scalar local is undefined, so
every access was treated as the same object and hoisted out of the loop. Neither net could have
matched anything, on any build — the shape net did not "miss", it never read real memory at all.

**`.28` makes it a stack reader.** The base is now the caller's captured-frame array — a pointer
parameter, which the optimizer has nothing to promote — every word goes through `ApoReadStackWord()`,
a `void* volatile const*` read so each slot is a real load, and **every printed line carries its own
slot address**, so a repeated address is visible in the log itself instead of being inferred from a
silent failure. The frame net (the `#2` return address, whose four lower neighbours *are* the spilled
arguments, `a = i - 4`, validated by shape against the copies in our own `frames[]`) and the wide
window both stay, now over `0 … 479`.

**Gesture correlation, both `.27` runs.** The maintainer's two reproductions are the trace's fourth
and fifth processes, and each ends inside a one-shot `WebCore::Timer` callback —
`??_7Timer@WebCore@@6B@` at VA `0x1185A038`, the plain one-shot, not a `DeferrableOneShotTimer` — so
the death site is the same one `.23`–`.26` found.

* **The pinch run.** `gesture: zoom enter` → `resize enter` → `zoom end n=1 dt=0ms idle=3592ms` →
  `resize end n=1 dt=0ms idle=3410ms` → `gesture: live n=50` → death. The pinch itself completed and
  the process died ~3.4 s later, idle.
* **The scroll run.** `gesture: scroll enter` → `gesture: live n=31` → **no `end`** → death. Here the
  death lands *inside* the scroll, which is what the maintainer reported.

Both die with the same last-completed engine work — `String::lower` over MIME types and response
**header names** (`image/webp`, `image/gif`, `Cache-Control`, `Content-Type`, `Date`, `Pragma`,
`Transfer-Encoding`, `X-Content-Type-Options`, `X-XSS-Protection`, `X-Requestid`) — i.e. the same
`ParsedContentType` subsystem as the `.23`/`.24`/`.25` deaths.

### `0.2.3.27` — built, audited, deployed

Both lines carry it: ARM reports `Compiled 14/14`, `[link] EXIT=0 unresolved=0 duplicate=142`,
`Exports: 40`, `SHA256 BE65AE0A…`; x64 `Compiled 13/13`, `unresolved=0 duplicate=0`, `Exports: 40`,
`SHA256 BD1AC5D4…`.
The bundle is `Src/Apotheosis/AppPackages/Apotheosis_0.2.3.27_Test/Apotheosis_0.2.3.27_arm.appxbundle`
(48,355,532 bytes, Release) and `Test-AppxPayload.ps1` reports `AUDIT PASSED` — 30 binaries all arm,
driver at the package root (30.6 MB), `resources.pri` present, **40/40** declared exports. The
freshly linked, staged **and packaged** `WebCoreDriver.dll` all hash to
`be65ae0ad877fbc24c6b1441…`, so the audit was run against this build's own artifact, not a previous one.
Deployed: `ME.Apotheosis_0.2.3.27_arm__cj4nrm0qt9s1y`, `HTTP 202 in 184 s`, `Success`. **The reader is
still unverified on the device** — no run has read it yet; the maintainer's next reproduction is the
proof, and it should log either `ABRT assert(frame#N): file=… function=…` or the wide raw window.

### `0.2.3.28` — built, audited, deployed

Both lines carry the volatile stack reader: ARM reports `Compiled 14/14`, `[link] EXIT=0 unresolved=0
duplicate=142`, `Exports: 40`, `SHA256 BF549277…`; x64 `Compiled 13/13`, `unresolved=0 duplicate=0`,
`Exports: 40`, `SHA256 77B3CA74…`.
The bundle is `Src/Apotheosis/AppPackages/Apotheosis_0.2.3.28_Test/Apotheosis_0.2.3.28_arm.appxbundle`
(Release) and `Test-AppxPayload.ps1` reports `AUDIT PASSED` — 30 binaries all arm, driver at the package
root (30.6 MB), `resources.pri` present, **40/40** declared exports. The freshly linked (`Src/port/WebCoreDriver-arm.dll`),
staged (`native/arm/`) **and packaged** `WebCoreDriver.dll` all hash to
`bf54927727c36400…`, so the audit was run against this build's own artifact, not a previous one.
Deployed: `ME.Apotheosis_0.2.3.28_arm__cj4nrm0qt9s1y`, `HTTP 202 in 509 s`, `Success`. **The reader is
still unverified on the device** — this is the first build whose walk provably reads memory (the
`.27` object's window loop loaded `[sp,#0x20]` every iteration), so the maintainer's next reproduction
is the proof. It should log either `ABRT assert(frame#N): file=… function=… (line=… counter=… slot=…)`
/ `ABRT assert(shape): …`, or a wide window whose **`slot` addresses differ line to line** — a repeat
in that column would mean the fold is back.

### `0.2.3.29` — built, audited, deployed

Two changes, both port-side, no WebKit rebuild: the assert reader now **copies** the string it validates
(`ApoImageString` wrote only the terminator, so both nets matched uninitialised stack garbage and could
never fire — the reason `.26`/`.27`/`.28` all named nothing), and the port installs its own
`SocketProvider` (`Src/port/PortWebSocket.{h,cpp}`) so a page that opens a WebSocket cannot abort the
process. See the section below for the assert this fixes and how it was named.

ARM reports `Compiled 15/15`, `[link] EXIT=0 unresolved=0 duplicate=142`, `Exports: 40`,
`SHA256 14E73E2A…`; x64 `Compiled 14/14`, `unresolved=0 duplicate=0`, `Exports: 40`,
`SHA256 B212351D…` (the counts are one higher than `.28` because `PortWebSocket.cpp` is a new TU in
`$Sources`; ARM was already 15 with `gdi-stubs.cpp`).
The bundle is `Src/Apotheosis/AppPackages/Apotheosis_0.2.3.29_Test/Apotheosis_0.2.3.29_arm.appxbundle`
(Release) and `Test-AppxPayload.ps1` reports `AUDIT PASSED` — 30 binaries all arm, driver at the package
root (30.6 MB), `resources.pri` present, **40/40** declared exports. The freshly linked
(`Src/port/WebCoreDriver-arm.dll`), staged (`native/arm/`) **and packaged** `WebCoreDriver.dll` all hash
to `14e73e2aa6a84516…`, so the audit was run against this build's own artifact, not a previous one.
Deployed: `ME.Apotheosis_0.2.3.29_arm__cj4nrm0qt9s1y`, `HTTP 202 in 88 s`, `Success`.

**What the next reproduction should show.** Either **no death at all** on dzen.ru (the expected result —
the WebSocket path now fails asynchronously instead of asserting), or, if something else kills it, an
`ABRT assert(frame#N): file=… function=… (line=… counter=… slot=…)` line from the repaired reader. Both
outcomes are informative; a silent death with no reader line is not, and would mean looking again at the
instrument rather than at the engine.

## `0.2.3.29`: what killed dzen.ru on the device, and why the reader had named nothing

Two separate faults, found in the same sitting and from the same artifact — the maintainer's `.28`
reproduction, pulled as `Src/tools/device-logs/20261009-105721/port-trace.txt` (1,225,857 bytes /
32,140 lines).

### The instrument was still blind: `ApoImageString()` never copied the string

`.28` proved its stack walk reads real memory (every window line carries a distinct `slot`, and module
attribution resolves) and it *still* printed `assert-scan: no argument triple in 1024 words`. The source
looked right, so the compiled behaviour had to differ — and `llvm-objdump` on the `.28` object confirmed
it: the copy loop contained **no store of the scanned byte**, only `strb r3,[r7,r2]` writing the
terminator, after which the code read `buf[n-4..n-1]` for its suffix test. The function computed a length
and validated *that* — while `file[]` and `fn[]` stayed uninitialised stack buffers. The shape net
therefore inspected garbage, and the frame net, which validates its triple through the same shape check,
could not match either. Fix: `out[n] = c;` inside the loop.

### The reader was not needed: the assert is `RELEASE_ASSERT(m_channel)` at `WebSocket.cpp:288`

The `.28` window is now readable by eye, and the four-word argument spill sits at window slots **105–108**
— `line=0x120=288`, `file` pointer, `function` pointer, `counter=0x13D=317` — which is four words
**above** slot 104, the one holding the `frames[2]` return address `5BE3F2CD` (the frame net's `a = i - 4`
offset was therefore wrong; `.29` tries both). Resolving the two pointers through the local packaged
DLL's PE (base `0x5BE30000`, RVA `0x1AF0C2C` and `0x1AF0BD6`) gives:

```
file      WebKit/Source/WebCore/Modules/websockets/WebSocket.cpp
function  ExceptionOr<void> WebCore::WebSocket::connect(const String &, const Vector<String> &)
line      288      counter 317
```

— i.e. `RELEASE_ASSERT(m_channel)` in `WebSocket::connect()`. The channel is null because
`pageConfigurationWithEmptyClients()` installs `EmptySocketProvider`, whose `createWebSocketChannel()`
returns `nullptr`; `ThreadableWebSocketChannel::create()` forwards straight to it. dzen.ru opens a
WebSocket (Yandex Metrika — `mc.yandex.ru/metrika/tag_phono.js` is the last `loader: CALL` before the
abort), so `connect()` hit the assert and `abort()` killed the browser. This is the same empty-client
trap as `.25`'s missing `NotificationController` supplement
([PLAN.md](PLAN.md) §0a), one supplement over.

The fix is `Src/port/PortWebSocket.cpp`: a port-side `SocketProvider` whose channel answers
`ConnectStatus::KO` from `connect()`. `WebSocket::connect()` turns a KO into `failAsynchronously()`
(`WebSocket.cpp:331`), so the page sees an ordinary WebSocket error event and a closed socket — what a
browser without WebSocket support is supposed to look like. It is linkable without a WebKit rebuild
because the port links WebCore **statically** (`WebCoreFull-arm32.lib`).

### The `.29` device answer (2026-10-09): the WebSocket assert did not fire, and an older cure is missing

`Src/tools/device-logs/20261009-132549/port-trace.txt`, 1,458,151 bytes / 38,263 lines, pulled only after
the maintainer closed the browser (`port-trace.txt` answers HTTP 404 to every read while its owner
lives). The first `.29` session (`DEATHNET` at 32149) dies 353 lines later; the second (33023) runs to the
end of the file alive, which matches the report. Two answers:

**The fix worked and something else killed it.** The first navigation is `https://sso.dzen.ru/install?…`
then `sso.dzen.ru/log?…`, both served, then `tail: paintToRGBA exit rc=0 nonWhite=0` — the SSO install
page paints nothing, i.e. the "белое окно" — then `teardown: exit why=load dt=16ms`, then abort. No
`RELEASE_ASSERT(m_channel)` anywhere.

**The abort is the RunLoop `CheckedPtr`-to-dead-object crash, in JavaScriptCore's copy of WTF.**
Resolved against `build-arm32-gpu/jsc.map` (ImageBase 0x10000000): `ABRT #2
JavaScriptCore.dll+0x1EA01B` → `0x101EA00A` + 0x11 =
`CanMakeCheckedPtrBase<std::atomic<unsigned>, unsigned, std::atomic<bool>, 0>::
crashDueToCheckedPtrToDeadObject` (the atomic instantiation — the single-threaded one is at RVA
0x177AA0, and `ScheduledTask` is a `ThreadSafeNode`); the frame above it, `+0x850C27` → `0x10850A60` +
0x1C7 = `RedBlackTree<RunLoop::TimerBase::ScheduledTask, MonotonicTime>::insert`, whose opening `x->
reset()` decrements a stale link left by a previous `remove()`. `tid=4232` is the main thread
(`pump: tid=4232 … isMainThread=1`). `CheckedRef.h:341` is `{ CRASH(); }`, not `WTFCrashWithInfo`, so
**there is no argument triple and the repaired reader was right to print
`no argument triple in 1024 words`** — this one can never be named by that instrument.

**Why it is still there — a header-only cure cannot travel through a relink.** The cure is
`Source/WTF/wtf/RedBlackTree.h`'s `remove()` ending in `z->reset()` under `#if defined(WK_WINUWP)` plus
the `apoRBCheck`/`TREE-BAD-*` probes in `RunLoopGeneric.cpp`, all added **2026-09-19** and measured on the
x64 bench. On the two binaries:

| binary | mtime | `TREE-BAD` string present |
|---|---|---|
| `build-arm32-gpu/bin/JavaScriptCore.dll` (on the Lumia) | **2026-08-23** | **0** |
| `build-x64-gpu/bin/JavaScriptCore.dll` (bench) | 2026-09-27 | **5** |

`apoRBCheck` is in neither ARM map and `TREE-BAD` appears zero times in the trace. The ARM driver
**imports** the one RunLoop (`RunLoopGeneric.cpp.obj` count = 0; `?mainSingleton@RunLoop@WTF@@SA…`
resolves to `JavaScriptCore:JavaScriptCore.dll`), so the tree that aborted lives in
`JavaScriptCore.dll`. **The next ARM step is not another relink: `JavaScriptCore.dll` has to be rebuilt
for ARM32 from current sources.** Until then, no WTF-header change can be claimed to be on the phone.

#### The trace rotates now, so a pull must take two files (2026-10-09)

Closing the browser to make the trace readable is not something to ask of a reproduction, so the trace
is bounded instead: `WebCorePortTrace` (`Src/port/WebCoreDriver.cpp`) and `portLoaderTraceV`
(`Src/port/PortPlatformStrategies.cpp`) rotate `port-trace.txt` at a 4 MB ceiling, each writing a
`trace: rotated …` seam so one rotated session cannot be read as two sessions. The driver's chunk is
`port-trace.1.txt` and the loader's is `port-trace.2.txt` — **distinct names on purpose**: the rotation's
destructive step is `remove(previous)`, and a shared name would let the second writer, rotating on its
own stale count, delete the chunk the first had just moved aside. Each re-probes the real file size
before deciding, for the same reason. **Pull `port-trace.1.txt` and `port-trace.2.txt` as well as
`port-trace.txt`** — both are in `pull-device-logs.ps1`'s default list. The read-side 404 while the app
lives is unchanged and is not a size limit; rotation bounds the damage, it does not lift it.

**It has never fired, and the reason is now known (2026-10-09, second `.30` run).** Measured:
`port-trace.txt` reached **6,223,695 bytes / 140,805 lines** with no `port-trace.1.txt` and no
`port-trace.2.txt` present, and it still carried the `.26`–`.29` sessions — so rotation has not fired
once in this install's whole life, not merely in that session. The code is deployed (the
`rotated out at the size ceiling` string byte-searches clean in the staged ARM driver, which was
staged 22:11:24 and packed at 22:14:47) and `ApoGestureMark` writes through `WebCorePortTrace` dozens
of times a session, so the rotation path is executed constantly.

The cause is a sharing violation on the destructive step, and it is silent. `apoTraceAppend`
(`WebKit/Source/WTF/wtf/generic/RunLoopGeneric.cpp:57`) keeps its `FILE*` in a function-static and
**never closes it**:

```cpp
static FILE* s_file = nullptr;      // process-lifetime, and every rlife:/wtimer: line goes through it
...
s_file = fopen(path.c_str(), "ab");
fputs(line, s_file);
fflush(s_file);
```

MSVC's `fopen` opens without `FILE_SHARE_DELETE`, so `rename(path, previous)` in
`portTraceRotateIfNeeded` fails with `ERROR_SHARING_VIOLATION` for as long as that handle lives — and
the handler for that case is `return;` with the comment "still over the ceiling; the next probe tries
again", i.e. it retries forever and reports nothing. `remove(previous)` fails the same way (harmlessly,
the file does not exist yet). The engine's writer is in a different module from the driver's, so this
is not visible from the rotation site at all.

Nothing about the ceiling constant or the two-name scheme is wrong; the rename simply cannot succeed
while an engine-side handle is open. A fix has to either make the engine writer open/append/close per
line (as `WebCorePortTrace` does) or have the rotation fall back to copying the chunk aside and
truncating in place. **Until then, treat a pull's `port-trace.txt` as unbounded and pull it whole** —
the `.1`/`.2` names in the default list are harmless to ask for but will always 404.


## Status measured 2026-10-03 (night, later) — the `.24` pull answers "gesture or close", and the engine death has a port-side fix

`0.2.3.25` is built, audited and deployed (`ME.Apotheosis_0.2.3.25_arm__cj4nrm0qt9s1y`, HTTP 202 in
33 s, `AUDIT PASSED`, 30 binaries all arm, 40/40 declared symbols exported). It carries the fix for the
fault that killed two of the maintainer's four `.24` tests. **The fix is unverified on the device** — it
needs the maintainer's next run, and that run is the proof, not this paragraph. Deployed **unlaunched**.

### What the `.24` pull said: no gesture was in flight

The file carries `gesture:` bursts for `close` and the idle `live` tick only — **no `scroll`, `click`,
`zoom` or `resize` anywhere in 5,838 lines**, across four tests that included a blocked pinch and a
blocked scroll. So the instrument's question has an answer, and it is not the one the report implied:
the deaths arrived **idle, mid-load, after a completed teardown**. The frozen pinch and scroll are a
consequence of a dead process, not its cause.

| Session | Lines | Ends with | Verdict |
|---|---|---|---|
| A | 1–354 | `VEH: … at SharedLibrary.dll+0x363B4D op=1 faultaddr=0x0` (n=1), then `CoreUIComponents.dll+0xD4347` (n=2) | **not an engine death** — no `teardown:`, no engine fault; the write-AV pair that also ended `.23` §1 |
| B | 355–3153 | `VEH: … at WebCoreDriver.dll+0x37D161 op=0 faultaddr=0x8`, after `wtimer #85` never exits | engine death, same class of site as `.23` |
| C | 3154–5838 | identical, after `wtimer #79` never exits | twin of B |

Both fatal sessions: `gesture: close end n=1 dt=0ms idle=8495ms` / `idle=8981ms` → five `gesture: live`
ticks → `teardown: enter why=load` → `teardown: exit why=load dt=17ms` / `dt=8ms` (**completed**) →
~1,400 lines of load / MIME / timer work → the AV. No `cont` without an `end` anywhere in the file.

**Caveat, now measured.** The `end` line is written by a later mark (the 1 Hz live tick will do), so an
open burst means "died during this gesture **or** before the next mark". Session A exhibits the false
positive: `gesture: close enter` with no `end`, in a session that died in platform code the port never
reaches. Read an open burst together with the fault class, never instead of it.

Three `started` lines in `mem.txt` for four tests: one launch never reached engine init.

### The root cause, named to the instruction

`Document::notificationClient()` (`WebCore/dom/Document.cpp`) ends in
`return &NotificationController::from(page.get())->client();` — **no null check** — and
`NotificationController::from(Page*)` is
`downcast<NotificationController>(Supplement<Page>::from(page, "NotificationController"_s))`. The
supplement is installed by the **embedder**: the only in-tree caller of `provideNotification()` is
`WebKitLegacy/mac/WebView/WebView.mm`, and this port builds its Pages with
`pageConfigurationWithEmptyClients()`. The lookup therefore answers *absent*, and the unguarded deref
reads the value pointer of a null hash-table entry, i.e. address **+8** — exactly the observed
`faultaddr=0x8`.

The disassembly agrees at the instruction: `1037d160: 6885 ldr r5, [r0, #0x8]` with `r0 == 0`, reached
from a `bl` to an ICF-folded `WTF::HashTable::lookup` whose key literal on the stack is
`??_C@_0BH@CBCLEOFL@NotificationController?$AA@`.

**This supersedes the `.23` reading.** `faultaddr=0x8` is not "a dead `Document` whose frame/page chain
is gone"; that chain is guarded with `cbz` at every step. It is the missing supplement.

### The fix: install the client the embedder owes

`Src/port/WebCoreDriver.cpp` gains `PortNotificationClient` (the six pure virtuals of
`NotificationClient`; `show` answers `false`, every permission answers `Denied`) and
`installPortNotificationClient(Page&)`, called at all four `Page::create` sites the driver has —
`buildSession`, `WebCoreRenderHtml`, `WebCoreLoadUrl` and the w×h probe. One immortal instance serves
every Page: `NotificationController` holds a **reference**, not an owner, so the client must never be
deleted. `provideNotification` is already exported by `WebCoreFull-*` on both lines, so this is a
**relink, not a rebuild** — no WebKit header is touched, and a WebCore header edit would have cost a
362-unified-source run.

### The build-script defect found on the way

`llvm-lib` cannot infer the target machine from a `.def` alone (MS `lib.exe` reads it off the `.obj`
files; here there are none), so `llvm-lib /OUT:… /DEF:…` fails with `/def option requires /machine to
be specified`. Under PowerShell 5.1 that stderr terminated the script *before* its export count and its
payload staging, leaving the previous DLL in `native\<arch>` after a clean link — the exact "my fix did
nothing" failure the staging comment warns about; the ARM env script dies the same way on its own `cl`
banner. Under PowerShell 7 the same call fails **silently**, so the import library was never
regenerated. Fixed with `/MACHINE:$Machine` plus an explicit exit-code check, and the ARM line is now
run under **`pwsh`** (7), not `powershell.exe` (5.1).

### Reference RVAs for the next pull

The `.25` relink regenerated `Src/port/WebCoreDriver-arm.map` (2026-10-03 22:29, 42,003,452 bytes), so
the `.24` numbers are dead as well. Every WebCore symbol moved **+0xB0**; the driver's own code moved by
the size of the inserted block. The preferred load address stays `10000000`; the **device** base is what
the log proves, not what the map says — re-derive it per run with exact symbol hits.

| Symbol | RVA in the `.25` image |
|---|---|
| `??_7Timer@WebCore@@6B@` | `0x1859FC8` |
| `??_7EventLoopTimer@WebCore@@6B@` | `0x186541C` |
| `Document::notificationClient()` | `0x37D1EE` |
| `WebCore::provideNotification` | `0x86a208` |
| `??_C@_0BH@CBCLEOFL@NotificationController?$AA@` | `0x18A483F` |

**Side finding, memory.** This pull's `mem.txt` carries `PRESSURE level=low used=9,8MB` 16 ms after a
process start — the memory-pressure handler **has** fired now, which retires the standing claim that it
never has. Peak in the same file is `used=192,2MB pct=12`, so the quota remains excluded.

## Status measured 2026-10-03 (night) — the input vocabulary exists, and `.24` is on the phone

`0.2.3.24` is built, audited and deployed (`ME.Apotheosis_0.2.3.24_arm__cj4nrm0qt9s1y`, HTTP 202 in
30 s, `AUDIT PASSED`, 30 binaries all arm, **40/40** declared symbols exported). It carries the fix
for item 1 of the "next build must fix" list below — **the port now names the gesture**. Port-side
only: one relink, no WebKit rebuild, no header edit, no managed change. The app was deployed
**unlaunched**, because the crash needs a real finger and the run is the maintainer's.

The grammar, the rules and the reasoning are in `Doc/Archive/Summary.md` under "2026-10-03 (night) —
the port learns to name the gesture". What matters here is how to read the next pull:

| Line | Means |
|---|---|
| `gesture: <verb> enter` | a burst began. Verbs: `scroll`, `click`, `zoom`, `resize`, `close` |
| `gesture: <verb> cont n=… dt=…ms` | the burst is still running — at most one line per 500 ms |
| `gesture: <verb> end n=… dt=…ms idle=…ms` | the burst finished; written by the idle sweep, which the 1 Hz live tick also runs |
| `gesture: live n=…` | the present tick is still alive — one line per 2 s while the app is idle and foreground |
| `teardown: enter why=…` / `exit why=… dt=…` | who asked for the teardown (`load`, `load-failed`, `close`, `paint:frameGone`, `click:frameGone`) and whether it finished |

**A `cont` with no `end` after it is the statement "the engine died during this gesture."** An abort
that follows `teardown: enter` with no `teardown: exit` is *inside* the teardown, with `why=` naming
the caller — which is exactly what the `.23` file could not say, because two of the five teardown
callers are gesture routes.

**The map trap applies to this build too.** The `.24` relink regenerated
`Src/port/WebCoreDriver-arm.map` (2026-10-03 21:40, 42,000,503 bytes), so the `.23` base `0x5F9D0000`
is dead. Re-derive the base per build and prove it with *exact* symbol hits.

**Side finding, x64 line.** The shipped `native\x64\WebCoreDriver.dll` was one export behind the
`.def` — 39 of 40, missing `WebCoreReleaseMemory`. Its only managed caller catches the resulting
`EntryPointNotFoundException` and merely logs it, so a stale payload degrades the bench **silently**.
Rebuilt; both payloads carry 40 now.

**Still open, unchanged:** item 2 below (the partial stack capture — 0 frames in the VEH, 3 in the
ABRT handler, so the caller above `expandCapacity` is not in the log) and the crash trigger itself.

## Status measured 2026-10-03 (late) — the death net fired on the device, and named three signatures

The first device run with the net installed. Six launch attempts by the maintainer; **four reached the
engine** and left a trace (four `DEATHNET: installed` blocks in one cumulative `port-trace.txt`).

**The net installs and fires, and the App Container permits it.** `DEATHNET:
RtlAddVectoredExceptionHandler -> 091EE738` (a real handle — the open question when `.23` shipped is
answered: yes). `DEATHNET: signal(SIGABRT) prev=00000000 _set_abort_behavior prev=0x2` — `prev=0x2` is
`_CALL_REPORTFAULT`, so this is measured confirmation of why every earlier device death was silent.
The only exception code the net files as "routine" is `0x40010006` (`DBG_PRINTEXCEPTION_C`, i.e.
`OutputDebugString`), counted and rate-limited, so the net can stay in.

**What died, by section.**

| § | Lines | What it was doing | How it ended |
|---|---|---|---|
| 1 | 350 | engine init, no paint | write-AVs to 0 in `SharedLibrary.dll` and `CoreUIComponents.dll` |
| 2 | 2991 | a real load, many timer fires | AV **reading `0x8`** inside `Document::notificationClient()` (`WebCoreDriver.dll+0x37CB05`), inside timer `#99`'s callback, after `String::lower("charset")` |
| 3 | 348 | load started, `nonWhite=0` | `abort()` from `JavaScriptCore.dll+0x1EA01A` at **teardown** |
| 4 | 332 | load started, `nonWhite=0` | identical stack to §3 |

Both aborts symbolise to `WTF::Vector<WTF::UniqueRef<Inspector::InspectorAgentBase>>::expandCapacity
<FailureAction::Crash>` → `reserveCapacity<Crash>`, whose only caller of that exact instantiation is
`Inspector::AgentRegistry::append` (`m_agents.append`). Reached after a clean `quiet=15` settle,
`RunLoop::run() returned`, a 15 s `mode=drain` sleep and `loader: MARK teardown …`.

**So the shape is one thing: the engine touching something already destroyed.** A dead `Document`
reachable from a timer callback; an agent registry appended to while the session tears down. Same
family as the dead-`ScheduledTask` aborts, the hidden `Page`s that outlive teardown, and the
throwaway-`Page` design.

**The memory-quota hypothesis is dead, measured.** `mem.txt` for the same session peaks at **179.5 MB
of a 1536 MB quota**, `pct=11`, `level=low` throughout.

**Two things the next build must fix, before another device run.**

1. **The port trace has no input vocabulary.** Its prefixes are `rl`, `APOTRACE`, `settle`, `wtimer`,
   `loader`, `rlsleep`, `tail`, `pump`, `twdct`, `DEATHNET`, `VEH`, `rllock`, `PS`, `spa`, and the font
   markers. Not one is a gesture, so this file cannot tie a death to the pinch or the scroll the
   maintainer saw. Add a marker on the engine entry points a gesture can reach.
2. **The stack capture is partial.** `RtlCaptureStackBackTrace` returned **0 frames** in the VEH and
   **3** in the ABRT handler, so the caller above `expandCapacity` is not in the log. Do not read the
   three frames as the whole stack.

Of those two, **item 1 is done in `0.2.3.24`** — see the night section above. Item 2 is still open.

Minor and not concluded: one timer (`#90`) reported `vtbl=6126EC44`, which resolves into a
`.Lswitch.table` rather than any vtable — and it entered and exited cleanly, so it is not the killer.

**Address resolution, and the trap in reusing it.** Base `0x5F9D0000` is proved for the `.23` runs by
two **exact** symbol hits (`61229DE8` → `??_7Timer@WebCore@@6B@`, `6123523C` →
`??_7EventLoopTimer@WebCore@@6B@`). But the `.22` numbers do **not** transfer: this session's relink
regenerated `WebCoreDriver-arm.map` and moved them, and the `.22` dying value `61228B38` appears nowhere
in the `.23` trace — resolving it against the `.23` map lands on a string literal, which reads as an
answer and is not one. **Re-resolve every address against the map of the DLL that produced the log.**

## Status measured 2026-10-03 (evening) — the device death is localised, and the device line now has a death net

`0.2.3.23` is built, audited and deployed (`ME.Apotheosis_0.2.3.23_arm__cj4nrm0qt9s1y`, HTTP 202 in
317 s, `AUDIT PASSED`, 30 binaries all arm, 40/40 declared symbols exported). It carries one change:
a **port-side death net** in `Src/port/WebCoreDriver.cpp`, which is the first thing on the ARM32 line
that can witness a fault at all. It needs no WebKit rebuild (see the blocker below) and no header
edit, so it cost one port relink.

### The addresses in `port-trace.txt` are now readable, and that turned out to be enough

The trace names objects by raw pointer, which has been useless offline because no log recorded a
module base — and one could not be recovered from the addresses alone, since they span >100 MB while
the driver image is 31 MB. It *can* be recovered from the **linker map**, which lists every symbol's
`ImageBase + RVA` including all **5327 `??_7` vtables**:

* a 64 KB-aligned base pins the low 16 bits of every RVA, so only map symbols with matching low bits
  are candidates: exactly **one** vtable in the whole map ends in `8B38`;
* that candidate is `??_7Timer@WebCore@@6B@` at RVA `0x1858B38` → base **`0x5F9D0000`**; the two other
  vtables observed in the same window (`0x44E5C` and `0xB454` further on) are also real `??_7` entries
  at that base, so the triple intersection is decisive rather than a guess.

With the base known, `Src/port/WebCoreDriver-arm.map` (41 MB, 188 111 symbols) resolves everything:

| Recorded | Runtime | Resolves to |
|---|---|---|
| dying timer `vtbl` | `61228B38` | `??_7Timer@WebCore@@6B@` — a plain `WebCore::Timer` |
| timers #129–130 `vtbl` | `61233F8C` | `??_7EventLoopTimer@WebCore@@6B@` |
| timers #125–128 `vtbl` | `6126D994` | `??_7DeferrableOneShotTimer@WebCore@@6B@` |
| `caller=60226687` | `60226687` | `ParsedContentType::setContentTypeParameter` +0xA7 |
| `caller=602265B7` | `602265B7` | `WebCore::collectHTTPQuotedString` +0x22F |
| `impl=61861F58` | `61861F58` | RVA `0x1E91F58`, `.rdata` — the static empty `StringImpl` |

`0x633BBF0D` lies **above** `base+SizeOfImage` (`0x618C9000`), i.e. a second module is involved in that
one marker; `0x5B1992EF` lies below the base. Neither is identified, and neither is load-bearing.

**So the death is inside the callback of a one-shot `WebCore::Timer`, fired on the main thread in a
burst right after four tracker loads, with MIME content-type parsing (`String::lower("charset")`) as
the last thing that completed.** The `wtimer` pair is unbalanced 133/132 exactly as reported.

### Why the diagnostic could not be an engine edit

`ninja -n -C build-arm32-gpu WebCore` reports **7400 pending edges**, and asking for the single
unified object that carries `ThreadTimers.cpp` still reports **6462** — the generated `DerivedSources`
are stale, so the PCH is rebuilt, so every unified source goes dirty. The ARM WebCore tree therefore
**cannot be rebuilt incrementally at all**; any engine edit is a multi-hour full rebuild. The tree is
otherwise intact and safe to *link* against: 1516 objects present, and no cmake input is newer than
`build.ninja`, so ninja will not reconfigure it.

### The death net: what it is and what it will say

On ARM32 the port's SEH wrappers compile to nothing (`APO_SEH_SUPPORTED 0` — clang-cl will not emit
`__try` for `thumbv7a`), which the comment at `WebCoreDriver.cpp:455` already admitted: *"there is no
ARM guard yet."* A Vectored Exception Handler is a **runtime call rather than a compiler feature**, so
it does reach that target. Installed with `First=1` it runs ahead of every SEH frame and is a pure
observer — it always returns `EXCEPTION_CONTINUE_SEARCH`, so a run with the net up is the same run.

Installed from `WebCoreSetFontConfigFile` (the managed host's first call, and the one that arms the
trace channel) and again from `WebCoreSetSehGuard`, idempotently. It reports:

* `DEATHNET: RtlAddVectoredExceptionHandler -> <handle>`, or a refusal — **a null handle is a finding
  in itself**: it would mean the App Container denies the hook and the device has no first-chance
  witness at all;
* `VEH: code=0x… at module+0xRVA op=… faultaddr=0x… n=…` for the codes that can end the process —
  access violation, **illegal instruction (`0xC000001D`, i.e. WebKit `CRASH()`/`RELEASE_ASSERT`)**,
  fast fail, **stack overflow (`0xC00000FD`)**, heap corruption, breakpoint — plus a `module+0xRVA`
  stack of 24 frames;
* `ABRT: abort() reached the CRT signal handler` with its own stack, after
  `_set_abort_behavior(0, _CALL_REPORTFAULT)` — the flag whose default turns `abort()` into the Watson
  fast-fail that made every earlier death silent.

`RtlAddVectoredExceptionHandler` is resolved **at run time from the already-mapped ntdll** rather than
linked. The C++ harness added `ntdll.lib` through a `#pragma comment` and that worked on x64, but
linking it here would put ntdll in the driver's import table on *both* architectures — a change to the
bench line for a diagnostic only the device line needs. The two-call lookup keeps the x64 link
untouched and turns a possible link failure into a logged line.

### What the next device run has to be

Open `dzen.ru`, let it paint, then scroll with a real finger — synthetic input cannot reach the UWP
input stack, so a scripted gesture cannot produce this death. Expect a `DEATHNET:` line early in
`port-trace.txt`; whatever follows a `wtimer: enter` with no exit is the answer, and both the code and
every address in it will now be symbolizable against `WebCoreDriver-arm.map` with the base above.

## Status measured 2026-10-03 — the payload exists, and it is staler than its own sources

The 2026-09-26 measurement below is kept as the record of how the driver DLL was made to build. What
followed it was never re-measured, and the main reason the ARM line is now the priority is in this
table.

| Item | State on 2026-10-03 |
|---|---|
| `Src/Apotheosis/native/ARM` | **populated** (26 Sep 21:22) — 22 DLLs incl. `WebCoreDriver.dll` 30.6 MB. The table below says "empty (0 files)"; that was true when it was written and is not true now |
| ARM payload age | **was stale, and is now current.** Staged 26 Sep 21:21 while `Src/port/WebCoreDriver.cpp` / `WebCoreDriver.h` were edited 27 Sep 04:57. The second suspect, `stubs-font-uwp.cpp` (27 Sep 04:13), turned out to be **orphaned** — no build script and no vcxproj compiles it; it survives only as comments in `WebCoreDriver.cpp`, `stubs-other.cpp` and itself. Relinked 2026-10-03, so the only source that mattered is in |
| **Export surface** | **was 37 against 39 on x64** — missing `WebCoreSessionResize` and `WebCoreSetPageZoom`, the viewport pair added to `WebCoreDriver.exports.def` *after* that payload was linked. `WebCoreDriver.exports.def` names this exact failure mode in its header: a function absent from the list fails as **`EntryPointNotFoundException` at runtime, not at compile or link time**. **Closed 2026-10-03**: the relink reports `Exports: 39 functions` and both names are in the staged DLL's export table (`llvm-readobj --coff-exports`) |
| What that would have done on the device | `about:home` reaches the UI, and the first viewport apply (`ApplySettings` → `AdoptViewport` → `PostZoomAndRepaint`) throws. Nothing about the ARM app was testable until the payload was relinked — and this is why it was worth relinking *before* spending device time |
| ARM appx | **was never built; now built.** `AppPackages\Apotheosis_0.2.3.19_Test\Apotheosis_0.2.3.19_arm.appxbundle` (46.9 MB, Release) and `Apotheosis_0.2.3.19_Debug_Test\Apotheosis_0.2.3.19_arm_Debug.appxbundle` (47.9 MB). Both on the first try — see the results block below |
| C# → device deploy path | **does not exist.** `install-apotheosis-x64.ps1` is x64-only; `Wdp-Deploy.ps1` is the harness-era WDP pusher and is reusable but targets no package |
| `WebCoreFull-arm32.lib` | **present** (3.3 GB, 26 Sep 20:09) — so the ARM driver is a **relink**, not a WebKit rebuild. This is the fact that makes the whole line cheap: no half-day ninja run |
| `/FORCE:MULTIPLE` + `duplicate=142`, `LNK4088` | deliberate and documented below (WTF objects live in both `WTF.lib` and `JavaScriptCore.lib`). The remaining risk is not the flag, it is that this image has **never executed anywhere** |
| UWP CRT on ARM (`MSVCP140_APP.dll` …) | **resolved in principle, gated in practice.** The driver's imports are `MSVCP140_APP.dll`, `MSVCP140_2_APP.dll`, `VCRUNTIME140_APP.dll`; none ship in the payload, so they must come from the `Microsoft.VCLibs.140.00` framework the manifest now declares at **MinVersion 14.0.33519.0**. A phone whose VCLibs is older refuses the install. Verify the version on the device before blaming the engine — see the results block below |

**Ordered docket for the first ARM run** (steps 1–2 need no device — both **done 2026-10-03**):

1. ~~relink the ARM driver from current sources (`build-driver-dll-arm32.ps1`) and **assert 39 exports,
   not 37**~~ — **done.** `Exports: 39 functions`, both viewport names verified in the *packaged* DLL;
2. ~~build the ARM appx for the first time (`Apotheosis.csproj -p:Platform=ARM`)~~ — **done**, Debug and
   Release, on the first try with no source changes needed;
3. add a managed deploy path for the Lumia (WDP), with the standing rules: back up `LocalState`
   first, never `Remove-AppxPackage` without it;
4. first run on the device, reading `boot.txt` / `stage.txt` / `setup-trace.txt` / `port-trace.txt` —
   the per-line flushed markers survive the device, the dumps do not;
5. named suspects for that run: the App Container (network, filesystem), the ABI (including the Thumb
   bit), .NET Native under an App Container, the font stack, the never-executed ARM image, and the CRT
   provisioning above;
6. **rebuild `JavaScriptCore.dll` for ARM32 from current `WebKit/Source/WTF` (added 2026-10-09).** The
   relink model carries port-side changes only; the `.29` device death is a WTF-header cure
   (`RedBlackTree.h`'s `remove()`/`z->reset()`) that the ARM line never received, because the ARM
   `JavaScriptCore.dll` predates it (2026-08-23 vs the 2026-09-19 fix). Verify by mtime **and** by the
   `TREE-BAD` string being present in the rebuilt binary — a stale DLL is indistinguishable from a
   broken fix at runtime.

   **Run it at `-j4` (revised the same day from `-j1`).** The bench cannot be handed over for the
   duration, so the run has to share the machine, and the first decision was the cautious one. Two
   measurements moved it to `-j4`: the maintainer raised the CPU clock (it had been running at ~80 %),
   and the pagefile is 8 GB on a machine with **4 logical processors and 3.9 GB of RAM**. At `-j1` the
   estimate was ~7 h and at `-j4` ~4-5 h, and the swap is the buffer that makes the wider setting
   survivable rather than a claim that memory exists. Expect the free-memory figure to sit under 1 GB
   and the pagefile to be busy — that is the setting working, not failing. Drop to `-j3` if it thrashes.
   Command: `ninja -C build-arm32-gpu -j4 bin/JavaScriptCore.dll` (2 650 steps, the full target — the
   WTF headers sit in the PCH, so no smaller target carries the cure). `build.ninja` is dated 2026-08-22
   and **must not be regenerated** — do not touch any WebKit cmake file or `CMakeCache.txt` in that
   directory; back the directory up first (done 2026-10-09: `D:\Backups\build-arm32-gpu_2026-10-09`,
   13 219 files, robocopy exit 1, timestamps preserved — the copy holds real files where the source has
   4 655 CMake `PrivateHeaders` symlinks, which is why it measures ~25 MB larger).

   **Source *both* environment scripts, or the link fails.** `Src\setenv.ps1` sets the project paths;
   `Src\port\arm32-uwp-env.ps1` is what sets `LIB`/`INCLUDE` to the UWP ARM SDK. Running ninja with only
   the former compiles happily — clang-cl gets every `-I` from `build.ninja` — and then dies on the
   first *link* with `lld-link: error: could not open 'ucrt.lib'`, because the host tool
   `bin/LLIntSettingsExtractor.exe` (a build-time generator linked `/machine:ARM`) resolves the CRT
   through `LIB`. Observed exactly once, at step 1996/2650, 2026-10-09. The canonical order is in
   `Src\port\build-webcore-arm32.ps1`: `setenv.ps1`, then `arm32-uwp-env.ps1`, then ninja.

   **Done 2026-10-09.** `NINJA_EXIT=0` in 35 minutes (a first aborted run had already built ~2000 of
   the 2650 edges). Verified on the binary: mtime `2026-10-09 21:55:31`, 10.42 MB, `COFF-ARM` /
   `IMAGE_FILE_MACHINE_ARMNT (0x1C4)`, and **all six `TREE-BAD-*` markers PRESENT** where the
   pre-rebuild DLL had none. The cure is now in the ARM engine. The artifact that carries it is
   `Apotheosis_0.2.3.30_arm.appxbundle` — staged driver 40/40 exports, `AUDIT PASSED`, and both engine
   DLLs extracted from the .appx hash-match their builds. **What this does *not* yet settle is the
   device:** no run has happened, so whether the `.29` dzen.ru death is gone is still open.

   **How the device proves which `JavaScriptCore.dll` it is running — and it costs nothing.** The
   install folder cannot be read: WDP answers `HTTP 403` to `knownfolderid=InstallationFolder` (and to
   `Package`) in every spelling tried — `path` empty, `%5C`, `\` — so a payload DLL on the phone cannot
   be hashed over Device Portal. Only `LocalAppData\LocalState` is readable. It does not need to be.
   The rebuilt WTF carries `apoRLLife`, which appends one
   `rlife: <what> a=<ptr> b=<ptr> count=<n> extra=<n> tid=<n>` line per event to the same
   `port-trace.txt` the puller takes, and several of those events are unconditional: `loop-create` at
   RunLoop construction, `sched`/`unsched` on every timer, `task-destroy` on every `ScheduledTask`.
   Measured on the `.29` trace, whose engine is the 2026-08-23 DLL: **zero** `rlife` lines in 38,263.
   A `.30` session containing any `rlife:` line is therefore proof by the engine's own hand that the
   rebuilt `JavaScriptCore.dll` is installed and executing — which is exactly the confusion that cost
   the `.29` round, and it needs no version bump and no extra instrument.

### Results of docket step 2 (2026-10-03) — the ARM appx builds, and the Debug one cannot be installed

Both configurations built on the first attempt, with no project-file change beyond stale comments
(the "folder currently empty" note and a placeholder `<Folder Include="native\ARM\" />` that is now
superfluous, both removed). What was then *checked* on the artifacts, rather than assumed:

| Check | Result |
|---|---|
| Bundle identity | `ProcessorArchitecture="arm"`, `Version="0.2.3.19"`, `Name="ME.Apotheosis"` — the ARM bundle is not an x64 bundle with ARM in the filename |
| Payload placement | all 22 engine DLLs plus `Apotheosis.dll` sit at the package **root**, so the `Link=%(Filename)%(Extension)` flattening in `Apotheosis.csproj` works; `WebCore.dll` is absent from both arches, as the glob intends |
| Architecture of every binary | 25 files in Debug, 24 in Release, **every one `0x01C4` (ARMNT)**. No x64 DLL leaked in — the failure that would only surface as `LoadPackagedLibrary` refusing a module on the device |
| Import closure | every import of `WebCoreDriver.dll` and `libcurl.dll` that is not an `api-ms-win-*` API set or `WS2_32` resolves to a file **in the package**: `JavaScriptCore`, `fontconfig-1`, `freetype`, `harfbuzz(-icu)`, `icuin78`, `icuuc78`, `jpeg62`, `libEGL`, `libGLESv2`, `libcrypto-3-arm`, `libcurl`, `libpng16`, `libssl-3-arm`, `libwebp`, `libwebpdemux`, `libxml2`, `pixman-1-0`, `z`. The `-arm` suffix in the libcurl imports matches the shipped filenames exactly, so the OpenSSL pair is not a name mismatch |
| Export table of the **packaged** DLL | 39 `WebCore*` exports, including `WebCoreSessionResize` and `WebCoreSetPageZoom` — the defect of docket step 1 is closed in the artifact that would actually be deployed, not only in the staging directory |
| `resources.pri` | present (25 KB). 94 `type="String"` candidates, all `Language-EN-US` `isDefault="true"`, e.g. `Back` → `Back`, `AboutUpdate` → `About / Update`. This is the historically dangerous one: a missing PRI gave the whole chrome blank labels. The chrome will have text |
| Bundle language packages | `language-ru.appx` carries `<ResourcePackage>true</ResourcePackage>` and `<Resource Language="ru-ru"/>`, `language-zh-hans.appx` likewise — the standard bundle split, so the ru-RU strings the x64 plain `.appx` holds inline (94 RU-RU candidates, `Back` → `Назад`) still reach a Russian phone |
| Log/trace files in the package | **none**, in either the ARM or the x64 package. The "49 MB `port-trace.txt` shipping in the appx" item is withdrawn: `port-trace.txt` is a runtime `LocalState` artifact and is never packaged |

These four checks are now automated in [Src/tools/Test-AppxPayload.ps1](../Src/tools/Test-AppxPayload.ps1),
so they are re-run on every ARM build rather than remembered; run it per §8 before deploying. On both
current bundles it reports `AUDIT PASSED`.

**The one real obstacle found, and it is a Debug-vs-Release difference, not an engine problem.** The
dependency block of each bundle:

| Configuration | Declared dependencies |
|---|---|
| **Debug\|ARM** | `Microsoft.NET.Native.Framework.**Debug**.1.7`, `Microsoft.NET.Native.Runtime.1.7`, `Microsoft.VCLibs.140.00.**Debug**` (≥ 14.0.33519.0), `Microsoft.VCLibs.140.00` (≥ 14.0.22929.0) |
| **Release\|ARM** | `Microsoft.NET.Native.Framework.1.7`, `Microsoft.NET.Native.Runtime.1.7`, `Microsoft.VCLibs.140.00` (≥ 14.0.33519.0) |

A Debug UWP package binds to the **debug** framework packages, which a retail Lumia does not have
installed. `Add-AppxPackage` on such a phone fails on the unmet dependency — the same *shape* of
failure as the `0x80073CF3` seen on the bench, but a different cause. The Debug payload also ships
`ucrtbased.dll`, the debug UCRT, which Release does not.

So **deploy Release\|ARM to the device.** It is not merely tidier: it is the only one of the two whose
framework dependencies a phone should already satisfy. Two consequences:

- the CRT question above is answered — `MSVCP140_APP` / `MSVCP140_2_APP` / `VCRUNTIME140_APP` come from
  `Microsoft.VCLibs.140.00`, which the manifest requires at ≥ 14.0.33519.0;
- but that minimum is itself a gate. If the phone's `Microsoft.VCLibs.140.00` is older than
  `14.0.33519.0`, the install is refused before any of our code runs. It is a one-line check over WDP
  and belongs at the top of the device session, before any engine theory.



## Status measured 2026-09-26

Before planning an ARM rebuild, the actual state of `build-arm32-gpu` was measured rather than assumed.

| Item | State |
|---|---|
| `build.ninja` | 2026-08-22 08:52 — **~5 weeks stale** |
| Objects on disk | 1516 `.obj` (3.4 GB); 1512 / 1548 `.obj` edges (97.7%) present |
| `.ninja_log` | **absent** — see [LESSONS.md](LESSONS.md); treat the tree as needing a full ninja run |
| `.ninja_deps` | present, 2026-08-23 20:51 |
| Engine archives | `lib\WebCore.lib` (5 MB, import), `lib\JavaScriptCore.lib`, `lib\WTF.lib`, `lib\PAL.lib` |
| Engine DLLs | `bin\WebCore.dll` (33.2 MB), `bin\JavaScriptCore.dll` (10.4 MB) + PDBs |
| `build-arm-gpu` | empty — do not use |
| `native\ARM` | **empty (0 files) at the time of writing** — the payload was staged later the same evening, see the 2026-10-03 table above |

**The ARM line needs its own `WebCoreFull-arm32.lib` after all.** (An earlier note in this file said
otherwise; that was wrong, and the link log proves it.) Linking ARM in import mode leaves
`stubs-other.arm32.obj` with unresolvable WebCore *internals*:

```
LNK2019: WebCore::RenderTheme::RenderTheme()            (protected ctor)
LNK2019: WebCore::SystemFontDatabase::SystemFontDatabase()
LNK2019: WebCore::WebKitFontFamilyNames::standardFamily
LNK2001: WebCore::RenderTheme::purgeCaches / baselinePosition / supportsFocusRing / isControlStyled
LNK2019: __imp_cairo_scaled_font_status / _extents / _text_extents
```

The tempting argument for import mode — "as `__imp_` references they resolve at load time" — is only
true for **exported** symbols. `RenderTheme::RenderTheme()` is `protected` and is not in
`WebCore.dll`'s export table, so no `__imp_` thunk exists for it and there is nothing to resolve at
load time. x64 avoids this precisely because it links WebCore **statically**. The same
`WebCoreFull` idea is therefore required on ARM, built from the 963 WebCore objects in
`build-arm32-gpu\Source\WebCore`:

```powershell
$objs = Get-ChildItem build-arm32-gpu\Source\WebCore -Recurse -Filter *.obj
'"' + $_.FullName + '"'   | Set-Content build-arm32-gpu\WebCoreFull-arm32.rsp
& "...\MSVC\14.44.35207\bin\Hostx64\arm\lib.exe" /NOLOGO `
    /OUT:build-arm32-gpu\lib\WebCoreFull-arm32.lib "@build-arm32-gpu\WebCoreFull-arm32.rsp"
```

Note the path: `bin\Hostx64\arm\lib.exe`, not `Hostx64\x64` — a static library records its target
machine, so an x64 `lib.exe` would produce a library that `link.exe /MACHINE:ARM` cannot use.

**This also casts doubt on the x64 script change of 2026-09-26.** `build-driver-dll-arm32.ps1` was
switched to import mode (`WebCore.lib`) for both architectures on the reasoning that the full archive
wanted ICU 75. But `WebCoreFull-x64-19041.lib` has just been rebuilt from the 25.09 objects, which are
ICU 78, and the shipped x64 `WebCoreDriver.dll` demonstrably does **not** import `WebCore.dll` — it is
static. If the x64 branch is re-run as it stands, it should fail with the same internal-symbol errors.
That is the line under active investigation, so verify it deliberately rather than discovering it.


**The old "no ARM ICU exists" blocker is stale.** `bin\WebCore.dll` imports **`icuin78.dll` /
`icuuc78.dll`**, i.e. the ARM engine was built against ICU 78 from `C:\vcpkg\installed\arm-uwp`,
not ICU 75. The script's `$IcuLibDir` for ARM points at the same vcpkg tree, so link and import agree
and ICU is *not* a blocker. `C:\icu-arm-uwp` does not exist and is not needed.

### Blocker found 2026-09-26: the SEH guard does not compile for ARM32

The first ARM32 driver-DLL build attempt fails in the **compile** step, not the link step:

```
WebCoreDriver.cpp(5872,5): error: SEH '__try' is not supported on this target
5 warnings and 15 errors generated.
```

`clang-cl` targets `thumbv7a-windows-msvc` for this device and does not implement `__try` there. Because
the whole TU fails, **no** driver objects are produced and the link then reports
`LNK2001: unresolved external symbol` for *every* export (`WebCoreClickAt`, `WebCoreCloseSession`, …),
which looks like a missing import library and is not. The other 12 port sources compile fine.

This is a clang limitation, not an OS one — MSVC's ARM32 backend does support `__try/__except`, and
clang does support SEH on AArch64. The wrappers in `Src/port/WebCoreDriver.cpp` now go through
`APO_SEH_RETURN_INT` / `APO_SEH_CALL_VOID`:

- **x64** expands to the original `__try/__except`. Behaviour is byte-identical to the build that
  fixed WER `0x1007`.
- **ARM32** forwards straight to the `*Impl` body and therefore has **no native guard**. A native fault
  there still reaches the CLR's `FailFast(7)`. `-DAPOTHEOSIS_REQUIRE_SEH` turns this into a build error.

A real guard on ARM32 requires compiling `WebCoreDriver.cpp` with MSVC for that target; it is not
achievable with a preprocessor switch. That is a decision worth taking deliberately, because ARM32 is
where a crash is most likely and where the managed host can least afford to die.

## Result 2026-09-26: the ARM32 driver DLL builds

The chain below took the ARM line from *no driver DLL at all* to a working one:

```
==> Compiled 14/14 objects
==> Linking WebCoreDriver-arm.dll
[link] EXIT=0  unresolved=0  duplicate=142
==> WebCoreDriver-arm.dll built successfully (31349.5 KB)
    Exports: 30 functions (via .def)
```

`Src/port/WebCoreDriver-arm.dll` = **30.6 MB**, 30 exported entry points, zero unresolved symbols. Its
imports are 22 DLLs and **`WebCore.dll` is not among them** — WebCore is linked statically, exactly as
on x64. So `build-arm32-gpu\bin\WebCore.dll` does **not** need to ship in the payload after all.

The six things that had to be fixed, in the order they surfaced:

1. **`__try` is not available for ARM32** — 15 compile errors, see the blocker section below. Fixed with
   `APO_SEH_RETURN_INT` / `APO_SEH_CALL_VOID`.
2. **`GetModuleHandleExW` undeclared** on the ARM32 include path — 1 error. Declared explicitly behind
   `#if !defined(_LIBLOADERAPI_H_)`.
3. **`LNK2005` duplicates** between `WTF.lib` and `JavaScriptCore.lib` (the latter is a real object
   archive on ARM, not an import lib, and already contains the WTF objects). Fixed with
   `/FORCE:MULTIPLE` for ARM only.
4. **WebCore internals unresolved** in `stubs-other.arm32.obj` — fixed by building and linking
   `WebCoreFull-arm32.lib` instead of the import library. This was the big one.
5. **GDI symbols unresolved** — vcpkg's ARM `cairo.lib` uses the win32 GDI backend, and UWP has no GDI.
   `gdi32.lib` does **not** help: linking the SDK `10.0.19041.0 um\arm\gdi32.lib` still leaves
   `__imp_SaveDC` / `GetGlyphOutlineW` / `GetFontData` / `GetTextMetricsW` unresolved. The fix is
   `Src/harness/gdi-stubs.cpp`, which the x64 line gets through the harness vcxproj and the standalone
   driver link has to compile in itself.
6. **`hb_icu_script_to_script` unresolved** from `LocaleICU.cpp.obj` — it lives in `harfbuzz-icu.lib`,
   not `harfbuzz.lib`.

`duplicate=142` is expected and benign: those are the WTF symbols defined in both archives, and
`/FORCE:MULTIPLE` keeps the first definition.

### Remaining runtime dependency: the UWP CRT

The driver's imports include `MSVCP140_APP.dll`, `MSVCP140_2_APP.dll` and `VCRUNTIME140_APP.dll`.
No ARM copies exist on this machine (the only `*_APP.dll` files anywhere are x64 debug symbols under
`Windows Kits\10\Debuggers\x64\sym`). On x64 this never surfaced, because there the CRT is named
`MSVCP140.dll` / `VCRUNTIME140.dll` — and on x64 `WebCore.dll` is ballast anyway, since the driver links
WebCore statically.

**Resolution, now confirmed on the artifact (2026-10-03).** `Apotheosis.csproj` carries
`<SDKReference Include="Microsoft.VCLibs, Version=14.0" />`, and the ARM bundle's manifest carries the
resulting `PackageDependency Name="Microsoft.VCLibs.140.00" MinVersion="14.0.33519.0"`. The three
`*_APP` DLLs are therefore not missing files but a **provisioned framework**: Windows installs them
arch-correctly when the dependency is satisfiable. The remaining condition is the *version* — a phone
whose `Microsoft.VCLibs.140.00` is older than `14.0.33519.0` will refuse the package. So the failure
mode to expect, if any, is a **deployment error naming VCLibs**, not a loader error inside our DLL.
Check the installed version over WDP before any engine theory; never substitute x64 DLLs.

Note the asymmetry that forced the issue: x64 links the **desktop** CRT and ARM links the **store**
CRT, and that is not a style choice. ARM's `cairo.lib` comes from vcpkg and was built against the store
CRT; dropping `lib\arm\store` from `$env:LIB` leaves every cairo object with an unresolved
`__security_cookie`. The CRT flavour is dictated by the third-party libraries, so each architecture has
to be re-checked on its own. See [BUILD-X64.md](BUILD-X64.md) §7.

### What is still missing for a device run

- `native\ARM` **is staged** — 22 files, 86 MB (the driver relinked 2026-10-03), audited against the
  ARM driver's imports plus the dynamically loaded `icudt78.dll`: nothing missing, nothing extra.
  `WebCore.dll` is correctly absent.
- ~~The appx must still be built with `/p:Platform=ARM`~~ — **built 2026-10-03**, Debug and Release;
  see the results block at the top.
- The three `*_APP` CRT DLLs come from the `Microsoft.VCLibs.140.00` framework dependency (above);
  the open part is only whether the phone's VCLibs meets `MinVersion 14.0.33519.0`.
- **Use Release|ARM on the device**, never Debug: Debug binds to `.Debug` framework packages a Lumia
  does not have.
- No on-device validation has happened; the phase exit criterion in [INDEX.md](INDEX.md) is a real
  Lumia 950, and x64 results remain necessary but not sufficient.

### ARM payload inventory

`llvm-readobj --coff-imports` on the **built driver** (not on `WebCore.dll`, which no longer ships)
names 22 imports. Where each one lives:

| DLL | Source |
|---|---|
| `JavaScriptCore.dll` | `build-arm32-gpu\bin` |
| `libEGL.dll`, `libGLESv2.dll` | `Src\angle\arm` |
| `icuuc78`, `icuin78`, `libcrypto-3-arm`, `libssl-3-arm`, `fontconfig-1`, `freetype`, `harfbuzz`, `harfbuzz-icu`, `jpeg62`, `libcurl`, `libpng16`, `libwebp`, `libwebpdemux`, `libxml2`, `pixman-1-0`, `z` | `C:\vcpkg\installed\arm-uwp\bin` |
| `MSVCP140_APP`, `MSVCP140_2_APP`, `VCRUNTIME140_APP` | **not files in the payload by design** — provisioned by the `Microsoft.VCLibs.140.00` framework dependency; see the UWP CRT note above |

`WebCore.dll` is deliberately absent from that list: the driver links WebCore statically. Staging
`build-arm32-gpu\bin\WebCore.dll` would only add 33 MB of dead weight to the package.

already built:

| Artifact | State |
|---|---|
| `build-arm32-gpu/lib/WebCore.lib` | **present**, 5.0 MB, 2026-08-23 |
| `build-arm32-gpu/lib/JavaScriptCore.lib` | **present**, 2.2 MB |
| `build-arm32-gpu/lib/WTF.lib` | **present**, 33 MB |
| `build-arm32-gpu/lib/PAL.lib` | **present**, 6.8 MB |
| `Src/angle/arm/{libEGL,libGLESv2}.lib` | **present** |
| `C:\vcpkg\installed\arm-uwp/lib` | **present** — including `fontconfig.lib`, which x64-uwp lacks |
| ARM port objects `Src/port/*.arm32.obj` | present but **stale** (2026-08-24) |

So `pwsh -File Src/port/build-driver-dll-arm32.ps1 -Arch arm` is minutes, not half a day. Reserve
the WebKit rebuild for an actual WebKit change.

## 1. What the driver-DLL build script had wrong (fixed 2026-09-26)

`Src/port/build-driver-dll-arm32.ps1` is the one command that builds the driver DLL for either
architecture. It could not build *either* one; four independent defects, all fixed:

1. **`$Root` was one level too deep.** `Split-Path -Parent $PSScriptRoot` yields `<repo>\Src`, so
   every `$Root\WebKit\...` and `$Root\Src\angle\...` resolved to a path that does not exist. The
   x64 driver DLL in the payload had therefore been produced by hand, not by this script.
2. **The ARM branch pointed at a tree this repo does not have** (`build-clang-gpu`). The real ARM
   tree is `build-arm32-gpu`.
3. **`$EnvScript` for x64 sourced the *ARM* environment**, which set `APOTHEOSIS_TARGET=arm32` and
   put the arm32 toolchain on `INCLUDE`/`LIB`/`PATH`. The script poisoned its own environment: the
   x64 compile step then aborted in `assert-target.ps1` and `link.exe` could not find
   `WindowsApp.lib`. Each architecture now gets its own env script.
4. **The library list was one list, written for `lib.exe`**, which never resolves externals — a name
   that does not exist is simply never pulled in, so `cairo.lib`, `fontconfig.lib`, `expat.lib`,
   `sqlite3.lib`, `zlib.lib` and `libjpeg-turbo.lib` sat in the x64 command for months unnoticed.
   `lld-link` *does* resolve, and failed on six of them. The lists are now per-architecture, with a
   fail-closed existence check that names the missing library.

The compile step also used to parse `-I` paths out of `harness-cmd.bat`, which is an ARM-only dump
of the *previous* tree layout — so the x64 run inherited ARM include paths and every source died on
`no such file or directory`. Both architectures now delegate to the compilers that are kept in step
with the real trees (`compile-driver-gpu-x64.ps1` reads `build-x64-gpu\build.ninja`,
`compile-driver-gpu-arm32.ps1` reads `build-arm32-gpu\build.ninja`).

## 2. Why the ARM driver DLL was deleted once, and what actually blocks it

`link-driver-gpu-arm32.ps1` documents that its `lld-link` step was **removed on purpose**: the DLL
link "could not resolve the protected RenderTheme virtuals (`minimumControlSize`,
`adjustButtonStyle`, …) referenced by the `StubRenderTheme` vtable in `stubs-other.cpp`".

That diagnosis is right, but two conclusions drawn from it were wrong:

- **The fix is import mode, not more exports.** `build-arm32-gpu\lib\WebCore.lib` is an *import*
  library (every archive member is literally named `WebCore.dll`, 8540 `__imp_` symbols) for an ARM
  `WebCore.dll` that already exists. In import mode those protected virtuals become `__imp_`
  references resolved at load time. So the ARM driver DLL links `WebCore.lib` and ships
  `WebCore.dll` next to it in `native\ARM\`, exactly as the ARM harness does at runtime.
- **The gap is not ARM-specific.** Linking against the x64 import library leaves **58 distinct
  symbols unresolved** — all `RenderTheme` protected virtuals, plus `RenderTheme::RenderTheme()`,
  `SystemFontDatabase::SystemFontDatabase()`, `Font::initCharWidths()` and friends.
  `llvm-readobj --coff-exports` on `WebCore.dll` finds **zero** hits for `baselinePosition`: the
  DLL does not export them.

x64 avoids this because its driver DLL links a **full static WebCore archive**
(`build-x64-19041-probe\lib\WebCoreFull-x64-19041.lib`, 3.2 GB) where those symbols are real code.
**There is no equivalent ARM archive on this machine** — `build-arm32-gpu\lib` holds only the four
archives listed in §0. That is the concrete remaining work for the ARM driver line:

> **Option A (cheap, recommended).** Link the ARM driver DLL in import mode against
> `build-arm32-gpu\lib\WebCore.lib` and ship ARM `WebCore.dll` + `JavaScriptCore.dll` in
> `native\ARM\`. If the unresolved list matches x64's 58 symbols, it is the expected, benign case
> and the import model is working as designed.

> **Option B (expensive).** Build an ARM full static WebCore archive the way
> `build-x64-19041-probe` did (archive every ARM WebCore `.obj`). This is the multi-hour job, and
> only becomes necessary if import mode proves unworkable on W10M.

## 3. The ARM payload is empty

`Src/Apotheosis\native\ARM\` **does not exist** (0 files), while `native\x64\` holds 33 staged DLLs.
ARM packaging is therefore not just "build the driver" — the third-party runtime DLLs must be staged
too. The dependencies the x64 line had to add late are all present in the ARM triplet
(`C:\vcpkg\installed\arm-uwp\bin`, 34 DLLs):

```
brotlicommon.dll  brotlidec.dll  brotlienc.dll  bz2.dll  libexpat.dll  libsharpyuv.dll  iconv-2.dll
```

plus ARM builds of `freetype`, `harfbuzz`, `harfbuzz-icu`, `jpeg`, `libpng16`, `libwebp`,
`libwebpdemux`, `libxml2`, `z`, `libcurl`, `libssl`, `libcrypto`, `pixman-1`, `libEGL`, `libGLESv2`
and the ICU set, plus `build-arm32-gpu\bin\WebCore.dll` and `JavaScriptCore.dll` in import mode.
The five fonts under `Assets/fonts` are architecture-independent and already staged for x64.

## 4. ARM-specific traps that are already known

- **Three toolchains, never mixed**: clang-cl + lld-link for the engine and `Src/port/*.cpp`; MSVC
  v143 (`bin\Hostx64\arm\cl.exe`) for the C++/CX harness. C++ exceptions are off (`_HAS_EXCEPTIONS=0`
  + `/EHs-c-`) — clang's thumbv7 backend cannot lower `cleanupret`.
- **ASCII path, no spaces** — the Ruby generators and meson break otherwise.
- **`__try/__except` compiles for ARM too.** The managed-host SEH guard is not an x64-only
  construct; it must be re-verified by building `-Arch arm`, since the filter is compiled for
  thumbv7 there.
- **fontconfig is linked on ARM but not on x64.** `arm-uwp` has `fontconfig.lib` and its header;
  `x64-uwp` has neither. WebCore itself is built `USE_CAIRO 1`, `USE_SKIA 0`, no `USE_FREETYPE` /
  `USE_FONTCONFIG` on **both** architectures, so the cairo backend does the font work — which is
  part of why x64 can render at all without a working fontconfig configuration.
- **JIT is disabled on both lines** (`useJIT=false`): executing JIT-generated code inside the W10M
  AppContainer kills the process with no exception and no dump.
- **Cairo ARM float clamp**: on ARM32, `Canvas::setImageInterpolation` could corrupt a surface after
  a float overflow (NEON saturation vs x87). Guarded under
  `#if defined(__ARM_PCS_VFP) || defined(__thumb__)`.

## 5. Moving a build tree breaks `.lib` archives that hold absolute member paths

**Apotheosis 2026-09-26.** `cairo-complete-x64-uwp.lib` was created with `lib.exe`, and its members
are recorded with **absolute paths**:

```
llvm-ar t build-x64-gpu/libs/cairo-complete-x64-uwp.lib
  → C:\...\build-x64-gpu\libs\cairo.x64.obj        (absolute!)
  → build-x64-gpu\libs\psl\psl.lib                 (relative — a different archive)
```

`lib.exe` stores the path it was given. So the moment that directory is **renamed or moved**, the
archive is broken in a way that is easy to misread: the file is present, `Test-Path` says `True`,
its hash is unchanged, `llvm-ar t` lists all 134 members — and the link still fails with

```
lld-link: error: could not open 'cairo-complete-x64-uwp.lib': no such file or directory
```

The message names the **member name as written in the command** (a bare `cairo-complete-x64-uwp.lib`),
not the path that is actually missing, so it reads as "the library is absent" when the library is
right there. Two hours went into this. Three things that would have caught it sooner:

1. **`llvm-ar t <archive>` prints the member paths — always read them** after moving a tree.
2. **Rebuild the archive with relative members** rather than copying it:
   `llvm-ar rc <new>.lib <objs...>` run *from inside* the directory, so members are plain filenames.
   That is what the current `build-x64-19041-probe/libs/cairo-complete-x64-uwp.lib` now contains.
3. **Prefer full paths for such libraries in the link line.** `$b\libs\cairo-complete-x64-uwp.lib`
   rather than the bare name. Bare names depend on `/LIBPATH` resolution, which hides the real
   problem behind a misleading error; the full path fails loudly and immediately instead.

The decisive test for a tree is to make it unusable and confirm the build still passes:

```powershell
Rename-Item build-x64-gpu\libs libs-HIDDEN-test   # make the dependency impossible
pwsh -File build-x64-19041-probe/run-driver-link.ps1
# DRIVER_LINK_EXIT=0  → the tree is genuinely self-sufficient
Rename-Item build-x64-gpu\libs-HIDDEN-test libs    # put it back
```

Do this **before** deleting anything. "The file is where I copied it" is not the same claim as
"the build no longer needs the old tree", and only the second one justifies a deletion.

## 6. Renaming a build tree breaks 4655 symlinks silently

**Apotheosis 2026-09-26 — the expensive one.** The WebKit build trees are full of **symlinks**: the
generated-header directories are populated with links back into the WebKit source tree, so the
compiler sees a tidy `JavaScriptCore/Headers/...` layout. `build-x64-gpu` carries **4655 of them**.

Renaming the tree breaks every link whose *target* contains the old directory name. The failure is
almost impossible to read, because nothing in the text files mentions it:

```
fatal error: 'JavaScriptCore/Bytecodes.h' file not found
```

…and every check that looks like it should settle it says the file is fine:

| Check | Result | Why it lies |
|---|---|---|
| `Test-Path <file>` | `True` | the link entry itself exists |
| `Get-ChildItem -Filter Bytecodes.h` | finds it | enumeration sees the link, not its target |
| `Get-Item <file>.Length` | `0` | a symlink has no length of its own |
| `Get-Content <file>` / `File.ReadAllText` | **`DirectoryNotFoundException`** | the real target is gone |
| `build.ninja`, `CMakeCache.txt`, all `.ps1` | clean | symlink targets live in NTFS, not in text |

Only two things actually diagnose it:

```powershell
Get-ChildItem <tree> -Recurse -Force |
  Where-Object LinkType -eq 'SymbolicLink' | Select-Object -First 5 FullName, Target

Get-Item <file> -Force | Select-Object LinkType, Target
```

The fix, after renaming:

```powershell
$stale = Get-ChildItem <tree> -Recurse -Force |
  Where-Object { $_.LinkType -eq 'SymbolicLink' -and ($_.Target -join '') -like '*old-name*' }
foreach ($l in $stale) {
    $t = ($l.Target -join '').Replace('old-name', 'new-name')
    Remove-Item $l.FullName -Force
    New-Item -ItemType SymbolicLink -Path $l.FullName -Target $t -Force
}
```

Verify with a real build, then count what is still broken — both should be reported, not eyeballed:

```powershell
(Get-ChildItem <tree> -Recurse -Force | Where-Object LinkType -eq 'SymbolicLink').Count   # 4655
# ... and of those, how many fail Test-Path
```

**The rule: renaming a build tree is not a rename.** It is a rename *plus* a text sweep *plus* a
symlink sweep. Do all three, and prove it with a clean compile of the affected target — in this case
`WebCoreDriver.cpp`, which pulls in the JSC headers and fails within seconds if any link is stale.
A green `ninja` run would not have caught it: the engine objects are already built and ninja would
have considered them up to date.

## 7. Watching a build: `Src/tools/build-monitor.ps1`

A `-j1` WebKit build on this machine is hours of silence. The monitor reads `.ninja_log` and reports
done/total plus a rate-derived ETA, and it never touches the build:

```powershell
# leave this running in its own window; Ctrl+C stops it
pwsh -File Src/tools/build-monitor.ps1 -Tree build-arm32-gpu

pwsh -File Src/tools/build-monitor.ps1 -Tree build-x64-gpu -IntervalSec 20
pwsh -File Src/tools/build-monitor.ps1 -Tree build-x64-gpu -Once      # one line, for a scripted check
```

```
[19:05:40]   1512 / 1548   files   97,7%  ETA   17m 34s  #############################.
```

Two things it gets right that the obvious implementation gets wrong, both found by running it:

- **Count `.obj` edges only.** This tree's `.ninja_log` holds **20 526** entries, but ~19 000 are
  Python generator steps (`generate-js-builtins.py` and friends) that finish in milliseconds.
  Counting log lines gives "20526 / 1548 files, 1323%".
- **Count distinct outputs, not log lines.** The log is append-only and survives every run, so line
  counts accumulate across rebuilds. Distinct output paths are the stable numerator.

The ETA comes from the **most recent 15 minutes**, not the whole run: early edges are small and
fast, the late ones are the big unified sources, so a whole-run average reports a finish time far
earlier than the real one. `IDLE` means no edge completed in the last two minutes — useful, because
a build can be stuck in one long TU and still look busy.

## 8. Deployment

Manual, over WDP, from the x64 build machine:

```powershell
pwsh -File Src/tools/deploy-launch.ps1 -Ip <device> -Ver 0.2.0.6 -Pub <publisher>
```

`Apotheosis.csproj` stages the ARM DLLs with `<Link>%(Filename)%(Extension)</Link>` so they land at
the **package root** — inside the AppContainer there is no `SetDllDirectory`, so a
`native\ARM\WebCoreDriver.dll` subdirectory makes `DllImport` fail. This is configured for both arches
and was verified against the built bundle on 2026-10-03: all 22 engine DLLs are at the root.

Build the package to deploy like this (from the repo root, `bash`):

```
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" \
    Src/Apotheosis/Apotheosis.csproj \
    -p:Configuration=Release -p:Platform=ARM -p:AppxBundle=Always
```

Use `-p:` (a dash), not `/p:` — Git Bash rewrites `/p:` into a path. Use `AppxBundle=Always`: a plain
`.appx` (`AppxBundle=Never`) is refused by `Add-AppxPackage` when updating an existing bundled install.
The artifact lands at `Src/Apotheosis/AppPackages/Apotheosis_<version>_Test/Apotheosis_<version>_arm.appxbundle`
(version follows `Package.appxmanifest`; the current one is `0.2.3.30`, the build that carries the
rebuilt `JavaScriptCore.dll`).

Run MSBuild and the ARM driver script under **`pwsh`** (PowerShell 7), not `powershell.exe` (5.1):
under 5.1 any native tool that writes to stderr (llvm-lib without `/MACHINE`, the ARM env script's own
`cl` banner) aborts the script through `$ErrorActionPreference = 'Stop'` at a line unrelated to the
real problem — and, before it aborts, skips the payload staging.

Push it with:

```
pwsh -File Src/tools/Wdp-Deploy.ps1 -Appx <the .appxbundle> -DeviceIp 192.168.1.35
```

**Audit the package before it goes anywhere near the phone:**

```
pwsh -File Src/tools/Test-AppxPayload.ps1 -Path <the .appxbundle> -ExpectedArch arm
```

It answers the four questions whose answers have each cost a debugging session: is every binary the
claimed architecture (an x64 DLL inside an ARM package fails only when the loader reaches it), are the
engine DLLs at the package root, is `resources.pri` there (its absence blanks the whole chrome), and
does the packaged driver export everything `WebCoreDriver.exports.def` declares (a missing symbol is an
`EntryPointNotFoundException` at *run* time, not a link error). It exits 1 on any failure, so it can be
chained into a deploy script; `-Keep` leaves the extracted package for inspection. Run against the
current bundle it reports `AUDIT PASSED` and **40/40** declared symbols exported (measured 2026-10-03
on `0.2.3.24` and again on `0.2.3.25`; the number follows `WebCoreDriver.exports.def`, which has declared 40 symbols since
`WebCoreReleaseMemory` was added — and the x64 payload DLL was one behind until it was rebuilt the
same night).

**Deploy Release, not Debug.** Debug's manifest depends on `Microsoft.NET.Native.Framework.Debug.1.7`
and `Microsoft.VCLibs.140.00.Debug`, which a retail Lumia does not carry; the install fails on the
unmet dependency before any of our code runs. Release depends only on the retail frameworks.


## The `.30` device outcome (2026-10-09)

Deployed (`HTTP 202 in 34s`, `Success`, `ME.Apotheosis_0.2.3.30_arm__cj4nrm0qt9s1y`) and launched by
hand. **The rebuilt `JavaScriptCore.dll` is on the phone**: the `.30` session in `port-trace.txt`
carries 1531 `rlife:` lines, all `tid=2548`, where the `.29` session carries none. **The new
`RedBlackTree` detectors never fired**: `TREE-BAD-*`, `TREE-ZERO-LINK` and `TREE-WALK-TRUNCATED` are
0 hits in the whole file, and the `sched`/`unsched` pairs stay balanced to the last line.

The process still aborted, ~20 s in, during a burst of image loads (ad banners and avatars). The
signature **changed**: `#2` is now `WebCoreDriver.dll+0xF7E5` (on `.29` it was
`JavaScriptCore.dll+0x1EA01B`), and there is **no argument triple**, so it is a plain `CRASH()`, not a
`RELEASE_ASSERT`. `#2` lands in an ICF-folded block of empty WebCore destructors (`~PolicyContainer`
at map RVA `0xE7E4`, one byte before the frame, with `WTFCrashWithInfo` at `0xE7CA`), so it is left
**unresolved rather than named**; what the frames support is that the abort came from driver-side
code, not from JavaScriptCore. Full analysis in `Doc/Archive/Summary.md`.

**Read the map VA column, not the RVA field**: for the driver tracer an offset `O` corresponds to map
RVA `O - 0x1000`, and the VA column equals `0x10000000 + O`. Keying on the RVA field lands in the
wrong ICF cluster and returns a plausible wrong symbol.

**The "blurry content" report, measured.** Device `settings.ini`: `zoom=50`, `fitwindow=0`,
`gpudefault=0`. With fit off the viewport is the fixed `720x1080` DIP, and `zoom=50` lays the page
out at 1440 CSS px into a 720-px buffer — one buffer pixel per two CSS pixels — which the compositor
then upscales to the panel. The port states the rule at `WebCoreSetPageZoom`
(`Src/port/WebCoreDriver.cpp:5947-5960`): software present "has a DIP-sized viewport and must stay at
zoom 1.0", while GPU present uses the panel's physical size and `zoom = CompositionScale`, "which is
where the sharpness comes from". On this device the engine has never composited:
`WebCoreEnableCompositing()` returns `g_session->chrome->rootLayer() != nullptr ? 1 : 0`, the harness
calls it on every load, and **`compositing=0` in all ten pulled `stage.txt` files** (2026-10-03 and
2026-10-09); `gpudefault=0` as well. **One-step test: set the zoom back to 100%.** The residual
DIP -> physical upscale is structural to the software path.

**Ran, and the number arrived later (2026-10-09 23:08).** The zoom was set to 100% by hand and the
maintainer reported "не идеально, но вроде бы повеселее" -- better, not fixed, which is exactly the
predicted split. The confirming measurement was missed at first because the pull asked for the port's
marker file under the **wrong name**: the app passes `gpu-steps.txt` (`MainPage.xaml.cs:219`) while the
C++ harness passes `gpuinit-steps.txt`, and only the harness name was in the default list -- so several
pulls logged a 404 for a file that existed. Pulled under the right name against the *running* app
(`gpuLogMarker` opens/appends/closes per line, so unlike `port-trace.txt` it is readable live), the file
is truncated once per process and its first zoom marker reads `[GPU] page zoom 1.00 -> 0.59 (contents
720x1654 css | viewport 720x1080 dip | w/zoom 1220 css)`. `before = 1.00` on the session's first zoom
call is the missing number: at `zoom=100` the `_defaultZoom != 100` guard at `MainPage.xaml.cs:1166`
skips the commit and `teardownSession` leaves 1.0, so the session truly ran at one buffer pixel per CSS
pixel. Two pinches in that session both applied (1.00->0.59, 0.59->0.80).

**Before arming the GPU, two things the measurements say.** (1) `rc=0` is not the same as a picture:
`WebCoreGpuInit` returns 0 as soon as the GL context and `TextureMapper` exist
(`WebCoreDriver.cpp:6089-6113`), while the gate that matters is the first frame. In the August harness
pulls on this same device `WebCoreGpuInit returned 0` appears in **every** one, yet
`EnableCompositing=1` appears **once** (2026-08-21 18:09:49, `presenting through the GPU surface`); all
the others read `EnableCompositing=0 Composite=-12 -- staying on software present`. The August harness
checked that pair and stayed on software; the current app (`MainPage.xaml.cs:2938`) treats `rcCopy == 0`
alone as success and sets `_gpuPresent = true`. (2) The app never calls `WebCoreSetDirectPresent` --
`WebCoreDriver.h:153-163` documents that without it the driver takes the direct-present branch regardless
and presents into a surface nobody sees, "a fully loaded page in a white window". Both are reasons to
expect the GPU switch to need a verification step of our own, not just a colour change on a button.
Also note the crash-loop guard: `EnableGpu` writes `gpu-crash.flag` before `WebCoreGpuInit` and the next
launch reads it to set `_gpuDefault = false` (`MainPage.xaml.cs:519-525`), so `gpudefault=0` in the
current `settings.ini` is the shape of a previous GPU attempt that died during init.

Not measured this run: the trace rotation (3.29 MB < the 4 MB ceiling, so `.1`/`.2` were absent).

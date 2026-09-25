# The `pump()` teardown abort: a `CheckedPtr` that outlived its object

Status: **root-caused and fixed 2026-09-19** — `RedBlackTree::remove()` left the node it unlinked
holding its own `CheckedPtr` children (§8), so a removed task kept a counter alive on a live node and
the node that died next died with a link on it. Fixed by detaching the node at removal (§9); the
sections below are the history of how it was narrowed, kept because §2, §3, §5 and §6 are the reusable
half. Read §7 first if you are here for the answer: §4's `~RunLoop` hypothesis is falsified.
Originally measured 2026-09-18 on the x64 bench (`build-x64-gpu`, appx `0.1.10.10`, GPU off).

This is the death that made every tap-reproduction run end in `CRASHVERDICT: CRASHED` a few hundred
milliseconds *after* the page it was investigating. It is not the calc AV (`CALC-HANDLE-DANGLING.md`);
it is a second, independent defect that fires later in the same session and had been hiding the first
one's diagnostics.

## 1. The symptom

`port-trace.txt` ends, every time, at the same three lines — the tail of `pump()` in
`Src/port/WebCoreDriver.cpp`:

```
rlsleep: woke #292 tid=5568 mode=drain planned=-213.4ms slept=0.0ms sched=4 pend=1 stop=1
pump: RunLoop::run() returned
pump: settle.stop enter
pump: settle.stop done
pump: watchdog.stop done
```

and then, in `log.txt` (engine thread `tid=5568`):

```
2026-09-18 23:22:53.912  SIGABRT: tid=5568 -- abort() reached the CRT signal handler
2026-09-18 23:22:53.912  SIGABRT: stack (12 frames):
2026-09-18 23:22:53.912    ABRT #0 00007FF764192D18 [Harness.exe+b2d18]
2026-09-18 23:22:53.912    ABRT #1 00007FFE89FF143C
2026-09-18 23:22:53.912    ABRT #2 00007FFE89FD1551
2026-09-18 23:22:53.912    ABRT #3 00007FFE2A9C7FDA [JavaScriptCore.dll+307fda]
2026-09-18 23:22:53.912    ABRT #4 00007FFE2B545274 [JavaScriptCore.dll+e85274]
2026-09-18 23:22:53.912    ABRT #5 00007FF7640E8FB1 [Harness.exe+8fb1]
2026-09-18 23:22:53.912    ABRT #6 00007FF7640E6E2B [Harness.exe+6e2b]
2026-09-18 23:22:53.912    ABRT #7 00007FF76419BA5B [Harness.exe+bba5b]
2026-09-18 23:22:53.912    ABRT #8 00007FF7641ED2CD [Harness.exe+10d2cd]
2026-09-18 23:22:53.912    ABRT #9 00007FF764175EC9 [Harness.exe+95ec9]
```

`crashverdict.txt` is not refreshed by this death (the monitor reads `heartbeat.txt`, which stops at
23:22:53 with `job=nav-load`), and no `unhandled.txt` appears: it is an `abort()` from the engine
thread, caught by the port's own `DEATHNET` handler, not an unhandled C++/CX exception.

## 2. Reading the stack, and the two ways to read it wrongly

Symbolisation is offline: `llvm-pdbutil dump -l <PDB>` gives `0001:XXXXXXXX-XXXXXXXX` line records
that are **section-relative**, so `offset = RVA - 0x1000` (`.text` starts at RVA 0x1000). The
convention was re-verified on 2026-09-18 against an anchor that does not depend on any of this
reasoning: frame #0 is `Harness.exe+b2d18`, and `RVA 0xB2D18 - 0x1000 = 0xB1D18` lands in
`MainPage.xaml.cpp`, bracketed by line **257** (21 bytes before) and line **258** (4 bytes after) —
which is exactly `frames = CaptureStackBackTrace(...)` / `LogWriteF("SIGABRT: stack ...")` in
`MainPage.xaml.cpp:253-265`, the signal handler that produced the report.

Subtracting a second 0x1000 is not a small error and it does not look like one. It produced:

| true | wrong-convention answer | plausible? |
|---|---|---|
| `CheckedRef.h:343` (frame #3) | `InspectorProtocolObjects.cpp:1361` | yes — a real file, a real line |
| `MainPage.xaml.cpp:257` (frame #0) | `MainPage.xaml.cpp:4413` | yes — the same file |
| `RunLoopGeneric.cpp:505` (frame #4) | `RunLoopGeneric.cpp:423` | yes — the same file, 80 lines off |
| `MainPage.xaml.cpp:2686` (frame #7) | `MainPage.xaml.cpp:5667` | yes |

Every one of those was believed at least once. The rule from `diagnostics-that-guess-will-lie` applies
to the symboliser as much as to the module-attribution code it came from: **calibrate the convention
against a frame whose meaning is known independently, before reading any frame whose meaning is not.**
`llvm-symbolizer` cannot do this job at all here — it answers `??:0:0` for PE addresses on these
binaries, and `llvm-nm` returns zero symbols for `Harness.exe` (no COFF symbol table).

With the convention pinned, the mapped stack is:

| # | module | address | symbol |
|---|---|---|---|
| 0 | Harness.exe | b2d18 | `MainPage.xaml.cpp:257` — the `DEATHNET` SIGABRT handler |
| 1,2 | UCRT | | `abort()` internals |
| 3 | **JavaScriptCore.dll** | 307fda | **`wtf/CheckedRef.h:343` — `crashDueToCheckedPtrToDeadObject()`** |
| 4 | **JavaScriptCore.dll** | e85274 | **`wtf/generic/RunLoopGeneric.cpp:505` — `RunLoop::TimerBase::~TimerBase()`** |
| 5 | Harness.exe | 8fb1 | `WebCoreDriver.cpp:634` — the closing brace of `pump()`, where its two local `RunLoop::Timer` objects are destroyed |
| 6 | Harness.exe | 6e2b | `WebCoreDriver.cpp` ~2972-2989 — the caller of `pump()` on the engine thread |
| 7 | Harness.exe | bba5b | `MainPage.xaml.cpp:2686` |
| 8 | Harness.exe | 10d2cd | `MainPage.xaml.cpp:1560` |
| 9 | Harness.exe | 95ec9 | `MainPage.xaml.cpp:1363` |

Frames 3 and 4 live in **JavaScriptCore.dll**, not in the port and not in WebCore: WTF is compiled
once into an object library and linked into both `JavaScriptCore.dll` and `WebCore.dll`, so
`~TimerBase` resolves to the copy the linker picked first (JSC's) even though the `RunLoop::Timer`
objects being destroyed are `pump()`'s stack locals, whose vtables are emitted in Harness.exe. The
frame after the `~Timer` thunk is therefore JSC's `~TimerBase`, and the frame after *that* is
`pump()` — a chain that is only surprising until the two copies are accounted for.

## 3. What `crashDueToCheckedPtrToDeadObject()` means

`CanMakeCheckedPtrBase::decrementCheckedPtrCount()` (`CheckedRef.h:315-323`) is reached from exactly
three places: `~CheckedPtr`, `~CheckedRef`, and their assignment/`clear()` operators. It reads:

```cpp
if (!checkedPtrCountWithoutThreadCheck()) [[unlikely]]
    crashDueToCheckedPtrToDeadObject();      // -> CRASH()
--m_checkedPtrCount;
```

The guard is **not** an underflow check and the counter is `std::atomic<uint32_t>` — it never wraps,
because the guard fires first. A zero here means one specific thing, named by the comment two lines
above it and by `WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR` (`FastMalloc.h:627-637`):

```cpp
void operator delete(T* object, std::destroying_delete_t, size_t size) {
    object->setDidBeginCheckedPtrDeletion();
    object->T::~T();
    if (object->checkedPtrCountWithoutThreadCheck()) [[unlikely]] {
        secureZeroBytes(*object);   // scribble the whole object ...
        return;                     // ... and LEAK it
    }
    T::operator delete(object);
}
```

So the sequence that aborts is: **an object was deleted while a `CheckedPtr` to it still existed** →
the object was zeroed and leaked by that override → later, the surviving `CheckedPtr` was destroyed →
it decremented a permanently zero counter → crash. The name of the crash function is literal: a
`CheckedPtr` to a dead object, caught by the machinery that exists to catch exactly that.

Two consequences worth stating because they invert the natural guesses:

- It is **not** "the RunLoop is gone" and it is **not** an unbalanced refcount. A live object's
  counter cannot be zero while its `CheckedPtr`s exist; only a scribbled one reads zero forever.
- The object is **leaked, not freed**, so the address is stable and the zero is permanent — which is
  why this dies deterministically once it has happened, rather than intermittently.

## 4. What `~TimerBase` contains, and why the suspect is the schedule tree

`RunLoop::TimerBase` (`RunLoop.h:152-200`) holds `const Ref<RunLoop> m_runLoop`, an `ASCIILiteral`,
and (generic event loop) `const Ref<ScheduledTask> m_scheduledTask`. **Neither is a `CheckedPtr`** —
so the decrement cannot come from its own members. What the function does contain, in the same
inlined frame:

1. `ApoLoopLocker locker { m_runLoop->m_loopLock, "~TimerBase" }` — `Ref::operator->`, a raw pointer,
   no counter;
2. `stopWithLock()` → `unscheduleWithLock(task)` and `m_scheduledTask->deactivate()`. `deactivate()`
   only stores a bool, and `unscheduleWithLock` is guarded by `if (task.isScheduled())`, which was
   already false for both of `pump()`'s timers — their `stop()` calls ran first and printed
   `settle.stop done` / `watchdog.stop done`. **So the schedule tree is not even touched on this
   path**;
3. the epilogue: `m_scheduledTask`'s `Ref` (possibly the task's last), then `m_runLoop`'s `Ref` —
   **possibly the RunLoop's last**, since `pump()` builds both timers from temporaries
   (`RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, ...)`) and nothing else on the
   engine thread need hold one. If the loop's count reaches zero here, `~RunLoop()` is inlined into
   this frame, and with it the destruction of `RunLoop::m_schedules`.

That last object is where every `CheckedPtr` in this story lives. `RedBlackTree` (`RedBlackTree.h`)
holds `CheckedPtr<NodeType> m_root` (line 629) and every node holds `CheckedPtr<NodeType> m_left` and
`m_right` (lines 157-158) — the whole tree is `CheckedPtr`, and `RunLoop::TimerBase::ScheduledTask`,
its node type, is the class that carries the delete override (`RunLoopGeneric.cpp:179`:
`WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ScheduledTask)`). A `ScheduledTask` that was ever deleted while
the tree still pointed at it would be scribbled and leaked, and the tree's own `CheckedPtr` — the
`m_root` member, or a sibling's `m_left`/`m_right` — would be the pointer that aborts on the way down.

That hypothesis is consistent with every measurement so far, and it is still a hypothesis: it does not
say *which* earlier timer's teardown left the tree holding a scribbled node, and the scribbled node
itself is unreachable by construction (its address is only in the tree that is being destroyed).

> **Falsified 2026-09-19 — see §7.** The tree was empty at *every* loop teardown (2125 of 2125
> `loop-destroy` lines, `extra=0`; zero `loop-destroy-node` lines), so nothing was destroyed with the
> tree. The container argument above survives and is what §8 builds on; the timing was wrong.
> The answer is §8: `RedBlackTree::remove()` itself leaves the unlinked node's links intact.

## 5. The instrumentation now in place

All of it lives in `WebKit/Source/WTF/wtf/generic/RunLoopGeneric.cpp`, inside the existing
`#if defined(WK_WINUWP)` block, and writes `rlife:` lines to `port-trace.txt` through the same
flushed-per-line sink the other `rl*` markers use. One translation unit: a compile and two links.

| marker | written from | fields |
|---|---|---|
| `rlife: loop-create` | `RunLoop::RunLoop()` | `a` = the loop |
| `rlife: loop-destroy-enter` | `RunLoop::~RunLoop()` | `a` = the loop, `extra` = `m_mainLoops.size()` |
| `rlife: loop-destroy-node` | `~RunLoop()`, while walking `m_schedules` | `a` = the node, `count` = **the node's checked-ptr count**, `extra` = `isActive()` |
| `rlife: loop-destroy` | end of `~RunLoop()` | `a` = the loop, `extra` = how many nodes the tree still held |
| `rlife: timer-create` | `TimerBase` ctor | `a` = the timer, `b` = its `RunLoop`, `count` = the task's counter |
| `rlife: timer-destroy-enter` / `-locked` / `-body-done` | `~TimerBase()` | `a` = the timer, `b` = its `RunLoop` (enter only), `count` = the task's counter, `extra` = `isScheduled()` (enter only) |
| `rlife: sched` / `unsched` | `scheduleWithLock` / `unscheduleWithLock` | `a` = the task, `b` = the loop, `count` = the task's counter after the operation, `extra` = `m_schedules.size()` |

How to read the chain: the entry to `~TimerBase` names the timer and its loop; `-body-done` proves the
body completed, so whatever dies next dies in the epilogue; `unsched` lines name every task that left
the tree. The decisive line is `loop-destroy-node` — **a node printed with `count=0` while it is still
in the tree is already-scribbled memory, and the crash that follows is the tree's `CheckedPtr` going
down with it.** If instead the chain ends at `timer-destroy-body-done` with no `loop-destroy-enter`,
the `Ref<RunLoop>` is not the last and the loop dies later, elsewhere.

`RunLoop`'s own counter is deliberately **not** logged: `RunLoop` inherits checked-ptr support through
`AbstractCanMakeCheckedPtr` (the virtual kind, which is what a class with a virtual destructor has to
use, since `WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR` needs the most-derived type), and its accessors are
not public — `checkedPtrCount()` on a `RunLoop` does not compile (`error: no member named
'checkedPtrCount' in 'WTF::RunLoop'`). `ScheduledTask`'s is a plain `CanMakeThreadSafeCheckedPtr` and
is readable.

## 6. Two rules this probe had to obey

- **`RedBlackTree::first()` and `successor()` return raw `NodeType*`** (`RedBlackTree.h:373`, `:92`),
  and the walk above depends on that. Had they returned `CheckedPtr`, the walking local would have
  *incremented* the very counter under measurement — reporting 1 for a scribbled node, and, worse,
  masking the crash the probe exists to catch, because the tree's own `CheckedPtr` would then have
  found a non-zero counter. **A probe that holds a `CheckedPtr` to the object it is measuring changes
  the measurement.** (Compare the rule already in `ARM32-DANGLING-SECURITYORIGIN.md`: a probe that
  faults or alters what it measures is worse than no probe.)
- **Read counters through a `Ref`, never through a `CheckedPtr`.** Every `count` above is read through
  `m_scheduledTask` (a `Ref<ScheduledTask>`) or a raw pointer, both of which leave the counter alone.

## 7. The `~RunLoop` hypothesis is dead (measured 2026-09-19)

§4's guess — a looping `Ref<RunLoop>` reaching zero inside `~TimerBase`'s epilogue, taking the schedule
tree down with it — was the most economical reading of the stack, and it is wrong. Re-measured on the
preserved 0.1.10.28 trace (`LocalState\prelaunch-0919-0832\port-trace.txt`, the run that aborted at
08:06:50):

| marker | count | reading |
|---|---|---|
| `loop-destroy-enter` | 2391 | loops that entered destruction |
| `loop-destroy` | 2125 | 266 never finished — the trace ends at the abort |
| `loop-destroy` with `extra=0` | **2125 of 2125** | every loop that died died with an EMPTY schedule tree |
| `loop-destroy-node` | **0** | the walk inside `~RunLoop` never found a node to print |

No loop was destroyed holding a node, scribbled or otherwise. §4 stands on the tree being the container
of every `CheckedPtr` in the story; what does **not** stand is the timing — the tree is already wrong
while its loop is alive, and teardown only reports it.

## 8. The root cause: `RedBlackTree::remove()` leaves the removed node linked

The invariant `CheckedPtr` is built on is: a node is in the tree ⟺ the tree holds exactly one link to it
(its parent's `m_left`/`m_right`, or `m_root`) ⟺ its counter can only be non-zero while it is in the
tree. `RedBlackTree::remove(z)` (`RedBlackTree.h:228`) breaks the second half. It rewires the **tree**
correctly in both branches and never touches **z itself** — and z's `m_left`/`m_right` are `CheckedPtr`s,
so z walks out of the tree still incrementing the counter of every child it used to have:

- `y != z` (z has two children): `y->setLeft(z->left()); y->setRight(z->right());` (lines 270-271) copy
  the links onto the successor and leave z's own copies behind — **two stale links**;
- `y == z` (z has fewer than two children): the single child `x` is moved up (or `m_root` reassigned)
  while `z->m_left`/`m_right` still name it — **one stale link**.

The consequences compound, and all of them appeared in one run (0.1.10.29, x64 bench, `port-trace.txt`):

```
rlife: sched a=A ... count=1 extra=4          <- tree of 4 nodes, walk clean
rlife: unsched a=B ... count=0 extra=3        <- B leaves the tree; B itself is clean (count 0)
rlife: TREE-BAD-AFTER-UNSCHED count=4 extra=3 <- the walk sees sum(4) over 3 nodes: B's stale link
rlife: TREE-BAD-REM-COUNT a=A ... count=1     <- A is removed and STILL POINTED AT -- by B
rlife: TREE-BAD-INS-COUNT a=A ... count=1     <- A is re-scheduled while still pointed at
rlife: sched a=A ... count=2 extra=3          <- insert()'s reset() cleared A's own outgoing links,
                                                 but B's stale link survives, so the tree now holds TWO
                                                 links to A: a node with two parents, i.e. a DAG
```

(`count` is the node's `checkedPtrCount()`; `extra` is, per marker, either `isScheduled()` or
`m_schedules.size()` — §5. The `TREE-BAD-*` lines are §9's probe.)

The endgame is the abort itself: a `ScheduledTask` whose last `Ref` is dropped while B's stale link still
names it is deleted with a non-zero counter → `FastMalloc.h`'s deleting-delete `secureZeroBytes()`es it
and **leaks** it (§3) → the next decrement of that link is `crashDueToCheckedPtrToDeadObject()`. The
`timer-destroy-enter ... count=1 extra=0` measured in 0.1.10.28 is that exact moment: the task was
already unscheduled, so `stopWithLock()` skipped the unlink and the destructor's epilogue dropped the
last `Ref` with a link outstanding.

Two properties explain why this hid for weeks:

- It is **silent until the victim dies.** A stale link costs one counter increment, and nothing on the
  happy path reads a counter.
- **`insert()` heals the node it re-links, never the tree.** `insert()` opens with `x->reset()`
  (line 176), so remove-then-reinsert loses the stale links — which is why `MetaAllocator`, which does
  exactly that (`MetaAllocator.cpp:217-273`: `remove()`, … `insert()`), never crashed. The RunLoop's
  `unscheduleWithLock` usually does *not* re-insert, so the stale link lives on until its target dies.

## 9. The fix, and the probe that proved it

The fix is `z->reset()` at the end of `remove()` (`RedBlackTree.h`, after the structural surgery and
after `removeFixup`, guarded `#if defined(WK_WINUWP)` with an `Apotheosis:` comment). `reset()` — both
children null, parent null, red — is precisely the detach, and it is the same call `insert()` opens with.
It is private, but `remove()` is a member, so no accessor was needed. It makes remove-then-reinsert
*more* correct, not less: the stale links are now cleared at removal instead of at the next insertion.

The probe that named it lives in `RunLoopGeneric.cpp` and obeys §6 — it uses only public API (`first()`,
`successor()`, `size()`, `checkedPtrCount()`), because `left()`/`right()`/`parent()` are private and
adding accessors would mean editing the header (§10):

| marker | site | fields |
|---|---|---|
| `TREE-BAD-AFTER-SCHED` / `-UNSCHED` | after `insert()` / `remove()` | `a` = an over-counted node and `extra` = 3 (the structural maximum) when one node is too hot; otherwise `a` = null, `count` = the SUM of the counts, `extra` = the tree size, and any difference is the number of links held from outside the tree |
| `TREE-ZERO-LINK` | same walk | `a` = a node in the tree whose count is 0 — its own tree link is missing |
| `TREE-BAD-REM-COUNT` | `unscheduleWithLock`, right after `remove()` | `a` = the task, `count` = its count: must be 0, since nothing may name a node the tree has just unlinked |
| `TREE-BAD-INS-COUNT` | `scheduleWithLock`, right before `insert()` | `a` = the task, `count` = its count: must be 0, or `insert()`'s `reset()` + link makes the second parent |
| `task-destroy` | `~ScheduledTask` | `a` = the task, `b` = its timer, `count` = its count, `extra` = `isScheduled()` — the poison alarm, at the only moment the object is still readable |

The walk's arithmetic is worth stating because it is what makes `sum != size` a proof rather than a
heuristic: in a well-formed binary tree of n nodes the structure holds exactly n `CheckedPtr`s (`m_root`,
plus every non-null `m_left`/`m_right`), so the counts of the n walked nodes must sum to exactly n and no
node can carry more than three links (parent + two children; the root carries `m_root` instead of a
parent). A link held by a node the tree no longer owns is *outside* the walk but still increments its
target — so the sum exceeds n by exactly the number of stale links.

## 10. Compiling a WTF header change without rebuilding the engine

`WebKit/Source/WTF/wtf/*.h` is no ordinary header here. It is copied into `build-x64-gpu/WTF/Headers/wtf/`
by an unconditional `cmake -E copy` under `restat = 1`, and `RunLoop.h` includes `RedBlackTree.h`, so
`RedBlackTree.h` reaches nearly everything. Measured 2026-09-19: touching it turned a 2-task build into
**736 tasks** — every WTF translation unit, all of WebCore's unified sources, the LLInt and both DLL
links. At `-j1` that is a day, for a three-line fix.

It does not have to be. Template **member functions are instantiated only where they are odr-used**, and
`RedBlackTree::remove()` is called from exactly three places:

| caller | tree | translation unit |
|---|---|---|
| `RunLoop::unscheduleWithLock` | `m_schedules` | `Source/WTF/wtf/generic/RunLoopGeneric.cpp` |
| `MetaAllocator::findAndRemoveFreeSpace` and friends | `m_freeSpaceSizeMap`, `m_allocations` | `Source/WTF/wtf/MetaAllocator.cpp` |
| `ExecutableAllocator::freeIslands` | `m_islandsForJumpSourceLocation` | `Source/JavaScriptCore/jit/ExecutableAllocator.cpp` (inside `UnifiedSource-3a3c4ec0-2.cpp`) |

The other ~730 units include the header but emit no code for `remove()`, so a fix compiled into those
three is a complete fix. The recipe, which keeps ninja's view of the world consistent:

1. edit the source header;
2. `cp` it over `build-x64-gpu/WTF/Headers/wtf/RedBlackTree.h` (the copy is what dependents include —
   WTF's own TUs include the source tree instead, verified from the compile commands);
3. pin **both** files to the mtime ninja recorded for the copy in the previous full build:
   `touch -d "2026-08-14 12:04:40.131588000 +0300" <src> <dst>`. With the source not newer than the copy,
   the copy edge stays clean; with the copy's mtime unchanged, the `restat` dependents stay clean;
4. delete the three `.obj` files and `ninja -C build-x64-gpu -j1 WebCore` — 3 compiles, 2 links.

The next full build (ARM32, or any later edit to a WTF header) picks the change up everywhere for free,
so the state is consistent, not divergent.

**A way to destroy a 20 KB upstream file in one command, and how it was recovered.** Step 3 was first
attempted as `touch -r "$DST" /tmp/ref; cp "$SRC" "$DST"; cp /tmp/ref "$SRC"` — but `cp` copies
*content*, and `touch -r` had just created `/tmp/ref` as a zero-byte file, so the second `cp` wrote
**empty** over the source header (and the first over the build copy: both were 0 bytes). `WebKit/` is a
**git work tree** (`git -C WebKit rev-parse --is-inside-work-tree` → true), so `git -C WebKit checkout --
Source/WTF/wtf/RedBlackTree.h` restored the pristine 20165 bytes and the edit was re-applied. Use
`touch -r FILE FILE2` on the real files, never a `cp` to move a timestamp; and remember that this tree
has a recovery path — the fork's own files under `Src/` do not.

## 11. Verification, and which module carries the probe

The pair of builds differs in exactly one thing, and that is what makes the measurement worth anything:

| build | probe in the binary | the fix in the binary | result |
|---|---|---|---|
| `0.1.10.29` | yes | no | **330** `TREE-BAD-*` lines, SIGABRT on an in-page tap |
| `0.1.10.30` | yes | yes | **0** `TREE-*` lines, 1269 `sched` / 1264 `unsched`, 23 `task-destroy` all `count=0 extra=0`, no SIGABRT |

Both binaries carry the probe, so the zero is a measurement and not a silence: `TREE-BAD-AFTER-UNSCHED`
is found by `grep -a` in the `JavaScriptCore.dll` of both appx files, and the two have different hashes
(`925fba29…` pre-fix, `c7f446b3…` post-fix).

**The probe is in `JavaScriptCore.dll`, not `WebCore.dll`** — which cost a false negative here. Five
`grep -c` checks against `build-x64-gpu/bin/WebCore.dll` returned 0 for every marker, including
`task-destroy`, which the trace plainly contains 23 times, and that read was briefly taken for "the probe
is not compiled in". It was the wrong file. WTF's object files are linked into several images and the
copy that lands in JSC is the one that runs; this is the same split as
`which-module-carries-which-marker` in project memory (the `WTFCrash` tracer is in JSC too). To check
whether a marker exists in a build, search **every** payload of the appx, and prefer the appx itself over
`build-*/bin`, because that is what the installed package contains:

```powershell
for f in build-x64-gpu/bin/*.dll; do "$f $(grep -a -c TREE-BAD-AFTER-UNSCHED $f)"; done
```

Two readings still open, neither a defect in the fix: 5 more `sched` than `unsched` (tasks scheduled and
still in the tree when the trace ended — check against the pre-fix run before calling it a leak), and the
0.1.10.29 run ended **wedged** rather than crashed (`beat-stuck … job=live-tick`, last log line 08:37:23),
which is a separate observation and was not reproduced on 0.1.10.30.




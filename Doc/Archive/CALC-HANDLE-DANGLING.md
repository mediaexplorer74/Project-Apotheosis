# The calc-handle dangling `Value&` crash

**Status 2026-09-18: root cause named, contained, and the containment extended to the second
entry point.** The crash is a *containment* failure, not a new bug: it was diagnosed on 2026-09-17
and guarded in `LengthWrapperData::nonNanCalculatedValue(float, const ZoomFactor&)` — but the same
defect has **two** entry points, and the guard was written into only one of them. The bench kept
dying through `nonNanCalculatedValue(float, const ZoomNeeded&)`, which had no guard at all.

Read this before touching `StyleLengthWrapperData.{h,cpp}`, `StyleCalculationValueMap.h`, or
`wtf/StdLibExtras.h`'s `visitOneVariant`.

## 1. The symptom

The x64 bench dies on the **engine thread** with a deterministic access violation, `0xC0000005`,
always a **read** (`op=0`), and always the same instruction:

```
VEH: code=0xC0000005 addr=00007FFE2879910F tid=1720 n=2 op=0 faultaddr=000000000000000F
```

`%LOCALAPPDATA%\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState\log.txt`, written by
`ApoFirstChance` (`Src\harness\MainPage.xaml.cpp`). The record's fields:

| Field | Meaning |
|---|---|
| `addr` | `ExceptionAddress` — the **faulting instruction**, i.e. `WebCore.dll` + an RVA |
| `op` | `ExceptionInformation[0]`: 0 read, 1 write, 8 execute |
| `faultaddr` | `ExceptionInformation[1]`: the faulting **data** address |
| `tid` | must match the engine thread id (`WEDGE`/`WE- job:` lines carry it) |

The same crash has now been observed at **three different RVAs** — `0x204901f` (recorded in the
2026-09-17 comment), `0x204903f` (the 16:46 and 19:30 logs) and `0x204910f` (the current build).
They are **one site**: every rebuild shifts `.text`, so an RVA is only meaningful together with the
build that produced it. See §3.

## 2. What the instruction is

Disassembled from the current `build-x64-gpu\bin\WebCore.dll`:

```
1820490c0:  pushq %r15                       # entry of the body that faults
...
18204910f:  movzbl 0x8(%rdx), %eax           # <- FAULT: reads v.index() at v+8, %rdx is 7 or 8
182049113:  cmpq $0x1e, %rax                 #   0x1e = 30, so indices 0..30 are in range
182049117:  ja 0x18204964f                   #   out of range -> the WTF_UNREACHABLE() tail
18204911d:  leaq 0x7952cc(%rip), %r8         #   jump table
182049124:  movslq (%r8,%rax,4), %rax
182049128:  addq %r8, %rax
18204912b:  jmpq *%rax
```

- `%rdx` is the **second argument** — the `V&& v` of `WTF::visitOneVariant(F&& f, V&& v)`.
- The read is `v.index()`, and WebKit's `Variant` keeps the index at **offset 8** of the object
  (the 8-byte storage union first), which is why the faulting data address is `%rdx + 8`.
- `%rdx` was **7** in one run and **8** in the others — small integers, i.e. *not addresses*.
  Both `0xF` and `0x10` therefore appear as `faultaddr`, and that is the whole story: the crash
  address is not a wild pointer into a page, it is a **reference that was never an address**.
  Whether the value 7/8 is a coincidence of two samples, or literally this Variant's *alternative
  index* leaking into the pointer field, is **not established** — two samples is not a mechanism.
  What matters for the fix is the same either way: `visitOneVariant` was handed a reference built
  out of memory it does not own.
- `cmpq $0x1e` says this instantiation's `Maximum` is **31**, i.e. a 31-alternative variant. The
  only such variant on this path is `WebCore::Style::Calculation::Node`
  (`style/calc/StyleCalculationTree.h:132` — 3 leaf alternatives + 28 `IndirectNode<>` arms).

The function is the fork's `visitOneVariant` in `WTF/wtf/StdLibExtras.h` (lines 543-560): a flat
chain of `WTF_INDEX_VISIT_IF(0..31)`, **no `WK_WINUWP` guard**, with the `ALWAYS_INLINE` that
upstream had removed, so it is a real out-of-line function. Note that this rewrite covers indices
`0..31` only, while upstream's macro recursed in chunks of 32 — a separate latent issue, *not* this
crash (`Maximum` here is 31, so the chain is complete for this instantiation).

## 3. Symbolising a crash from these logs (offline, no debugger)

`llvm-symbolizer` and `cdb` **both fail on this PDB** (`??:0:0`), so the recipe is built out of two
dumps that do work. It is in two passes because they are two different tables, and mixing them up
produces a confident wrong answer (§3.3).

### 3.1 Function, from `.pdata`

```
llvm-readobj --unwind build-x64-gpu\bin\WebCore.dll > wc-unwind.txt
```

Search the RVA inside the `RuntimeFunction` entries, which are **linker-absolute RVAs** (no
ImageBase, no `.text` bias): `0x204910F` falls inside the entry starting at `0x20490C0` (size 4784).
That is one function, and the instruction is `+0x4F` into it.

### 3.2 File and line, from the line table

```
llvm-pdbutil dump -l build-x64-gpu\bin\WebCore.dll > wc-lines.txt     # ~24 MB
```

Then `%TEMP%\apo-line-multi.ps1` (range-aware, reads its offset list from `%TEMP%\wc-offsets.txt`)
prints every containing range with the nearest preceding line entry. Three gotchas, learned the hard
way:

- **Line-table ranges are section-relative.** Subtract the `.text` RVA (`0x1000`) from the RVA
  before searching, or every hit lands one section off — which reads as a plausible neighbouring
  function, not as an error. With this offset wrong, `0x204810F` was reported as `StdLibExtras.h`
  (right file, wrong line) and another build's `0x204803F` as `Vector.h:1881`.
- **Offsets go in a file, not on the command line.** `pwsh -File script.ps1 -Offs 204810F,2048686`
  does **not** split on the comma; `[IO.File]::ReadAllLines` is the fix.
- **PowerShell indexer + nested call.** `$q[[Convert]::ToInt64($o,16)] = @()` fails with a bogus
  *"Additional non-parsable characters are at the end of the string"*, because the comma inside the
  nested invocation parses as an index-argument separator. Parse into a plain variable first.

### 3.3 The trap that cost the most time: two builds in one log directory

`log.txt` is appended across installs, and `LocalState\prelaunch-*` holds older logs. Every WebCore
frame in the aggregated `VEH #` lines appeared **twice**, and the pairs differed by exactly **0xD0**
in `.text`. That is not a broken unwinder — it is two builds. Current build: fault `+0x204910F`
(`log.txt`, `crash-report.txt`). Previous build: fault `+0x204903F` (`prelaunch-0918-1649`,
`prelaunch-0918-1942`). The RVA in the 2026-09-17 source comment, `0x204901F`, is a third, even
older build's value for the same instruction. **Never mix frames from more than one build, and
always symbolise against the DLL you are actually running.**

### 3.4 Why `visitOneVariant` appears several times in one stack

Frames #5 and #6 are the same function. The disassembly explains it: `visitOneVariant` contains
`callq 0x1820490c0` at RVAs `0x2049681`, `0x20496A6` and `0x20496DF` (return addresses `...686`,
`...6AB`, `...6E4`), each loading `leaq 0x38(%rsp), %rcx` (the visitor object) and taking the
variant pointer from `(%rbx)+0x18` / `+0x28`. The visitor lambdas are **inlined into**
`visitOneVariant`, so the nested `switchOn` calls are call sites *inside its own `.pdata` range*.
Several PCs in one range is expected here, not evidence of a bad unwind. The stack itself is real:
`CaptureStackBackTrace(0, 40, bt, nullptr)` (`MainPage.xaml.cpp`).

## 4. The engine-thread stack

Frames #4 → #39, symbolised against the current build (line numbers are that build's):

```
visitOneVariant                            wtf/StdLibExtras.h:548 (551 for the nested calls)
Style::Calculation::Value::evaluate        style/calc/StyleCalculationValue.cpp
LengthWrapperData::nonNanCalculatedValue   style/values/primitives/StyleLengthWrapperData.cpp:113
StyleTranslateTransformFunction.cpp:92  -> StyleTransformList.cpp:40
StyleTransformResolver.cpp:151 / :189
RenderBox.cpp:708                       -> RenderLayer.cpp:1709
RenderLayerBacking.cpp:732 / 607 / 305  -> RenderLayer.cpp:5794
RenderLayerCompositor.cpp:2400 / 2369 / 2197
RenderLayer.cpp:6231 -> RenderLayerModelObject.cpp:150 -> RenderBox.cpp:342
RenderBlock.cpp:342 -> RenderBlockFlow.cpp:2428 -> RenderElement.cpp:574
RenderTreeUpdater.cpp:538 / 453 / 372 / 344 / 250 / 132
RenderTreeUpdaterGeneratedContent.cpp:197
dom/Document.cpp:2849 / 2866 / 3234 / 5476
dom/Element.cpp:1908
```

Two things this stack says that the earlier diagnosis could not:

1. **It is the compositing path, not layout.** `StyleTransformResolver` →
   `RenderLayerCompositor::updateCompositingLayers`. The 2026-09-17 comment names
   `RenderLayer::updateTransformFromStyle` and `KeyframeEffect::computeExtentOfTransformAnimation`
   as the two callers it saw; this crash arrives through the transform resolver instead.
2. **It is `StyleTranslateTransformFunction.cpp:92`** — a `translate` transform function — so the
   stale value is a *transform* length, and the tree under evaluation is a `calc()` on it.

`Value::evaluate` is where the dangling `Value&` becomes a fault: it calls
`Calculation::evaluate(m_tree, …)`, which walks the tree with `switchOn(Calculation::Node)` and
lands in `visitOneVariant`.

## 5. Mechanism

`LengthWrapperData` holds an **unsigned handle**, `m_calculationValueHandle`, into the
process-wide `Style::Calculation::ValueMap`; the real `Calculation::Value` lives in that map
(`style/calc/StyleCalculationValueMap.h`). The handle is never reused
(`m_nextAvailableHandle++`), so a handle can only go wrong by being deref'd one time too many —
at which point `deref()` hits `referenceCountMinusOne == 0` and **removes the entry**:

```cpp
inline Value& ValueMap::get(unsigned handle) const
{
    ASSERT(m_map.contains(handle));                       // <- compiled OUT in release
    return *m_map.find(handle)->value.value;              // find() == end() -> *end()
}
```

Release builds have no asserts, so a stale handle does not fail here — it **dereferences `end()`**,
i.e. reads a `RefPtr<Value>` out of the hash table's own storage, and the process continues with a
"`Calculation::Value&`" that is not one. Evaluating it walks `m_tree`, which is garbage, which
produces a `Calculation::Node` reference that is not an address, and `visitOneVariant` faults on
`v.index()`. That is the whole chain, and it is why the fault is a **read of 0xF/0x10** rather than
an access to a plausible-looking bad pointer.

The consequence for triage: **do not look for a layout bug.** The fault site is three layers of
abstraction away from the defect, and the fault address is meaningless except as proof that the
reference was never an address.

## 6. The fix, and why there was a second crash

The 2026-09-17 containment was written into **one of the two overloads**:

```cpp
float nonNanCalculatedValue(float maxValue, const ZoomFactor& usedZoom) const;   // guarded 09-17
float nonNanCalculatedValue(float maxValue, const ZoomNeeded&)               const; // NOT guarded
```

The crash arrives through the second (`StyleLengthWrapperData.cpp:113`). Two copies of one check,
written months apart, drifted immediately — so the containment is now **one function**,
`LengthWrapperData::hasLiveCalculationValue()` (`Calculation::ValueMap::calculationValues().contains`),
called by both overloads and by `isCalculatedEqual()`, which reached `calculationValue()` unguarded
too and would have faulted on the same stale handle during a style comparison.

Both paths return **0** on a stale handle, the same shape as a NaN result: the frame renders with a
wrong length for one frame instead of taking the process down. `isCalculatedEqual` answers
**false** — a comparison must never claim an equality it cannot see; a wrong *false* costs extra
style work, a wrong *true* costs a stale pixel.

The guards are `#if defined(WK_WINUWP)`, which is set in **both** toolchains
(`Toolchain-x64-UWP-clang.cmake:30`, `Toolchain-ARM32-UWP-clang.cmake:35`), so ARM32 gets the same
containment. The crash itself was measured on x64 only.

The containment is a **file-local function** in the `.cpp`, not a member, and that is a build-cost
decision, not a style one — see §6.2.

### 6.1 The diagnostic in the guard is invisible — and the old comment said it was not

`apoStaleCalculationValueZero()` calls `WTFLogAlways`. On this port that goes to
`vprintf_stderr_common` → **`OutputDebugStringA`** (`WTF/wtf/Assertions.cpp:203`) — **not a file**.
The 2026-09-17 comment claimed "the event log records every hit"; that is wrong. `grep "[APO calc]"`
over every log returns nothing, and **that absence proves nothing** — it must not be read as "the
guard never fired". Under a debugger is currently the only way to see it. If this guard needs to be
observable on the phone, it needs the established file channel (`apoCrashTrace`'s
`FONTCONFIG_FILE` → `port-trace.txt`, or `gpuLogMarker` → `gpuinit-steps.txt`).

### 6.2 Why the guard is a file-local function: editing one widely-included header costs a full WebCore rebuild

Measured 2026-09-18, and worth more than the fix it came from. The first version of this containment
declared `bool hasLiveCalculationValue() const` as a **member** in `StyleLengthWrapperData.h`.
`ninja -C build-x64-gpu WebCore -j1` then reported **362 dirty tasks** — 361 unified-source compiles
plus the link — i.e. essentially all of WebCore, ~10 hours at `-j1`. `ninja -n -d explain` named the
input exactly:

```
output .../UnifiedSource-3a52ce78-16.cpp.obj older than most recent input
       WebCore/PrivateHeaders/WebCore/StyleLengthWrapperData.h (8092828444279261 vs 8114618159822286)
```

348 of the 362 sources reach that header transitively (it is a style *primitive* header), which is
the whole story: the header is on the dependency edge of half the engine, and a declaration-only edit
changes no layout and no ABI for any of them.

**The trap is how the dirty set is counted, and it is not the source header's mtime.**
`build-x64-gpu\WebCore\PrivateHeaders\WebCore\StyleLengthWrapperData.h` is a **symbolic link** to the
source file, and ninja on Windows stats the **link itself**, not its target:

```
Get-Item build-x64-gpu\WebCore\PrivateHeaders\WebCore\StyleLengthWrapperData.h
  → LinkType: SymbolicLink, Target: …\WebKit\Source\WebCore\…\StyleLengthWrapperData.h
```

So `touch` on the source (which follows the link) changes nothing that ninja can see. What ninja
compares is the link's own mtime, and the "Generating …/StyleLengthWrapperData.h" step recreates that
link with a fresh mtime every time the source changes. The undo is therefore two separate touches:

```powershell
# source header: back to the bytes+time the existing objects were built against
touch -d "2026-08-22 00:00:02" WebKit/Source/WebCore/style/values/primitives/StyleLengthWrapperData.h
# the symlink ITSELF -- -h, or touch follows the link and this line does nothing
touch -h -d "2026-08-22 00:00:03" build-x64-gpu/WebCore/PrivateHeaders/WebCore/StyleLengthWrapperData.h
```

with the link's time **one second after** the target's, so the copy rule's own
`output >= input` check stays satisfied and does not recreate the link. Result, verified:
**362 pending → 2** (one compile, one link). Ninja's model is mtimes only — it cannot know that a
reverted file is byte-identical to what the objects were built from, so the revert has to be told to
it. This is the same family as the `touch -r` trick CLAUDE.md records for `build-arm32-gpu`'s
`build.ninja`.

**Consequence for the next agent:** a `WK_WINUWP` patch to a widely-included *header* costs the full
WebCore rebuild; the same containment written into the `.cpp` costs one translation unit. Prefer the
`.cpp` when the two are equivalent — here they are, because all three call sites are members and can
read the private handle and pass it to a file-local check.

## 7. Reproducing

The crash was deterministic, 3 runs out of 3, on the engine thread, from a plain page load plus a
gesture on the dzen.ru article — no debugger attached. `%TEMP%\apo-repro-tapcrash.ps1` drives it
end to end; the manual form is the standard cycle with GPU armed:

```powershell
pwsh -File Src\tools\x64-cycle.ps1 -Url https://dzen.ru -Gpu
```

and then read `LocalState\log.txt` for a `VEH:` record whose `addr` is `WebCore.dll`-relative and
whose `tid` is the engine thread. Verification after the fix is the *absence* of that line plus a
page that still paints (the `diag:` line's `nonwhite=` pixel count is the check).

## 8. Still open

> **Superseded for this crash by §9 (2026-09-18 22:46).** The guard at these call sites holds: the
> run that followed the fix faulted *past* it, which proves the handle was live and the tree, not
> the handle, was corrupt. The questions below stay open for the *other* crash — the 09-17 one this
> guard was written for — but they are not what kills the bench now.

- **Who overderefs the handle.** The containment stops the crash; it does not explain the stale
  handle. The candidates are a `LengthWrapperData` copy/move path (see `initialize()` in the header:
  the copy ctor refs, the move ctor exchanges the handle to 0) or a `Value`/`ValueMap` teardown
  ordering issue at style-recalc. Nothing has been measured yet, and the guard being invisible
  (§6.1) is exactly what makes it hard: the first step is a *file* sink for the hit, then the URL
  and the element that produced the transform.
- **Whether 0 is the right answer** for a stale *transform* length. It keeps the frame alive, but a
  wrong geometry could in principle be worse than a missing repaint. Accepted for now: crashing is
  worse.

## 9. The guard held and the crash stayed: the payload is what is wrong

The fixed build (`0.1.10.8`) was linked, packaged, installed and driven through the same repro. The
engine thread faulted **at the same instruction**, one second after the article finished loading.
Full VEH record, from `LocalState\log.txt`:

```
2026-09-18 22:46:08.226  VEH: code=0xC0000005 addr=00007FFE26F1910F tid=3048 n=3 op=0 faultaddr=0000000000000010
2026-09-18 22:46:08.226  VEH: stack (40 frames):
                           #4 00007FFE26F1910F [WebCore.dll+204910f]   <- the fault
                           #5 00007FFE26F19686 [WebCore.dll+2049686]
                           #6 00007FFE26F196E4 [WebCore.dll+20496e4]
                           #7 00007FFE26F11AA7 [WebCore.dll+2041aa7]
```

`WebCore.dll` base `00007FFE24ED0000` (logged at startup). Symbolised against the *new* build — the
line dump was regenerated first, because symbolising against the old one is the §3.3 trap:

| RVA (minus `0x1000` for the line table) | File / line | |
|---|---|---|
| `0x204910F` | `wtf/StdLibExtras.h:548` | `visitOneVariant`, `v.index()` — the same instruction as before |
| `0x2048686` | `wtf/StdLibExtras.h:551` | `visitOneVariant` again (a different row of the `if` chain) |
| `0x20486E4` | `wtf/StdLibExtras.h:551` | `visitOneVariant` again |
| `0x2040AA7` | `StyleCalculationValue.cpp:70` | `Value::evaluate(_, const ZoomNeeded&)`, the `evaluate` call |
| `0x20CE4DB` | `StyleLengthWrapperData.cpp:151` | `nonNanCalculatedValue(ZoomNeeded)`, the `evaluate` call |
| `0x2110DE6` | `StyleTranslateTransformFunction.cpp:92` | |
| `0x210A5D6` | `StyleTransformList.cpp:40` | |
| `0x20188D1` | `StyleTransformResolver.cpp:151` | then `RenderBox` → `RenderLayer` → `RenderLayerCompositor` |

Three frames inside the same 4784-byte range as the fault is expected, not a symboliser artefact:
`visitOneVariant` is out-of-line (the fork's rewrite dropped `ALWAYS_INLINE`) while the tiny
`evaluate(const Child&)` is inlined into the visitor lambda, so a two-level descent reads as
`visitOneVariant → visitOneVariant → visitOneVariant` with the return addresses attributed to the
rows of the `if` chain the calls sit in.

**The guard provably ran and passed.** Frame `#8` is line **151**, which is the statement *after*
the check at line 148 — so control reached the check. And a passing check is strong:
`ValueMap::contains()` (`StyleCalculationValueMap.h:51`) is verbatim the predicate `get()` asserts
(`m_map.contains(handle)`, line 90) before it dereferences `find(handle)->value.value`. The entry
exists, the entry's `RefPtr<Value>` is non-null, and `protectedCalculationValue()` returns a
`Ref<Value>` that pins the object for the whole call. A stale handle cannot be the explanation at
this call site, and the Value cannot be freed under the walk.

**What the AV address says.** `movzbl 0x8(%rdx)` reads the `WTF::Variant` index byte, which sits 8
bytes past the variant's payload. `faultaddr = 0x10` ⇒ `%rdx = 8`; the *previous* crash's
`faultaddr = 0xF` ⇒ `%rdx = 7`. The faulting reference is a `Child` **at address 7 or 8**. A `Child`
only ever comes from three places, and each implies the same thing about its parent:

- `Children::operator[]` → `Vector<Child>::data()[i]`,
- a `Child a;`/`Child min;` member at offset 0 of an `Op`,
- `Tree::root` (always a valid address, so not this one).

For the payload-carrying cases the reference is computed as `op_pointer + offset`, so a Child at
address 7/8 means the parent's `UniqueRef<Op>` held **7 or 8** — and 7 and 8 are not pointers, they
are the *numbers in the `calc()` expression*, i.e. a leaf `Number`/`Dimension` double read as a node
pointer. That is a Child whose variant **index** names an indirect node while its **payload** is a
leaf: `switchOn` dispatches to the `IndirectNode<Op>` overload, `root->a` reads at `payload + 0` (no
fault), and the index byte of that "Child" is then read at `payload + 8` — the AV. It also explains
why the two crashes differ by exactly one: different style values carry different numbers.

### 9.1 The containment, in the one place every `Child` must pass

`StyleCalculationTree+Evaluation.cpp` — `evaluate(const Child&)` is the funnel: `ChildOrNone` and
`std::optional<Child>` both land here, and an `IndirectNode<Op>&` can only be obtained from a
`Child`, so validating the payload here validates the whole tree exactly one node ahead of the
dereference that faults. `apoCalcChildPayloadIsSane()` switches on the Child, and for the
alternatives that own an `op` it requires the pointer to be non-null, above the null page, and
8-aligned; otherwise the walk returns NaN, which `Value::evaluate` already turns into 0.

One `.cpp`, two ninja tasks (`UnifiedSource-26ec8d00-7.cpp` + the link) — deliberately *not* the
header, per §6.2.

A hit appends to `port-trace.txt` (the `getenv("FONTCONFIG_FILE")` → dirname channel of
`apoCrashTrace`, `WTF/wtf/Assertions.cpp:383`), rate-limited to the first 16 hits and then every
500th:

```
calcbogus: hit=1 index=7 payload=0000000000000008 -- index names a node, payload is not a pointer;
           evaluating it would read the index byte at payload+8
```

**The log line is the experiment.** `payload=0x8` (or `0x7`) confirms the leaf-double-read-as-node
story and makes the missing half of the investigation a hunt for *which* code builds a `Child` with
a mismatched index and payload. A large, plausible `payload` would instead mean a use-after-free of
the node, and an empty file with the crash still present would mean the corruption is created
*during* the walk — neither is likely, but the file distinguishes them without another guess.

### 9.2 What is still open

- **Who builds a Child whose index and payload disagree.** Candidates, none measured yet: the
  conversion path that builds the tree (`StyleCalculationTree+Conversion.cpp`), `Calculation::copy`
  (the only deep-copy path, `StyleCalculationTree+Copy.cpp`), and the fork's own `visitOneVariant`
  rewrite — which is the one component here that upstream does not have. The fork's flat `if
  (v.index() == N) return f(std::get<N>(…))` chain ends in `WTF_UNREACHABLE()`, so an index outside
  `[0, Maximum)` is undefined behaviour by construction rather than a caught error.
- **Whether NaN is the right answer.** Same argument as §8: a wrong transform beats a dead engine,
  and it is visible in the log.

## 10. The contrast experiment: the payload is a length's bytes, not a leaf double

§9.1's containment was built, installed and re-run against the same deterministic repro (x64,
`-Gpu`, tap on a `dzen.ru` article link). Its prediction was a `calcbogus:` line with a tiny payload
(`7`/`8`). Result, verbatim:

```
2026-09-18 23:11:33.755  TapDone: rc=0 changed=1 navEmpty=1 linkHit=1 refused=0 branch=nav-link
2026-09-18 23:11:38.981  VEH: code=0xC0000005 addr=00007FFE2AC4E553 tid=10248 n=2 op=0 faultaddr=0000000040800008
grep -c calcbogus port-trace.txt   ->  0
```

`WebCore.dll` base that session was `00007FFE28C00000`, so the faulting RVA is `0x204E553` (the
previous crash's was `0x204910F`) and the AV address moved from `0x10` to `0x40800008`. Symbolised
against the *current* PDB (line dump regenerated at 23:13 for the 23:02 PDB — §3.3 applies to every
relink, and the DLL had been relinked at 23:01:40):

| frame | RVA | symbol |
|---|---|---|
| #4 | `+204E553` | `wtf/StdLibExtras.h:551` — the inlined body of the fork's flat `visitOneVariant` |
| #5 | `+204E626` | same, `:551` |
| #6 | `+2041ADF` | `style/calc/StyleCalculationValue.cpp:70` — `Value::evaluate` |
| #7 | `+20D39DB` | `style/values/primitives/StyleLengthWrapperData.cpp:151` — `nonNanCalculatedValue` |
| #8 | `+21162E6` | `StyleTranslateTransformFunction.cpp:92` |
| #9 | `+210FAD6` | `StyleTransformList.cpp:40` |
| #10 | `+20188D1` | `StyleTransformResolver.cpp:151` |
| #11 | `+2018E98` | `StyleTransformResolver.cpp:189` |

Same chain, one `visitOneVariant` frame fewer: the fault is still the *index* read at `(child)+8`,
but now `child = 0x40800000` — a value the §9.1 check **accepted**, because `0x40800000` is above
`0x10000` and 8-aligned. So the guard let the walk descend one level and the fabricated *address*
killed it there. Three things follow, and the first is the one worth keeping:

- **The payload is not a leaf double either.** `0x40800000` is the bit pattern of the *float* `4.0`
  in the low dword with a zero high dword — the shape of the 8-byte `LengthWrapperData`
  (`float m_floatValue; uint8_t m_opaqueType; LengthWrapperDataKind m_kind; bool m_hasQuirk;`), not
  the shape of a `double` (which would have made the AV address `0x4010000000000008`, i.e. a 4 GiB
  high dword, not a zero one). A `Child` reference whose payload looks like a **length's storage** is
  a different failure from a leaf read as a node: the walk is being handed bytes out of a
  `Style::Length`, not a number out of the `calc()` source. `7`/`8` from the 22:46 runs are consistent
  with the same shape (`m_calculationValueHandle`, the `unsigned` twin of that union, holding 7 or 8).
- **A sanity window that garbage can pass is not a containment.** The only values the old floor
  rejected were ones *smaller* than the null page; the corruption it was written for is larger.
- **A check whose sink stays silent is indistinguishable from a check that never ran** — the loader
  tracer's lesson (`Doc/DEFERRED-SCRIPTS.md`, `portLoaderTrace`'s `PT: sink ok` line), applied here:
  the v2 check writes one liveness line per process, so `port-trace.txt` says whether it ran at all.

### 10.1 Containment v2: test the address, not the payload's magnitude

Same single funnel (`evaluate(const Child&)`), same single `.cpp` (two ninja tasks), two tiers now:

1. **The child's own address** against the null page (`< 0x10000`) — never dereferenced when it
   fails, because the fabricated reference is exactly what arrives here in the 23:11 crash.
2. **The payload** against a floor: `0x100000000` (4 GiB) on `CPU(ADDRESS64)`, `0x10000` on 32-bit,
   plus the 8-byte alignment test.

The 4 GiB floor is a measurement, not an assumption: every private-heap pointer this app logs is far
above it (the loader trace prints `loader: +0000020F3102B930`), and a calc node is always a
fastMalloc'd object — never on the stack, never inside a module image — while every bogus value seen
so far is far below (`7`, `8`, `0x40800000`). 32-bit is the honest weak case: the heap shares the
whole user address space with everything else, so only the null page is rejectable by arithmetic
alone, and the check is there a containment rather than the instrument.

Both tiers write the same line shape into `port-trace.txt` (first 16 hits, then every 500th):

```
calcbogus: hit=1 what=node-payload child=… index=12 payload=0000000040800000
calcok: the containment is live; first indirect child=… index=3 payload=0000020F…
```

`index=` is the datum the next round needs — it names *which* alternative the corrupted node claims
to be — and `calcok:` is the control that makes a silent `calcbogus:` mean something.

**Known false-positive risk:** if a live node ever sits below the 4 GiB floor, the check returns NaN
where a number was wanted. The effect is a wrong style value (`Value::evaluate` turns NaN into 0), not
a crash, and the `calcok`/`calcbogus` pair makes it visible rather than silent.

### 10.2 What is still open

- **Who builds a `Child` whose payload is a length's bytes.** Now the leading question, with
  `index=` from the log as the entry point. Candidates unchanged from §9.2, plus one new one: the
  tree is built *deterministically* on this page (the crash reproduces on every tap), which argues
  against a race and for a build/clone path — `StyleCalculationTree+Conversion.cpp` or
  `Calculation::copy`.
- **Whether a stale handle can name a live-but-wrong `Value`.** `protectedCalculationValue()` returns
  a `Ref<Value>`, so the `Value` is pinned for the call; the *handle* is not. A recycled handle would
  hand back a valid `Value` with the wrong tree, which is not a crash — but it would put a tree meant
  for a different `Length` under this one.

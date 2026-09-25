# `~BitmapTexture` teardown AV — 2026-09-18, RVA `0x418C2`

**Status: identified to the instruction and the member; not reproduced since. Its reproduction session
ran with a GL context the harness had already terminated — read `Doc/GPU-LIVENESS.md` first, because that
is the leading explanation and it changes what this AV is evidence *of*.**

## 1. The record

`LocalState\log.txt`, 2026-09-18 23:59:05, `0.1.10.12`, x64 bench, GPU armed:

```
VEH: code=0xC0000005 addr=00007FFE284E18C2 tid=5280 n=4 op=0 faultaddr=FFFFFFFF00000008
UEF: unhandled exception code=0xC0000005 at addr=00007FFE284E18C2 tid=5280
UEF: AV reading address=0xFFFFFFFF00000008
UEF: RIP=00007FFE284E18C2 RSP=000000800AFFD440 RBP=00000000FFFFFF1F
```

`0xC0000005` is the read AV, and the fault PC is `0x00007FFE284E18C2`. **The RVA is `0x418C2`**, and it
is established by two tools rather than by one subtraction: `llvm-objdump -d` puts
`??1BitmapTexture@WebCore@@QEAA@XZ` at image offset `0x417C0` in `build-x64-gpu\bin\WebCore.dll`, and
`llvm-symbolizer --obj=… --relative-address` resolves `0x418C2` into that function's inline chain. The MD5
of the *installed* DLL equals the one built here (`0B2DE11A908E5421283E50B138BCA9BF`), so the disassembly
below and the log describe the same binary — without that anchor the whole exercise would be about a
different build.

> **The arithmetic, because an earlier writeup of this record did it wrong.** `0x00007FFE284E18C2 −
> 0x00007FFE264A0000 = 0x20418C2`, not `0x418C2` — the module base quoted in that writeup does not
> produce the RVA that the symboliser and objdump independently agree on, so the quoted base belongs to
> another module or another session. A base of `0x00007FFE284A0000` is what makes the subtraction exact.
> CLAUDE.md's rule ("check the arithmetic — an offset larger than the image cannot be inside the image")
> was written for a diagnostic that printed something wrong and was believed; this is the same failure in
> its other form, a correct conclusion resting on a number that does not support it. The conclusion
> survives only because two tools were asked separately.

## 2. The function: `WebCore::BitmapTexture::~BitmapTexture`

`llvm-symbolizer --obj=build-x64-gpu\bin\WebCore.dll --relative-address` on `0x418C2` gives the function
plus its inline chain, innermost first:

```
WTF::ThreadSafeRefCounted<WebCore::FilterOperation, 0>::deref()   ThreadSafeRefCounted.h:99
WTF::DefaultRefDerefTraits<const WebCore::FilterOperation>::derefIfNotNull()   Ref.h:64
WTF::RefPtr<const WebCore::FilterOperation>::~RefPtr()            RefPtr.h:66
WebCore::BitmapTexture::~BitmapTexture()                          BitmapTexture.cpp:583
```

The disassembly around the fault (`??1BitmapTexture@WebCore@@QEAA@XZ`, RVA `0x417C0`-`0x418E5`):

```
180041816: movq 0x4e0(%rsi), %rcx     ; load the member
18004181d: movq $0x0, 0x4e0(%rsi)     ; null it
18004182d: movl 0x8(%rcx), %eax       ; read its refcount
180041830: lock ; 180041831: decl 0x8(%rcx)
180041834: je 0x1800418ba              ; last reference -> destroy
18004183a..1800418b2: three Vector frees + epilogue
1800418ba: movl $0x1, %eax ; 1800418bf: xchgl %eax, 0x8(%rcx)
1800418c2: movq (%rcx), %rax          ; <-- faulting PC: 48 8b 01
1800418c5: movl $0x1, %edx ; 1800418ca: callq *(%rax)   ; virtual deleting dtor
```

So: a `RefPtr` member, whose refcount has just reached zero, holds a pointer that is not an object — and
the virtual-destructor dispatch through its vtable faults.

## 3. Which member: `m_filterOperation` at `this+0x4E0`

Two independent proofs, because one signature match is how a wrong member gets named:

1. **Code shape.** The sequence is `RefPtr::operator->`/`deref` — load the pointer, null the slot, read
   the refcount at `+8`, `lock decl`, and on zero `xchgl` a sentinel back and call the virtual dtor
   through the vtable. Only a refcounted member compiles to that, and only the one the inline chain
   names.
2. **Layout arithmetic.** `BitmapTexture`'s own fields match the declaration order at `0x14/0x18/0x1c/0x20`
   (`m_id`, `m_fbo`, `m_depthBufferObject`, `m_stencilBufferObject`). Then `ClipStack m_clipStack`
   (`Vector<State>` buffer `0x28`/capacity `0x30`), `State clipState` (24 B), `IntSize size`,
   `bool clipStateDirty`, `YAxisMode yAxisMode`, `Vector<float, 120> m_roundedRectComponents` (buffer
   `0x60`, inline `0x70`), `Vector<float, 160> m_roundedRectInverseTransformComponents` (buffer `0x250`,
   inline `0x260`, 160 floats = `0x280`) — which ends exactly at `0x4E0`. The next member,
   `m_pixelFormat`, is at `0x4E8`, confirmed by an unrelated function's `cmpb $0x0, 0x4e8(%rcx)`.

Black-bordered with the header: `RefPtr<const FilterOperation> m_filterOperation` is declared between
`ClipStack m_clipStack` and `PixelFormat m_pixelFormat`, and there is no fork-added member in this class.
`TextureMapper.cpp:1277` is the only `setFilterOperation` call site (`applySinglePassFilter`).

**A discrepancy left open, deliberately.** The logged fault address is `rcx+8`
(`0xFFFFFFFF00000008`), while the PC at `0x418C2` says `movq (%rcx)` — a plain read at `rcx`. Eight
bytes cannot be waved away, so the honest reading is narrower than the log's: the member's value is
`0xFFFFFFFF00000000`-shaped garbage and the *refcount* access at `+8` (two instructions earlier) is what
the exception record most plausibly describes. Either way the conclusion is the same — the object is not
an object — but the +8 must not be quoted as if it were the faulting instruction's operand.

## 4. What this AV is evidence of — and what it is not

The immediate context is the dead display (see `Doc/GPU-LIVENESS.md`). In that session every GL operation
had been failing since the harness's EGL probe terminated the engine's display: 228/228 `rb: enter`
attempts read `rb: makeCurrent=0` + `eglMakeCurrent FAILED err=0x3001`. In the one run where a paint did
succeed (`prelaunch-0918-2053`), the trace shows the order immediately after it:

```
rb: readPixels done
tail: paintToRGBA exit rc=0 nonWhite=200299
glctx: eglMakeCurrent FAILED err=0x3001
rl: timer BitmapTexturePool::ReleaseUnusedTexturesTimer enter
```

`BitmapTexturePool::ReleaseUnusedTexturesTimer` destroys textures the pool believes are unused, and this
AV is a `BitmapTexture` destructor reading a garbage member. **The live hypothesis is therefore: textures
created or resized while the display was uninitialized left the pool's bookkeeping inconsistent, and the
release timer then destroyed one twice (or destroyed one whose memory had been reused).** A freed-and-
reused heap block whose "member" reads `0xFFFFFFFF00000000` is what that looks like.

This is a **hypothesis, not a finding**, and the project has paid for such guesses before. What supports
it: the timing (release timer immediately after the last successful paint, in the only run with a
measurable before/after), and the member value's shape. What contradicts nothing but does not confirm it
either: the AV has **not recurred** in the first session with a live GL context (`0.1.10.13`, 178
TextureMapper paints, 3438 `bindAsSurface`, zero `0xC0000005`) — one session without an AV is not a fix.

## 5. If it recurs — the next measurement, already scoped

The harness's VEH/UEF filter (`MainPage.xaml.cpp:1370-1509`) prints the AV kind, address, module table, a
40-frame `CaptureStackBackTrace`, a 256-word raw stack dump and arch-split registers. Two additions make
the next occurrence go further, and both must be written for **x64 and ARM32 from the start** (CLAUDE.md:
a file that has not been compiled for an architecture is not a file that compiles for it):

- **The faulting registers, not just the stack**: x64 `Rcx/Rdx/R8/R9/Rsi/Rdi/Rax`, ARM `R0-R3`. Here `rsi`
  is `this` and `[rsi+0x4E0]` was already consumed into `rcx`, so `rcx` alone identifies the garbage; `rsi`
  identifies the object. The raw stack dump does not contain them.
- **The marker file has to carry it too.** `gpuinit-steps.txt` is the channel that has survived every
  crash and hang in this project; a UEF line in `log.txt` is written by a process that is about to die.
  `apoFetchStallReport`'s pattern (`Src\port\WebCoreDriver.cpp`, "formatted inside gpuLogMarkerF's frame")
  is the shape to copy — and note CLAUDE.md's rule about no stack buffers inside the
  `#pragma strict_gs_check(push, off)` regions.

## 6. Tooling that came out of this, and the false trail

- **`llvm-symbolizer --obj=<dll> --relative-address`** reads the PDB directly and gives function + inline
  chain + file:line. This supersedes the fragile route of parsing the line table: `--no-inlines` and
  `--relative-address` are the flags (there is no `--inlines=false`, no `-i=0`), and `llvm-pdbutil` has
  **no `find` subcommand** (its subcommands are `bytes/diadump/dump/explain/export/merge/pdb2yaml/pretty/
  yaml2pdb`). The line-table route (`apo-frames-many.ps1`) is still useful for one thing and one thing
  only: the `offset = RVA − 0x1000` calibration, which is where the `0x1000`/`0x2000` ambiguity in that
  script's output comes from.
- **`CaptureStackBackTrace`'s chain was junk here** and must not be quoted as a stack. In that record the
  frames symbolised into `CurlRequest.cpp:477`, `PNGImageDecoder.cpp:945`, `EventNames.cpp`,
  `CSSPropertyParsing.cpp`, `HashMap.h` and `FloatPolygon3D.cpp` — incoherent for a `BitmapTexture`
  destructor, and explicable: the walker reads return addresses that are plain values inside a 46 MB image
  and happily attributes each to whatever symbol contains it. **A frame that resolves is not a frame that
  is real**; the same rule as `Doc/UNKNOWN-EXPORTS.md`'s "a symbol that exists is not a symbol that is
  callable".
- **`VEH: addr=…` is the absolute fault PC, not a module-relative address.** Subtracting the base once is
  the whole operation, and getting the base from the wrong row of the module table produces an offset in
  the same *shape* as the right one (`0x20418C2` against the correct `0x418C2`) — both are "inside a
  46 MB image", so the image-size sanity check does not catch it. The check that does: symbolise the
  offset and see whether the answer is a function that could plausibly be there.

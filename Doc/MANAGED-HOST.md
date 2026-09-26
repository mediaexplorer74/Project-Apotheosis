# Managed-host diagnostics — what the C# side can actually see

> 2026-09-26. This is the reference for the diagnostics that exist **because** the host is managed.
> The native harness does not need any of it: it has OutputDebugString, a UEF filter and wedge
> dumps, all of which are unavailable or invisible inside an App Container.

## The problem this solves

On the managed side a native fault is not an exception, so none of the normal reasoning applies:

- `catch` sees nothing (the CLR calls `FailFast(7)` before the P/Invoke returns),
- there is no return code to inspect,
- `OutputDebugStringA` goes nowhere — nothing outside the App Container can read it,
- and the driver's own `WebCoreStage` / `WebCorePortTrace` trace files are **gated on
  `FONTCONFIG_FILE`** (they derive the output directory from it), so before that variable was set
  natively the engine wrote no trace at all. A crash in the first paint left literally no evidence.

## What the driver exports for the managed host

| Export | Purpose |
|---|---|
| `WebCoreSetSehGuard(int)` | opt in to the SEH guard. Must precede the first engine call |
| `WebCoreGetSehInfo(char*, int)` | last caught fault: `seh code=0xC0000005 at <module>+0x<off> guard=1` |
| `WebCoreSetFontConfigFile(const char*)` | set `FONTCONFIG_FILE` natively (`_putenv_s`) |
| `WebCoreGetFontInfo(char*, int)` | font probe summary (below) |
| `WebCoreDumpFontConfig()` | re-run the probe on demand |

All five are declared in **both** copies of the C ABI header
(`Src/port/WebCoreDriver.h` and `Src/harness/WebCoreDriver.h`) and in
`Src/port/WebCoreDriver.exports.def`.

## The fault record

`ApoRecordException` reads the `EXCEPTION_RECORD` (code, faulting address) and maps the address to
`module+offset` with `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)` — the UWP-safe way;
there is no PEB walk and `tlsm32` does not exist on Mobile. It then walks the stack with
`RtlCaptureStackBackTrace` and records each frame the same way.

The stack walk is a deliberate choice over reading `RSP` slot by slot: a raw RSP scan finds mostly
*data*, and the port is built `/O2` without frame pointers, so an FP chain is not available either.

## The font probe

`ApoDumpFontConfig` runs once per process on the first render, and on demand. It reports what the
font stack actually got rather than whether the call succeeded:

```
font[manual]: FONTCONFIG_FILE=<path>\fonts.conf dir=<install>\Assets\fonts
  cachedir=<LocalState>\fontconfig-cache cachedirPresent=yes
  | segoeui.ttf=959752 | arial.ttf=1016724 | times.ttf=1190736 | cour.ttf=817164 | simhei.ttf=9751960
  | ft=not-probed | fc=not-probed
```

Each shipped font is reported with its size and its sfnt magic (`0x00010000` / `true` / `OTTO`), so
"missing" and "present but corrupt" are distinguishable. The `ft=` / `fc=` fields are honest about
what is *not* compiled in (see below).

### Why the live freetype/fontconfig probes were removed again

The first version called `FcInitLoadConfigAndFonts()` / `FT_New_Face()` under `__has_include`. It had
to be taken back out, for a reason worth remembering: **this region of `WebCoreDriver.cpp` is inside
the file's anonymous namespace**, so a header included *there* declares its namespaces into that
anonymous scope. Whichever of the two headers was reachable put a `std` in that namespace and made
every unqualified `std::` use in the rest of the file ambiguous — first failure seen at the file's
own `glReadPixels` buffer, 1000 lines from the `#include`. The file-level probes need no third-party
header and answer the question; if the live probes are ever wanted they belong in their own
translation unit at file scope.

Two more consequences of that same constraint, both of which cost a compile cycle:

- The probe's locals are all `apo`-prefixed. WebKit's include soup defines macros like `conf` (which
  expands to `cosf`), so ordinary names get macro-expanded and fail in ways that read as nonsense.
- `<excpt.h>` must be included **at the top of the file**, not next to the filter. `GetExceptionInformation`
  is a function-like macro there, so declaring it by hand is an error.

## Reading a run

The C# app writes `stage.txt` (`WriteStage`) and the driver writes `port-trace.txt` and
`stage-port.txt` into `LocalState`. A typical failure sequence:

```
stage.txt      after-home-render rc=-17
               SEH: seh code=0xC0000005 at WebCoreDriver.dll+0x0 guard=1
               fonts: font[manual]: FONTCONFIG_FILE=... cachedirPresent=yes | segoeui.ttf=959752 | ...
stage-port.txt WCRH:frame-init
```

Read it as three separate facts: the **return code** says the guard worked, the **SEH line** says
what faulted, and `stage-port.txt` says how far the engine got. `+0x0` as the faulting address means
a call through a null function pointer; a real address inside `WebCoreDriver.dll` would mean the
fault is in driver code proper.

## Constraints on adding more diagnostics

- Trace through `WebCorePortTrace` / `WebCoreStage`, never `OutputDebugStringA` — the latter is invisible in the container.
- Anything that needs a new `#include` goes in a **new TU**, not into this file's namespace scope.
- Every new export needs three edits: the `.cpp`, the `.def`, and **both** header copies.
- The native line must not change behaviour: the guard stays opt-in and off by default.

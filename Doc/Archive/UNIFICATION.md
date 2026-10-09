# Unification of the two build lines

Working notes for a body of work that is deliberately **not** a refactor for its own sake. Every item
here removes a duplicated copy of something, and duplication in this project has already produced
live crashes, not just wasted bytes. Written 2026-08-17; the ARM half waits for the postponed device
week.

Read this together with `CLAUDE.md` (durable rules) and `Doc/BUILD-NOTES-X64.md` (how x64 is built
today). Where the two disagree with this file, this file is the newer measurement.

## The central observation: the two lines are mirror images

Neither line is "the correct one". Each made the opposite choice on each of two independent
questions, and nobody ever brought them together.

| | engine | Cairo |
|---|---|---|
| **x64** (`build-x64-gpu`) | `WebCore.dll` + `JavaScriptCore.dll` (DLLs) | static `cairo-complete-x64-uwp.lib` |
| **ARM** (`build-arm32-gpu`) | `WebCore.lib` + `PAL.lib` + `WTF.lib` (static, monolith inside the exe) | `cairo-2.dll` |

The common denominator that both lines can reach is **DLL form for both**. Two arguments for that
direction:

1. On x64 the DLL engine already works, and it is the line we can actually run and debug locally.
2. Per the maintainer's account of the upstream original
   (`Jimmyxiao2009/Project-Apotheosis`), that project shipped a single `WebCore.dll` on ARM32. If so,
   DLL form is *proven* on the device, and this fork's static ARM monolith is a deviation rather than
   a necessity. Verifying this is task #12 and it is not yet confirmed from the upstream sources.

## Why duplication here is a crash class, not an aesthetic

`Src/port/webcore-exports.def:19-25` carries a note from an earlier round of this work about a real
failure: the exe received **its own copy** of `Document::links()`, bound to its own uninitialised
`HTMLNames` globals, and the result was `0xC0000005` inside `HTMLCollection::item(0)`.

Two copies of code that owns state means two sets of global variables. That is the same shape as the
startup death in 0.1.8.64 through 0.1.8.67 (a frozen copy of generated XAML code drifting from the
markup it was generated from) and the same shape as `/FORCE:MULTIPLE` silently picking one of two
`cairo_*` implementations. The instinct to avoid multiplying entities is confirmed by the code.

## Track A: the link (task #13, x64 first)

### A1. `WebCoreFull.lib` is 3.49 GB of dead weight

It sits in `build-x64-gpu\lib\`, is produced by a one-off `mkfull.ps1`, and **nothing links it**.
The only things pointing at it are a stale comment and the documentation
(`Doc/BUILD-NOTES-X64.md:68,73,99`), plus the leftover response files `wcf.rsp` and
`WebCoreFull.rsp`.

`Src/port/PlatformWinUWP.cmake:118-128` already records what replaced it: the module definition file
`Src/port/webcore-exports.def`, which exports the internal WebCore symbols the port needs, thereby
"eliminating the need for WebCoreFull.lib static linking". The replacement happened; the 3.49 GB
stayed behind. It does not affect appx size, only disk and the attention of the next agent.

**Action:** delete `WebCoreFull.lib`, `wcf.rsp`, `WebCoreFull.rsp` and `mkfull.ps1`; correct the three
lines in `Doc/BUILD-NOTES-X64.md`.

### A2. `WebCore.dll` already exports all of Cairo

`WebCore.lib` carries **472 `cairo_*` symbols and 14 `Fc*`** (fontconfig). It does *not* carry
freetype, harfbuzz, pixman, png or sqlite: those come from their own DLLs. So the static Cairo linked
into the exe is literally a second copy of code that already lives inside `WebCore.dll` — which is
exactly why the link needs `/FORCE:MULTIPLE`.

### A3. Dropping static Cairo is a six-symbol job

`llvm-nm` over `WebCoreDriver-gpu.lib` says that if static Cairo goes away, precisely these remain
undefined:

```
cairo_paint
cairo_ft_scaled_font_lock_face
cairo_ft_scaled_font_unlock_face
GraphicsContextCairo::GraphicsContextCairo(RefPtr<_cairo>&&)
GraphicsContextCairo::~GraphicsContextCairo()
WTF::DefaultRefDerefTraits<_cairo>::derefIfNotNull(_cairo*)
```

Six lines in `webcore-exports.def` — a mechanism the project already has and already relies on. If it
holds, the following all go away together: 2.31 MB of static Cairo in the exe,
`/NODEFAULTLIB:libcmt.lib`, the whole `BitBlt`/`AlphaBlend` story in `gdi-stubs.obj`, and most likely
`/FORCE:MULTIPLE` itself.

### A4. Ordered plan

1. Delete the dead `WebCoreFull.lib` and fix `Doc/BUILD-NOTES-X64.md`. No build risk.
2. Add the six symbols to `webcore-exports.def`, relink WebCore, drop
   `cairo-complete-x64-uwp.lib` from `Harness.vcxproj:75`, then measure the appx with PowerShell
   `.Length` before and after. **Baseline: 51.21 MB.**
3. Test whether `/NODEFAULTLIB:libcmt.lib` and `/FORCE:MULTIPLE` can now go.
4. Try removing the seven duplicated stub translation units from
   `Src/port/link-driver-gpu-x64.ps1:20-21`. This one may genuinely not be possible: `WebCore.lib`
   exports no `MainThreadSharedTimer` symbol at all, so the driver's own copy may be load-bearing.
5. ARM only after the postponed device week, and only once Track B below unblocks the ARM harness.

**Standing promise for step 2:** if removing static Cairo produces a *second* wave of undefined
`_cairo_*` internals, that gets reported and the work stops there. The archive does not get quietly
restored to make the link succeed.

## Track B: one Windows SDK (task #15)

Goal: get every consumer onto `10.0.19041.0` so `10.0.26100.0` can be uninstalled (**~1.5 GB**:
26100 measures 1506 MB across Include/Lib/bin/Platforms/UnionMetadata/References, 19041 measures
1374 MB) and, more importantly, so both architectures run the **same XAML compiler**.

> **Status: the harness half is DONE, same day.** `Harness.vcxproj` now pins `10.0.19041.0`
> unconditionally for both platforms. What unblocked it was removing the five desktop-partition
> loader calls described in B3 — `LoadPackagedLibrary` for `icuuc75.dll` and `libEGL.dll`, and the
> linker-provided `__ImageBase` instead of `GetModuleHandleW(nullptr)` for the module-base log. With
> those gone, a full 19041 compile of all ten harness translation units (forced by touching them, not
> by `/t:Rebuild`, per §C1) produced zero errors and an appx of 51.21 MB with 50 payload files —
> identical to the 26100 build — which installs, launches, walks its whole constructor trace and
> renders. `ModuleBase(Harness.exe) = 00007FF6C19E0000` confirms `__ImageBase` behaves like the API it
> replaced.
>
> That also settles B3's open question: the `MSB4181` / `CompileXaml` failure under 19041 was a
> **cascade of the compile errors**, not an SDK defect. 19041's XAML compiler is fine.
>
> What remains for this track is the engine half, listed in B5: `Src/setenv.ps1`,
> `Src/port/x64-uwp-env.ps1`, `Src/port/vcpkg-triplets/x64-uwp-toolchain.cmake`, the meson cross files,
> and a reconfigure of `build-x64-gpu` (hours of `ninja -j1`). Until then 26100 stays installed.

### B1. What is true about the two SDKs (measured 2026-08-17)

| | 19041 | 26100 |
|---|---|---|
| target arches under `Lib\<ver>\um` | arm, arm64, x64, x86 | arm64, x64, x86 — **no `arm`** |
| `bin\<ver>\XamlCompiler\...Build.Tasks.dll` | present, 2 484 736 B | present, 2 509 312 B |
| UAP platform, `Windows.winmd`, winrt headers | present | present |
| contract references | 88 | 97 |

So 26100 can never build ARM32, and `Src\port\arm32-uwp-env.ps1:12` correctly pins 19041 for the ARM
engine line.

### B2. The recorded reason for x64 being on 26100 is wrong

`Doc/Summary.md:278,338,349,364` and `Doc/BUILD-NOTES-X64.md:105,129` state that 19041 "is missing the
XAML compiler DLL". It is not: the DLL is present and full size, as the table above shows. That note
most likely described an SDK installed incompletely at a time when the machine was short of disk
space. `Doc/AGENTS-legacy-2026-08.md:256,270,282` repeats it too, but that file is a verbatim
historical record and should be left alone.

### B3. The real blocker, found by trying it

Moving `Harness.vcxproj` to an unconditional 19041 and running a full rebuild fails, and not on XAML:

```
MainPage.xaml.cpp(204):  error C3861: 'GetModuleHandleA': identifier not found
MainPage.xaml.cpp(206):  error C2065: 'LOAD_LIBRARY_SEARCH_SYSTEM32': undeclared identifier
MainPage.xaml.cpp(206):  error C3861: 'LoadLibraryExA': identifier not found
MainPage.xaml.cpp(1235): error C3861: 'GetModuleHandleW': identifier not found
MainPage.xaml.cpp(3905): error C3861: 'GetModuleHandleA': identifier not found
MainPage.xaml.cpp(3906): error C3861: 'LoadLibraryA': identifier not found
MainPage.xaml.cpp(4029): error C1083: Cannot open include file: 'MainPage.g.hpp'
...XamlCompiler\Microsoft.Windows.UI.Xaml.Common.targets(486,5): error MSB4181:
    The "CompileXaml" task returned false but did not log an error.
```

Cause, verified in the headers themselves: `Include\10.0.19041.0\um\libloaderapi.h:370` guards
`LoadLibraryExA` with

```c
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM | WINAPI_PARTITION_GAMES)
```

— no `WINAPI_PARTITION_APP` — whereas 26100 declares the same function unguarded. An
`ApplicationType=Windows Store` project compiles with `WINAPI_FAMILY=WINAPI_FAMILY_APP`, so under
19041 these calls simply do not exist. The `MainPage.g.hpp` and `CompileXaml` errors are downstream of
that: with the compile broken, no generated file is produced.

### B4. The consequence nobody had noticed: the ARM harness cannot compile either

The three call sites are **not** architecture-guarded:

| site | call |
|---|---|
| `MainPage.xaml.cpp:204-206` | ICU 75 probe: `GetModuleHandleA("icuuc75.dll")`, then `LoadLibraryExA(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` |
| `MainPage.xaml.cpp:1235` | module base for symbolizing crash stacks: `GetModuleHandleW(nullptr)` |
| `MainPage.xaml.cpp:3905-3906` | EGL probe: `GetModuleHandleA("libEGL.dll")`, then `LoadLibraryA("libEGL.dll")` |

The ARM harness builds against 19041, so it hits exactly the same wall. These are all recent, x64-era
additions, which is consistent with the ARM harness not having been rebuilt since they landed. **This
is an ARM-line landmine that is worth defusing before the postponed device week, not during it.**

### B5. Ordered plan

1. Make the harness 19041-clean. `MainPage.xaml.cpp:1236` already uses `LoadPackagedLibrary`, the
   app-partition API, a few lines below one of the offending calls — so the pattern is established.
   Both DLLs in question (`icuuc75.dll`, `libEGL.dll`) ship inside the package, which is exactly what
   `LoadPackagedLibrary` is for. The `GetModuleHandleW(nullptr)` module base needs a different answer
   (check the guard on `GetModuleHandleExW`, or drop the line and take the base from the crash dump).
   This step is a prerequisite for **both** the ARM harness and for retiring 26100.
2. Only then flip `Harness.vcxproj` to a single 19041 for both platforms, and rebuild x64 to confirm
   19041's `CompileXaml` actually succeeds once the C++ compiles. That claim is currently untested:
   B3 never got far enough to prove it.
3. Engine line: `Src/setenv.ps1:23` `APOTHEOSIS_SDK_VER` and `Src/port/x64-uwp-env.ps1:11` to 19041,
   then reconfigure `build-x64-gpu` and rebuild WebCore/JSC/WTF. This is hours of `ninja -j1`, so
   batch it with the next full rebuild that is needed anyway. Low *risk* though: the ARM line already
   compiles all of WebCore/JSC/WTF against 19041 headers, which proves the engine sources do not need
   26100.
4. Host-tool references, zero risk and no rebuild: `Src/port/vcpkg-triplets/x64-uwp-toolchain.cmake:10`
   (`CMAKE_MT`), and the meson cross files still pointing `mt.exe`/`rc.exe` at 26100 —
   `cairo-cross-clang.txt:5,7`, `fontconfig-cross-clang.txt:5,7`,
   `fontconfig-cross-clang-x64.txt:8,10,43,45`, `harfbuzz-cross-clang.txt:5,7`,
   `harfbuzz-cross-clang-x64.txt:8,10,43,45`. The `-x64` ones also carry 26100 `LIBPATH`s. These only
   matter on a deps rebuild.
5. Correct the stale claim in `Doc/Summary.md` and `Doc/BUILD-NOTES-X64.md` (leave the legacy AGENTS
   file as the historical record).

**Until step 3 lands, 26100 has to stay installed**: `build-x64-gpu`'s CMakeCache has its include
paths baked in, so an incremental WebCore rebuild — likely needed for task #10 — would break without
it. Nothing already built depends on it: linking only needs `Lib\<ver>\um\x64` and `ucrt\x64`, which
19041 has.

## Track C: one copy of the generated XAML code (done 2026-08-17)

`MainPage.xaml.cpp` used to end with a hand-pasted snapshot of `Generated Files\MainPage.g.hpp`:
`InitializeComponent()`, a ~490-line `Connect()` switch with hardcoded connection ids, and
`GetBindingConnector()`. The XAML compiler renumbers those ids on every `MainPage.xaml` change; the
snapshot did not. Adding the diagnostics page gave `DiagPage` id 7 (previously `TabSwitcher`, also a
`Grid`, so `safe_cast` accepted it silently) and moved `ContentArea` from 76 to 88, an id with no
`case`. `LoadComponent` then built the whole tree, bound fields to the wrong controls, and left
`ContentArea` null — and a null `^` handle in C++/CX is a raw dereference, an access violation that
`catch (...)` cannot catch under `/EHsc`. Startup died with an empty LocalState in 0.1.8.64 through
0.1.8.67.

Fixed by replacing 522 lines with `#include "MainPage.g.hpp"`, deleting the orphaned
`Src\harness\MainPage.g.hpp` snapshot, and widening the fallback trigger from `RootGrid == nullptr` to
`RootGrid && ContentArea && RootShift`. Two permanent diagnostics came out of it and are worth keeping
in mind: `ctor-trace.txt` (one line per constructor checkpoint, written before `LogInit` makes
`log.txt` live) and `unhandled.txt` (`App::UnhandledException`).

This is the cheapest illustration of the whole document: while the two architectures run different
XAML compilers (Track B), a frozen `Connect()` could not have been correct for both, so the *only*
correct arrangement is the generated file included from one place.

### C1. The generated file is a build *input* the compiler no longer produces

Including `MainPage.g.hpp` costs one thing that has to be written down, because it bit within the
hour. `Generated Files\MainPage.g.hpp` is **not** regenerated by a normal build: a build rewrites
`MainPage.g.h` (declarations only, 61 fields, 6 KB), `XamlTypeInfo.*` and `XamlBindingInfo.g.h`, and
leaves the `.g.hpp` carrying `InitializeComponent()`, `Connect()` and `GetBindingConnector()`
untouched. Invoking `MarkupCompilePass2` on its own to force it fails with `WMC9999: Object reference
not set to an instance of an object` — the second pass of this XAML compiler crashes in this project.
That is what an earlier agent was working around by pasting the file into `MainPage.xaml.cpp`, and it
also explains the two orphans in `Generated Files\`. `App.g.hpp.backup` and `MainPage.g.hpp.backup` are
**not** failed-write leftovers, as first assumed here: `Doc/AGENTS-legacy-2026-08.md:282-283` records
them as a deliberate workaround from the VS18 era, when the project was built with VS 2026 Insiders
(MSVC 14.51, toolset v145 — see `Doc/PLAN.md:257`). That compiler emitted `.g.hpp` and overwrote it
with a broken version even with `XamlMarkupCompileEnabled=false`, so pre-generated copies were kept
under `.backup`. VS 2022, the only Visual Studio installed today, emits `.g.h` and no `.g.hpp` at all.
Two different XAML compilers, two different output shapes — which is the same lesson as Track B, one
layer up.

Consequences, in order of how much they cost:

- **Never build this project with `/t:Rebuild`.** Clean deletes `MainPage.g.hpp`, Build does not bring
  it back, and the next compile dies with `C1083: Cannot open include file: 'MainPage.g.hpp'`.
  `Src\tools\_build-appx-x64.bat` uses a plain `Build`, and now the reason is on record. Cost of
  learning it the hard way on 2026-08-17: one wasted build round, plus a false lead blaming SDK 19041
  for a breakage that had nothing to do with it.
- If the file does go missing, restore it from `MainPage.g.hpp.backup` — but verify before trusting
  it. Every `x:Name` declared in the freshly generated `MainPage.g.h` must be assigned in the restored
  `Connect()`, **and in the same order**, because ids are handed out in document order. Verified that
  way on 2026-08-17 before restoring: 61 declared, 61 assigned, order identical, highest id 89,
  `DiagPage` present. A restore without that check re-introduces exactly the id drift Track C fixed.
- **Open question, worth settling before the markup changes again:** what makes the compiler emit
  `.g.hpp` at all. It did at 12:20 on 2026-08-17 — the `.backup` already contains `DiagPage`, so it
  post-dates the diagnostics page — and it did not at 13:38 after a `Rebuild`. Until that is
  understood, treat `MainPage.g.hpp` as a source file that merely happens to be machine-written: copy
  it aside before touching `MainPage.xaml`, and run the field-and-order check afterwards.

None of this weakens the fix. The frozen copy failed **silently**, binding fields to the wrong
controls and leaving `ContentArea` null; the included copy fails **loudly**, at compile time, with a
one-line error naming the file. A build that refuses to produce an appx is strictly better than an
appx that launches into an access violation.

## Adjacent cleanup spotted along the way

Not unification as such, but recorded here so it is not re-discovered from scratch. File:line as of
2026-08-17.

- `Src/port/` carries a layer of one-off debugging residue from the link fight: `repro_*.cpp`,
  `mangle-repro*`, loose `*.obj`/`*.lib`/`*.dll`, `*.log`, `undef-*.txt`, `_*.bat`. Deleting it is
  safe and makes the directory readable; `CLAUDE.md` currently has to warn agents to ignore it.
- `x64-cycle.ps1:87-89` calls `Remove-AppxPackage` on every run, which **wipes LocalState** — directly
  contradicting its own header comment, which says the version bump exists precisely to avoid that.
  Any log evidence from a manual test session is destroyed by the next cycle.
- The whole `S_SET_*` string block (zh/en/ru, all three written) is dead: `ApplyLanguage()` never
  touches the Settings page.
- No hardware-back handling exists anywhere: no `SystemNavigationManager`, no `BackRequested`. On a
  Lumia only the on-screen back arrows work. This one is a real device-line gap, not cleanup.
- `Src/port/webcore-driver-stubs.cpp:211-214,260-262` — stale notes.
- A stale comment on `paintToRGBA` still claims `USE(TEXTURE_MAPPER)=0`.
- Three stale `L"Apotheosis v0.1.8.25"` strings; the hero text hardcodes "ARM32"; the home page has a
  hardcoded Chinese `常用站点` heading (upstream heritage: do not translate it, but it should not be
  hardcoded either).
- `CheckForUpdate` at `MainPage.xaml.cpp:3207` still points at the upstream repository — relevant to
  task #12.
- ~508 comment lines were mangled to `?` by an encoding accident. Only those may be removed or
  replaced; genuine Chinese comments are heritage and stay.
- `deps-build/` versus vcpkg is documented contradictorily; the FL9_3 cap, `lib\onecore\arm` and ICU 75
  for ARM32 all still need a decision on the ARM line.

## State as of 2026-08-17

- Track A: not started. Baseline appx 51.21 MB.
- Track B: **harness done** — one SDK, 19041, for both platforms, verified by a full compile plus a
  runtime launch (0.1.8.70). The engine half (B5) is untouched, so 26100 must stay installed for now.
  The ARM harness compile blocker recorded in B4 is closed: the five call sites it named are gone.
- Track C: done and verified running (0.1.8.68). Re-verified on 0.1.8.69 after a `/t:Rebuild` deleted
  the generated `MainPage.g.hpp` and the compiler declined to write it back — see §C1, which is the
  one caveat this track carries.
- ARM device work: postponed by one week from 2026-08-17, but B4 says the ARM *harness* is already
  broken at compile time, independently of the device.




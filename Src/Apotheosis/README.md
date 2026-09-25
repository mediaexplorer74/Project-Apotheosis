# Src/Apotheosis — the C# harness experiment (stopped, kept on purpose)

**Status: abandoned experiment. Not built, not maintained, not expected to compile as-is.**
Nothing in the working browser depends on it. It is kept because the idea was sound and the reason it
was dropped turned out to be weak — so the next person should be able to pick the decision up on
accurate grounds instead of re-deriving it.

## What it is

A port of the harness UI from C++/CX to C#, started around 2026-07 with a deliberately reduced brief:
**address bar and content view only** — no history, no bookmarks, no find-in-page, no diagnostics page,
no overlays. It is not a half-finished copy of `Src/harness`; it is a smaller thing, finished to a
smaller specification.

Measured 2026-08-22:

| | `Src/harness` (C++/CX, working) | `Src/Apotheosis` (C#) |
|---|---|---|
| page code | 4937 lines | 2234 |
| XAML | 548 | 459 |
| ABI surface | 44 exports in `WebCoreDriver.h` | 30 `DllImport`s in `WebCoreDriver.cs` |

The 30-of-44 gap is mostly the features the brief excluded, not missing work.

## Which solution is which

- `Src/Harness.sln` — the **working** C++/CX harness. This is what ships.
- `Src/Apotheosis.sln` — this C# project alone.
- `Apotheosis.sln` (repo root) — `WebCoreDriver` **and** this C# project. Opening it and building
  everything will not produce the browser; that is `Src/Harness.sln`.

## Why it was stopped, and why that reason does not hold up

It was stopped on the argument that marshalling C ABI calls from managed code would cost performance.
On the numbers that objection is **overstated**:

- A `DllImport` call with blittable arguments costs tens of nanoseconds. This ABI is coarse:
  `WebCoreLiveTick` performs a run-loop pump, a rendering update, layout and a full paint — measured in
  hundreds of milliseconds on the device. Thirty such calls per second at 50 ns each is noise.
- The frame buffer is the only bulk transfer: `WebCoreLiveTick(uint8_t*)` fills roughly 711 KB
  (360×494×4) per tick. From C# a `byte[]` can be pinned with `fixed` and passed directly — no copy.
  The C++/CX harness already copies that buffer into a `WriteableBitmap`, so the cost is comparable.

And one thing C# would genuinely have bought, which is worth stating plainly: **a null reference in C#
throws a catchable exception with a stack trace, while a null `^` handle in C++/CX is a raw access
violation that `catch (...)` cannot catch under `/EHsc`.** That exact failure killed startup silently in
versions 0.1.8.64 through 0.1.8.67, and it is what motivated this experiment. A minimal UI also means
few XAML connection ids, so less exposure to the drift that caused it, and C# has no two-pass
`MainPage.g.hpp` trap at all.

That wound is now fenced on the C++ side instead: `Src/tools/verify-xaml-connect.ps1` fails the build on
any markup/code drift, and the "never `/t:Rebuild`" rule is in CLAUDE.md.

## What actually argues against resuming *right now*

1. **The instrumentation, not the UI.** The C++ harness has since grown the machinery that is currently
   closing an open defect: a dedicated engine thread with a labelled job queue, a heartbeat reporting
   `busy` / `pending` / `finished` / `job` / `tickstep`, the live-tick step counter, screen-wake and test
   switches shipped inside the package, the duplicate-navigation guard, and the GPU-enable path with its
   software fallback. None of that is UI decoration and a minimal brief does not excuse porting it.
2. **.NET Native for UWP on ARM32 under VS 2022 is an unknown.** Not a small risk, not a measured one
   either — nobody has tried. This, rather than marshalling, is the question to answer first.
3. **Timing.** Replacing the harness mid-investigation means restarting a defect hunt with zero
   instrumentation.

## What this directory is good for today

- A compact, readable statement of the **minimum viable harness** — useful when arguing about what the
  MVP actually needs.
- A ready reference for the ABI surface from a managed caller, if the engine is ever embedded elsewhere.

## If you resume it

Answer (2) above first — build an empty C# UWP app for ARM32 and deploy it to the device. If that does
not work, nothing else matters. Then port the plumbing before the UI: engine thread, job queue with
labels, heartbeat, packaged switches. Read `Doc/WIKI_EN.md` §11 and `Doc/PUMPLOOP-SILENT-DEATH.md` first,
so the diagnostics come across intact rather than being rediscovered.

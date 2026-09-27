# Phase RnD — the C# / UWP host is now the product line

> **Written 2026-09-26.** This file is the front door for the current phase. Everything under
> [Archive/](Archive/) and every file that predates this date describes the *C++/CX harness* line and
> is kept as historical record; where the two disagree, this file and [PLAN.md](PLAN.md) win.

## What changed

The project used to be a C++/CX UWP harness (`Src/harness`, `Harness.vcxproj`) with a native engine
underneath. It now ships a **C# UWP app** (`Src/Apotheosis`, `Apotheosis.csproj`) that drives the
same engine through the C ABI of `WebCoreDriver.dll`. The C# app is the shipping target; the
harness is a debugging tool for the native side only.

| | C++/CX harness (legacy) | C# UWP app (current) |
|---|---|---|
| Project | `Src/harness/Harness.vcxproj` | `Src/Apotheosis/Apotheosis.csproj` |
| Links the engine | **statically** (`WebCoreDriver-gpu-*.lib` + `WebCoreFull.lib`) | **dynamically** (`native/<arch>/*.dll` staged into the appx) |
| Runs on | x64 dev box, ARM32 in theory | x64 dev box now, Lumia 950 as the goal |
| Diagnostics | OutputDebugString, wedge dumps, UEF | files in `LocalState` (`stage.txt`, `port-trace.txt`, `gpuinit.txt`, …) |
| Crash model | process dies, WER dump | **must not die** — see the SEH contract below |

## The one hard constraint that shapes everything

**A native exception that unwinds out of a P/Invoke is fatal on the managed side.** .NET Native's
P/Invoke guard (`mrt100_app!RhpPInvokeExceptionGuard` → `RhpFailFastForPInvokeExceptionPreemp`) calls
`FailFast(7)`: WER reports `0x00001007` in `SharedLibrary.dll`, the process is gone in well under a
second, and **no managed `catch` can see it** — there is no exception object, no return code, no
stage line. The native harness does not have this behaviour and relies on its own UEF filter and
wedge dumps, which is why the same engine code can be a debuggable crash there and an
unexplainable death here.

So the driver now guards every engine entry point in SEH and the C# app opts in:

```c
// Src/port/WebCoreDriver.cpp — bodies renamed to *Impl, three-line wrappers around them
int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    __try { return WebCoreRenderHtmlImpl(utf8Html, w, h, outRGBA); }
    __except (ApoSehFilter(GetExceptionInformation())) { return kErrSeh; }
}
```

- `WebCoreSetSehGuard(1)` — called from `MainPage.xaml.cs` `SetupRuntimeEnv()` before any engine call.
- Guard **on**: a native fault becomes `rc = -17` (`kErrSeh`) plus a `WebCoreGetSehInfo` line. The app survives and can show an error page.
- Guard **off** (the default): the filter returns `EXCEPTION_CONTINUE_SEARCH` and the host sees exactly the fault it saw before. The native harness line is therefore unchanged, which is deliberate — it is the shipping browser.

A function containing `__try` cannot also require object unwinding, which is why the bodies are
`static *Impl` functions and the exported names are the wrappers. Verified: `clang-cl` accepts
`__try/__except` on x64 with `_HAS_EXCEPTIONS=0` and `/EHs-c-` (SEH is independent of C++ exceptions).

## The managed host cannot set `FONTCONFIG_FILE`

`Environment.SetEnvironmentVariable` does not reach the native process environment block in an App
Container, so the engine saw no font configuration at all and fontconfig matched nothing. The driver
now sets it natively, exactly as the C++ harness does:

```csharp
WebCoreDriver.WebCoreSetFontConfigFile(confPath);   // _putenv_s inside the driver
WebCoreDriver.WebCoreSetSehGuard(1);                // before the first engine call
```

## Build lines

Two independent lines, sharing the source tree and the SDK but nothing else — not objects, not
generated bindings, not `build.ninja`, not the CMake cache.

### x64 (dev machine)

```powershell
. .\Src\setenv.ps1
. .\Src\port\x64-uwp-env.ps1
# engine (only when WebKit itself changes — hours on this box)
cmake -S . -B build-x64-gpu -G Ninja -DCMAKE_TOOLCHAIN_FILE=$env:APOTHEOSIS_PORT/Toolchain-x64-UWP-clang.cmake
ninja -C build-x64-gpu -j1
# port objects + driver DLL (minutes)
pwsh -File Src/port/build-driver-dll-arm32.ps1 -Arch x64
# C# appx (minutes)
MSBuild /restore /p:Configuration=Debug /p:Platform=x64 Src/Apotheosis/Apotheosis.csproj
```

### ARM32 (Lumia 950, W10M)

Same two-line shape, with `build-arm32-gpu` and `-Arch arm`. The engine trees
(`WebCore.lib`, `JavaScriptCore.lib`, `WTF.lib`, `PAL.lib`) are **already built** in
`build-arm32-gpu/lib`, so a driver rebuild does *not* require the multi-hour WebKit build — see
[BUILD-ARM32.md](BUILD-ARM32.md) for what the ARM driver line still needs.

## Phase exit criteria

The phase is done when the **same build** runs on a real Lumia 950 under W10M, not when it runs on
the dev machine. Until then, treat x64 results as necessary and not sufficient: the ARM32 App
Container, the ABI, the JIT and the font stack all differ, and every one of them has already cost
this project a separate bug.

## Document map

- [PLAN.md](PLAN.md) — live state, the v1.0 definition, and open work. Start here for "where are we".
- [ARCHITECTURE.md](ARCHITECTURE.md) — how the C# line is put together: layer and threading diagrams, the C ABI, the present path, the SEH contract, and what is measured versus assumed.
- [MANAGED-HOST.md](MANAGED-HOST.md) — what the C# side can see: the SEH contract, the new exports, the font probe, how to read a run.
- [LESSONS.md](LESSONS.md) — operating rules distilled from past failures.
- [BUILD-ARM32.md](BUILD-ARM32.md) — the ARM32 line: engine archives, the driver-DLL link, deployment, known ARM traps.
- [BUILD-X64.md](BUILD-X64.md) — the x64 line: the three build stages, the static `WebCoreFull` archive, the C# appx build.
- [GPU-LIVENESS.md](GPU-LIVENESS.md) — GPU investigation and presentation status.
- [XAML.md](XAML.md) — the XAML/C++/CX contract and safe UI rules (legacy host).
- [GITHUB.md](GITHUB.md) — publishing this tree safely.
- [Archive/](Archive/) — dated snapshots and closed investigations. Reference only, never a spec.

## Rules that follow from the focus shift

- **Never build the appx with `/p:MinimalTest=true`** — it links no engine at all. It is a crash-isolation stub, not the product.
- **A managed-side change is not a measurement.** Only `LocalState` files and the return code count as evidence.
- **Preserve the native line's behaviour.** The C# app may add guard-rails; the guard must stay opt-in.
- **Keep the two C ABI headers in sync** (`Src/port/WebCoreDriver.h` and `Src/harness/WebCoreDriver.h`) — they are still byte-identical and both must be edited together.
- Device validation is manual, over WDP (`Src/tools/deploy-launch.ps1`). CI is explicitly deferred.

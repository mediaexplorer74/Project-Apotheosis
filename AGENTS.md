# AGENTS.md

This file provides guidance to AI agents (OpenCode, Codex, or other) when working in this repository.
It is a thin pointer to the single source of truth, not a duplicate of it.

**Read [CLAUDE.md](CLAUDE.md) first.** It is the single source of truth for this repo: what the
project is, the hard constraints, the layout, the architecture, and the exact build/deploy commands.
This file used to duplicate all of that and the two copies drifted apart, so it no longer does.
AGENTS.md adds only what OpenCode-style sessions most often miss without help.

## The one trap that costs the most time

**ASCII-only paths, no spaces.** The repo lives at `C:\Users\media\source\repos\Vibe\Apotheosis`
(or wherever `$env:APOTHEOSIS_ROOT` points). Ruby generators and meson choke on non-ASCII and on
spaces. Scripts resolve everything from `$env:APOTHEOSIS_ROOT` (`Src/setenv.ps1`) instead of
hardcoding. **Do not** hardcode `C:\Users\...` into a script or a test.

## The seven traps that cost the next-most time

- **Three toolchains that must not be mixed**: clang-cl for the engine and `Src\port\*.cpp`
  (lld-link), MSVC v143 for the C++/CX harness.
- **C++ exceptions are off** (`_HAS_EXCEPTIONS=0` + `/EHs-c-`) - clang's thumbv7-windows-msvc
  backend cannot lower `cleanupret`.
- **Every upstream WebKit edit is guarded** by `#if defined(WK_WINUWP)` and carries an
  `Apotheosis:` comment.
- **`ninja -j1`** for engine builds; parallel clang-cl exhausts RAM.
- **Never build the appx with `/p:MinimalTest=true`** - it links no engine at all; it is a
  crash-isolation stub, not the product.
- **The C ABI header exists twice** (`Src\port\WebCoreDriver.h`, `Src\harness\WebCoreDriver.h`) and
  both copies must be edited together.
- **Present only on the engine thread**; the UI thread must never synchronously wait on the engine.

## Build lines: practical commands (don't try to remember these)

Two parallel build lines share the same source tree, same SDK (10.0.19041.0), and same per-tree
build directory. They do **not** share compiled objects, generated bindings, build.ninja, or
cmake cache. Each lives in its own tree and is driven by its own env script.

### x64 (dev machine: `C:\Users\...\Source\Apotheosis`)

```powershell
. .\Src\setenv.ps1                    # sets $env:APOTHEOSIS_ROOT, $env:APOTHEOSIS_PORT, etc.
. .\Src\port\x64-uwp-env.ps1          # x64 INCLUDE/LIB/PATH for MSVC 14.44 + Win10 SDK
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build-x64-gpu -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$env:APOTHEOSIS_PORT/Toolchain-x64-UWP-clang.cmake \
  -DICU_ROOT=C:\icu-x64-uwp
& "C:\Program Files\CMake\bin\ninja.exe" -C build-x64-gpu -j1     # ENGINE build, no parallelism
pwsh -File Src/port/link-driver-gpu-x64.ps1                      # produces WebCoreDriver-gpu-x64.lib
& "C:\Program Files\MSBuild\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" \
  /p:Configuration=Release /p:Platform=x64 Src/harness/Harness.vcxproj  # appx build
```

### ARM32 (device: `Lumia 950` over WDP, build on x64)

```powershell
. .\Src\setenv.ps1
. .\Src\port\arm32-uwp-env.ps1         # ARM32 INCLUDE/LIB/PATH, cl 14.44 Hostx64/arm
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build-arm32-gpu -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$env:APOTHEOSIS_PORT/Toolchain-ARM32-UWP-clang.cmake \
  -DICU_ROOT=C:\icu-arm-uwp
& "C:\Program Files\CMake\bin\ninja.exe" -C build-arm32-gpu -j1
pwsh -File Src/port/link-driver-gpu-arm32.ps1
& "C:\Program Files\MSBuild\..." /p:Configuration=Release /p:Platform=ARM Src/harness/Harness.vcxproj
pwsh -File Src/tools/deploy-launch.ps1 -Ip <device> -Ver <ver> -Pub <pub>   # WDP install
```

**Key rule**: never `find .` or `Get-ChildItem -Recurse` over `.git`, `build-x64-gpu`, `build-arm32-gpu`,
`node_modules`, `.opencode` -- they will drown the output. Most of what an agent needs is reachable
through the two build trees and `Src/`.

## Deeper engineering detail (kept out of CLAUDE.md on purpose)

- `Doc/BUILD-NOTES-X64.md` - WK_WINUWP compile pitfalls, the harness link recipe, the ICU 75
  version trap, XAML compiler quirks, toolchain reinstall.
- `Doc/ARM32-BUILD-GUIDE.md`, `Doc/ARM32-RECOVERY.md` - the device line.
- `Doc/PUMPLOOP-SILENT-DEATH.md` - 16-build investigation that found and fixed the instant silent
  death (synchronous curl + CA re-parse + JIT in AppContainer).
- Claude Code project memory (`MEMORY.md` + one file per fact) - root causes, dead ends, real-device
  data points, and the upstream patch list.
- Legacy AGENTS drafts (this repo had several): see `Doc/AGENTS-legacy-*.md`.

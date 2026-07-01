# Project Apotheosis — Development Plan

> Updated: July 1, 2026
> See also: `Summary.md` (architecture), `WIKI_EN.md` (project wiki)

---

## 1. Repository Cleanup & Path Migration ✅

### Status: Completed

All `E:\Apotheosis\` hardcoded paths have been replaced with `$env:APOTHEOSIS_*` environment variables throughout `.ps1`, `.bat`, `.cmake` files. The `Harness.vcxproj` uses relative `$(ProjectDir)` syntax.

| Task | Status |
|------|--------|
| Remove `repro_*`, `mangle-repro*`, `_*.bat`, `*.obj`, `*.log` from `port/` | ✅ ~60 files deleted |
| Create `Src/setenv.ps1` with `APOTHEOSIS_ROOT` + sub-vars | ✅ |
| Fix 87 path occurrences in scripts | ✅ |
| Fix 83 quoting issues (single→double quotes) | ✅ |
| Fix `Harness.vcxproj` (vcxproj uses MSBuild properties) | ✅ |
| Fix `harness-cmd.bat` (bat uses `%VAR%` syntax) | ✅ |
| Verify zero `E:\Apotheosis\` in `.ps1`/`.bat`/`.cmake` | ✅ Clean |

### Key files created

| File | Purpose |
|------|---------|
| `Src/setenv.ps1` | Sets `$env:APOTHEOSIS_ROOT`, `PORT`, `HARNESS`, `TOOLS`, `ANGLE`, `CRASH` |

---

## 2. x64 Build Infrastructure ✅

### Status: ALL deps installed, CMake configured, WTF/bmalloc compiled, PAL header gen done

| Component | Status | Notes |
|-----------|--------|-------|
| LLVM/clang-cl 22.1.8 | ✅ Installed | `C:\Program Files\LLVM\` |
| Ninja | ✅ Via VS2022 | `...\CMake\Ninja\ninja.exe` |
| WebKit source (webkitgtk-2.52.4) | ✅ Cloned | commit e4ab5336 |
| ANGLE x64 binaries | ✅ Done | NuGet → `Src\angle\x64\` |
| vcpkg x64-uwp deps (16 pkgs) | ✅ ALL INSTALLED | Community triplet (no VS_PATH/TOOLSET bug) |
| ICU x64-uwp | ✅ Built | `C:\icu-x64-uwp\` |
| SQLite3 x64-uwp | ✅ Manually built | Amalgamation, `SQLITE_OS_WINRT=1` |
| CMake configure | ✅ FIRST SUCCESS (June 29, 2026) | `build-x64-gpu` generated |
| WTF compiled | ✅ | 12+ WK_WINUWP patches across 8 WTF files (RunLoopWin, FileSystemWin, WindowsExtras, DbgHelperWin, OSAllocatorWin, MemoryPressureHandlerWin, SignalsWin, PlatformWin.cmake) |
| bmalloc compiled | ✅ | getpid→GetCurrentProcessId via `add_definitions` |
| **PAL build** | ✅ **Header gen complete** | Object library — .obj files compiled as part of WebCore |
| **JavaScriptCore** (physical file set) | 🔄 **In progress** | ~370 WTF .cpp ✅; LowLevelInterpreter.cpp ✅ (GNU driver); JSC unified sources compiling; MacroAssemblerX86_64.cpp 🔴 (`-imsvc` in GNU rule) |
| WebCore | ❌ Not yet | Target: `ninja WebCore` → `bin/WebCore.dll` (includes PAL objs) |

### GNU driver for AT&T assembly files

LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp contain AT&T-syntax inline assembly that clang-cl cannot parse. Solution: two custom ninja rules using **clang++ (GNU driver)**. The GNU rules use `deps = gcc`, `-MD -MF $out.d`, `-o $out -c -- $in` (not the MSVC `/Fo`/`/showIncludes` flags). `FLAGS` must use `-D`/`-I` syntax; `-imsvc` in `INCLUDES` must be replaced with `-isystem` since clang++ doesn't understand `-imsvc`.

Status: LowLevelInterpreter.cpp ✅; MacroAssemblerX86_64.cpp 🔴 (`-imsvc` not yet replaced in its INCLUDES).

### Key findings from PAL build analysis

| Discovery | Detail |
|-----------|--------|
| **PAL is an OBJECT library** | No `PAL.lib` target exists. PAL's 19 `.cpp` files compile to `.obj` files that are directly linked into WebCore.dll |
| **2462 total ninja edges** | Full build graph for PAL target includes all header generation (1203 steps) + compilation steps |
| **`ninja PAL` only generates headers** | The `PAL` phony target depends on `PAL_CopyHeaders` only, not on PAL object compilation |
| **Actual compilation targets** | `ninja JavaScriptCore` → `bin/JavaScriptCore.dll`, `ninja WebCore` → `bin/WebCore.dll` |
| **C++23 confirmed** | `build.ninja` uses `-clang:-std=c++23`, WTF/bmalloc compile cleanly |
| **Build launch workaround** | Use `[System.Diagnostics.Process]::Start()` for long builds — tool's bash kills subprocesses after timeout |
| **Stale process hazard** | Two zombie clang-cl processes stalled for 1+ hour compiling `LowLevelInterpreter.cpp`. Always `taskkill /F /PID` stale PIDs + delete `.ninja_lock` |
| **CMake 4.0 missing rules** | Only ~10 of 34+ compiler/linker rules emitted in `rules.ninja`. Workaround: `patch-build-ninja-gnu.ps1` scans `build.ninja` for all rule names, auto-generates missing rules in `rules.ninja`. Also adds utility rules (`CLEAN`, `HELP`, `RERUN_CMAKE`) omitted by CMake 4.0 |

### GNU driver status (July 1)

Both LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp now compile cleanly with the clang++ GNU driver. The `-imsvc`→`-isystem` conversion is handled by `patch-build-ninja-gnu.ps1`. GNU rules use `deps = gcc`, `-MD -MF $out.d`, `-o $out -c -- $in` syntax.

**Important:** `build.ninja` corruption discovered — if the patch script is interrupted (timeout) during `WriteAllText`, the 38MB `build.ninja` is silently truncated. Always verify file integrity after patching, or re-run CMake with deleted `build.ninja` + `rules.ninja` to regenerate fresh files before re-patching.

### What was fixed in scripts

| Script | Fix |
|--------|-----|
| `configure-gpu-x64.ps1` | Added `$cmake` and `$ninja` path definitions (were undefined) |
| `link-driver-gpu-x64.ps1` | ANGLE path `$Root\angle\x64` → `$Root\Src\angle\x64`; added compile-each-source loop |
| `compile-driver-gpu-x64.ps1` | Full compile script with correct target triple, includes, UWP defines |

---

## 3. Multilingual UI (en/ru/zh) ✅

### Status: Completed

| Feature | Status |
|---------|--------|
| `.resw` files for en-US, zh-Hans, ru-RU | ✅ Created |
| `GetStr()` dual-source loading (`.resw` + fallback table) | ✅ Implemented |
| All hardcoded Chinese toasts replaced | ✅ 20+ locations |
| Language persistence (`settings.ini` → `lang=0\|1\|2`) | ✅ |
| `ComboBox` language selector in Settings | ✅ |

### Future improvements

1. Migrate XAML static labels to `x:Uid` binding
2. Auto-detect system language on first launch
3. Localize `kHomeHtml` (browser home page HTML)

---

## 4. WebKit Upgrade: 2.52.4 → 2.53.4

See `Doc/WEBKIT-UPGRADE.md` for full analysis.

**Status:** Research complete. **Effort:** Low-Medium (~2-3 days). Mostly Skia-focussed changes — low impact on our TextureMapper path.

---

## 5. Upstream Sync (gpu-path1, June 28)

### Status: Patches applied

| Commit | Change | Our sync |
|--------|--------|----------|
| `22c9721` | Anti-OOM + BackForwardCache disable | ✅ Patched `WebCoreDriver.cpp` |
| `1601b87` | MEDIA-PLAN.md | ✅ Copied to `Doc/` |
| `a5a1a20` | CryptoDigest → real OpenSSL SHA | ✅ Patched `stubs-crypto.cpp` |
| `ca387e7` | OOBE language selection | ⚠️ Our 3-lang i18n is more advanced |
| `c18e0fd` | MinVersion lowered to 14393 | ✅ Patched `Package.appxmanifest` |
| `5ff1a18` | License cleanup | ✅ Kept our LICENSE |
| `00d5427` | HTTP→HTTPS redirect fix | 🔄 Needs WebKit source patch |

### Files patched

| File | Change |
|------|--------|
| `Src/port/stubs-crypto.cpp` | CryptoDigest → real OpenSSL SHA |
| `Src/harness/Package.appxmanifest` | Version 0.1.8.5, MinVersion 14393 |
| `Src/port/WebCoreDriver.cpp` | BackForwardCache::setMaxSize(0), MemoryCache caps, WebCoreReleaseMemory() |
| `Src/port/WebCoreDriver.h` | WebCoreReleaseMemory() declaration |

---

## 6. Critical Technical Debt

### 6.1. Path system ✅ RESOLVED
- ✅ `E:\Apotheosis\` removed from all scripts
- ✅ `$env:APOTHEOSIS_ROOT` + sub-variables in place
- ✅ `$(ProjectDir)` relative paths in vcxproj

### 6.2. Remaining
| Issue | Status | Notes |
|-------|--------|-------|
| Signing cert password in clear text | ⚠️ Known | `CN=EdgeHTMLReborn` hardcoded |
| ARM32 vcvars missing in VS2022+ | ⚠️ Known | `arm32-uwp-env.ps1` workaround |
| SDK 26100 removed ARM32 libs | ⚠️ Known | Locked to SDK 19041 |
| ICU path hardcoded | ⚠️ Known | `C:\icu-arm-uwp\` not configurable |
| Stale ninja processes | ⚠️ Operational | Must kill before each build |

### 6.3. vcpkg VS UWP Detection ✅ RESOLVED

**Problem:** `vcpkg install <pkg> --triplet x64-uwp` fails with "Unable to find a valid Visual Studio instance" when using overlay triplet with explicit `VCPKG_VISUAL_STUDIO_PATH`.

**Root cause:** vcpkg's VS detection validates UWP instances by checking registry keys that fail when `VCPKG_VISUAL_STUDIO_PATH` is explicitly set.

**Resolution:** Use **community triplets** (`C:\vcpkg\triplets\community\x64-uwp.cmake`) instead of overlay triplets. No `VCPKG_VISUAL_STUDIO_PATH` — lets vcpkg auto-detect VS.

```cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME WindowsStore)
set(VCPKG_CMAKE_SYSTEM_VERSION 10.0)
```

---

## 7. Architecture Overview

### 7.1. Graphics Pipeline (CPU vs GPU)

```
WebKit (HTML/CSS → rendering tree)
        │
        ├── CPU path (default, always available)
        │   WebCore → Cairo → RGBA bitmap → WriteableBitmap (XAML)
        │
        └── GPU path (activated after WebCoreGpuInit() succeeds)
            WebCore → TextureMapper → OpenGL ES 2.0 → ANGLE → D3D11 → SwapChainPanel (XAML)
```

### 7.2. Cross-Platform: ARM32 vs x64

| Layer | ARM32 (Lumia 950) | x64 (PC debug) |
|-------|-------------------|----------------|
| **Toolchain** | clang-cl `thumbv7-unknown-windows-msvc` | clang-cl `x86_64-unknown-windows-msvc` |
| **System** | UWP App Container | UWP App Container (same sandbox) |
| **Graphics** | ANGLE → D3D11 FL 9.3 → Adreno 418 | ANGLE → D3D11 → any GPU |
| **Dependencies** | `C:\vcpkg\installed\arm-uwp\` | `C:\vcpkg\installed\x64-uwp\` |
| **ICU** | `C:\icu-arm-uwp\` | `C:\icu-x64-uwp\` |

---

## 8. Roadmap

### Phase A: Build Environment ✅ COMPLETE (June 29-30, 2026)

| # | Component | Status | Notes |
|---|-----------|--------|-------|
| 1 | LLVM/clang-cl | ✅ | 22.1.8 |
| 2 | Ninja | ✅ | WinGet + VS2022 bundled |
| 3 | WebKit source | ✅ | webkitgtk-2.52.4 |
| 4 | ANGLE x64 binaries | ✅ | NuGet → `Src\angle\x64\` |
| 5 | vcpkg x64-uwp deps (16 pkgs) | ✅ | Community triplet |
| 6 | ICU x64-uwp | ✅ | `C:\icu-x64-uwp\` |
| 7 | SQLite3 x64-uwp | ✅ | `SQLITE_OS_WINRT=1` |
| 8 | CMake configure | ✅ | `build-x64-gpu` |
| 9 | WTF compiled | ✅ | 12+ WK_WINUWP patches across 8 WTF files |
| 10 | bmalloc compiled | ✅ | getpid fix |
| 11 | PAL header generation | ✅ | 1203 header steps completed |

### Phase B: First x64 WebKit Build ⚠️ In Progress (July 1, 2026)

| Step | Component | Status | Notes |
|------|-----------|--------|-------|
| 1-4 | vcpkg/ICU/SQLite/Configure | ✅ Done | |
| 5 | WTF | ✅ Done | 12+ WK_WINUWP patches across 8 files; RunLoopWin rewritten for UWP |
| 6 | bmalloc | ✅ Done | |
| 7 | PAL (headers) | ✅ Done | Object lib — .objs built with WebCore |
| 8 | **JavaScriptCore** | 🔄 **Compiling** | WTF ✅; LowLevelInterpreter.cpp ✅ (GNU driver); MacroAssemblerX86_64.cpp ✅ (GNU driver, `-imsvc`→`-isystem`); JSC unified sources at ~8/111, only warnings |
| 9 | **WebCore** (includes PAL .objs) | ❌ | Blocked by JSC |
| 10 | Link port driver → `WebCoreDriver-x64.dll` | ❌ | |
| 11 | Build UWP test harness | ❌ | |

**Blockers discovered:**
- CMake 4.0 omits most rules from `rules.ninja` — workaround: `patch-build-ninja-gnu.ps1` auto-scanner
- `build.ninja` corruption risk on script interruption — always regenerate CMake after patch failure
- More UWP API cuts expected in JSC unified sources and WebCore

### Phase C: Upgrade & Refactor
1. Upgrade WebKit to 2.53.4
2. Consolidate stubs, reduce code duplication
3. Add CI

### Phase D: Features
1. WebGL support (already partially working via ANGLE)
2. Service Worker / PWA support
3. Video/audio playback (see `Doc/MEDIA-PLAN.md`)
4. Upstream WK_WINUWP patches

---

*Progress is measured in working DLLs. Next milestone: `bin\JavaScriptCore.dll`.*

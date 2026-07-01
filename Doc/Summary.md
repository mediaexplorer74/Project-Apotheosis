# Project Apotheosis — Engineering Research Summary

> Compiled by an AI code agent during deep-dive sessions (June 28 - July 1, 2026).
> Original repo: [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis)
> Reddit thread: [r/windowsphone — Porting WebKitGTK 2.52.4 to Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)

---

## 1. What Is This Project?

**Apotheosis** (codename "EdgeHTML Reborn") ports the modern **WebKit/WebCore** rendering engine (webkitgtk-2.52.4) to **Windows 10 Mobile on ARM32 (UWP, App Container)**, targeting the **Lumia 950** family. It brings JIT-accelerated JavaScript and GPU-composited rendering to a platform abandoned by Microsoft years ago.

**Status (real device, Lumia 950, Win10M 15254):**

| Feature | Status | Notes |
|---------|--------|-------|
| WTF + JSC CLoop | ✅ | Phase 0 — engine core runs on device |
| WebCore + Cairo SW render | ✅ | Bing, GitHub, Apple, MS sites render correctly |
| Live interactive session | ✅ | Mouse events, form input, scroll, keyboard |
| JSC JIT | ✅ | ~5-50× speedup over CLoop |
| GPU compositing (ANGLE + TextureMapper) | ✅ | Direct present to SwapChainPanel |
| Smooth scroll / pinch-zoom | ✅ | GPU-backed, real-time |
| Browser shell (tabs, URL bar, settings) | ✅ | Version 0.1.8+ |
| Multi-language UI (en/ru/cn) | ✅ | `.resw` + fallback table |
| **x64-uwp build (PC debug)** | 🆕 **In progress** | WTF/bmalloc/PAL headers compiled |

---

## 2. Source & Origin

- **GitHub:** [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis)
- **Reddit:** [r/windowsphone](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- **Author:** Jimmy Xiao (GitHub: `Jimmyxiao2009`)
- **License:** MIT (port layer); LGPL-2.1/BSD (upstream WebKit + dependencies)

The upstream WebKit source is **webkitgtk-2.52.4** (released June 2, 2026), GitHub tag: `webkitgtk-2.52.4` (commit `7acdf5e`).

---

## 3. Architecture

```
Harness — UWP App (C++/CX, MSVC v143)
   · MainPage: toolbar, gestures → engine
   · GpuPanel (SwapChainPanel) ← GPU | RenderImage ← SW fallback
        │  C ABI (WebCoreDriver.h)
WebCoreDriver (clang-cl → WebCoreDriver-gpu.dll)
   · Resident Page/Frame session, event dispatch
   · Cairo paintToRGBA | TextureMapper GPU composite
   · PortChromeClient / FrameLoaderClient / Strategies
        │
WebKit / WebCore / JSC / WTF (clang-cl, WK_WINUWP patches)
```

**Key decisions:**
- Three layers decoupled by a stable **C ABI** (`WebCoreDriver.h`)
- Engine thread: all calls serialized on single background thread
- UI thread: never synchronously waits on engine (deadlock prevention)
- Two paint paths: Cairo SW (fallback) | TextureMapper GPU (runtime switch)
- ANGLE (D3D11 FL9_3) as OpenGL ES 2.0 wrapper for UWP App Container

---

## 4. Build System

### 4.1 Three Toolchains

| Layer | Compiler | Target | Output |
|-------|----------|--------|--------|
| WTF/JSC/WebCore | **clang-cl** (LLVM 22.1.7) | `thumbv7-unknown-windows-msvc` | Static `.lib` |
| Port driver | **clang-cl** + **lld-link** | ARM32 UWP | `WebCoreDriver-gpu.dll` |
| Harness (UWP app) | **MSVC v143** (14.44.35207) | ARM | `Harness.appx` |

### 4.2 Build Configurations

| Dir | JIT | GPU (ANGLE) | Purpose |
|-----|-----|-------------|---------|
| `build-clang-webcore` | ❌ (CLoop) | ❌ (Cairo) | Phase 1b baseline |
| `build-clang-jit` | ✅ | ❌ (Cairo) | JIT line |
| `build-clang-gpu` | ✅ | ✅ | **Active dev line (gpu-path1, ARM32)** |
| `build-x64-gpu` | ✅ | ✅ | **New: PC debug (x64-uwp)** |

### 4.3 Ninja Build Target Structure

Key discovery: **PAL is an OBJECT library** — no `PAL.lib` produced. PAL `.obj` files are compiled and linked directly into `WebCore.dll`. The top-level ninja targets:

| Ninja Target | Produces | Status |
|---|---|---|
| `ninja bmalloc` | Object lib objects | ✅ Compiled |
| `ninja WTF` | Object lib objects | ✅ Compiled |
| `ninja PAL` | Headers only | ✅ Header gen complete (1203 steps) |
| `ninja JavaScriptCore` | `bin/JavaScriptCore.dll` | ❌ Not yet |
| `ninja WebCore` | `bin/WebCore.dll` (includes PAL objs) | ❌ Not yet |
| `ninja all` | Everything | ❌ |

---

## 5. Key Engineering Discoveries

### 5.1 Critical Patches (WK_WINUWP)

| Area | Changes |
|------|---------|
| Memory allocation | `VirtualAlloc` → `VirtualAllocFromApp` |
| File I/O | `CreateFileW` → `CreateFile2` |
| Crypto | `CryptGenRandom` → `BCryptGenRandom` |
| Threading | Remove SEH `__try` (clang ARM can't lower `cleanupret`) |
| Networking | Stub `DNSResolveQueuePlatform` |
| Graphics | Cairo over DirectWrite (FreeType+Fontconfig+HarfBuzz) |
| C++ exceptions | `_HAS_EXCEPTIONS=0` + `/EHs-c-` |
| mpark::variant | Replaced with `std::variant` |
| Window APIs | `SHGetValueW`/`GetWindowLongPtr`/`SetWindowLongPtr` guarded in `WindowsExtras.h` |
| Debug Help | `#include <dbghelp.h>` + `SymFromAddress` guarded in `DbgHelperWin.h/.cpp`; UWP stub returns `false` |
| Filesystem (UWP) | `SHGetFolderPathW` → `GetEnvironmentVariableW`; `CreateFileW` → `CreateFile2` in `FileSystemWin.cpp` |
| Memory unlock | `VirtualUnlock` guarded in `OSAllocatorWin.cpp` |
| Memory pressure | `CreateMemoryResourceNotification`/`QueryMemoryResourceNotification` guarded in `MemoryPressureHandlerWin.cpp` |
| Signals | `AddVectoredExceptionHandler` guarded in `SignalsWin.cpp` |
| Event loop | RunLoopWin.cpp rewritten for UWP: HWND messaging → generic condition-variable loop (`USE(GENERIC_EVENT_LOOP)`) |

### 5.2 The "Pack Expansion" Compiler Wall

clang's `thumbv7-windows-msvc` backend cannot mangle variadic pack expansions in function template signatures when using `mpark::variant`. Fix: replace `WTF::Variant` (= `mpark::variant`) with `std::variant` under `WK_WINUWP` guard. This unblocked ~80% of WebCore compilation.

### 5.3 GNU Driver for Assembly Files

LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp contain AT&T-syntax inline assembly (`asm("...%rsi...")`) that clang-cl cannot parse. Solution: compile these two files with **clang++ (GNU driver)** while everything else uses clang-cl. Two custom ninja rules (`_gnu_Release`) added to `rules.ninja`:

```
rule _gnu_Release
  command = clang++.exe --target=x86_64-unknown-windows-msvc -x c++ $DEFINES $INCLUDES $FLAGS -MD -MF $out.d -o $out -c -- $in
  deps = gcc
  depfile = $out.d
```

Key differences from clang-cl rules: `deps = gcc` (not msvc), `-MD -MF $out.d` (not `/showIncludes`), `-o $out -c -- $in` (not `/Fo$out /c $in`), no `/Fd` for PDB. The `FLAGS` must use `-D` defines (not `/D`), `-I` includes (not `/I`). The `-imsvc` paths in `INCLUDES` must be replaced with `-isystem` for the GNU rule, since clang++ doesn't understand `-imsvc`.

**Current status (x64):** Both LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp compile cleanly with the GNU driver. The `-imsvc`→`-isystem` conversion is handled by `patch-build-ninja-gnu.ps1` for both files' `INCLUDES`. The only warnings are the harmless `[[no_unique_address]]` attribute ignored by clang-cl.

### 5.4 CMake 4.0 Missing Rules Workaround

CMake 4.0's Ninja generator has a quirk: **not all compiler/linker rules are emitted into `rules.ninja`**. Fresh builds produce only ~10 rules (bmalloc, unifdef, WTF, LLInt*), leaving ~24+ rules missing — including `JavaScriptCore`, `WebCore`, `WebKit`, `jsc`, `ANGLE`, `PAL`, and all utility rules (`CLEAN`, `HELP`, `RERUN_CMAKE`).

**Solution:** `patch-build-ninja-gnu.ps1` now scans `build.ninja` for all unique rule names used in `build` statements, cross-references against `rules.ninja`, and auto-generates any missing rules with correct command templates (C/CXX compiler, executable/shared-library/static-library linker, utility rules). This is a fully generalized workaround — it handles any future CMake re-generation without hardcoded target lists.

### 5.5 x64 Build Progress

| Component | ARM32 | x64-uwp |
|-----------|-------|---------|
| vcpkg deps (16 pkgs) | ✅ | ✅ ALL INSTALLED |
| ICU 78 | ✅ Custom cross-build | ✅ Built at `C:\icu-x64-uwp\` |
| SQLite3 UWP | ❌ Not bundled | ✅ Manually built (amalgamation) |
| ANGLE | ✅ Pre-built ARM | ✅ x64 from NuGet at `Src\angle\x64\` |
| WebKit source | ✅ Same source | ✅ Same source |
| WebCore config | ✅ `build-clang-gpu` | ✅ `build-x64-gpu` configured |
| WTF build | ✅ | ✅ COMPILED (x64, clang-cl, Release; 12+ WK_WINUWP patches) |
| bmalloc build | ✅ | ✅ COMPILED |
| LLIntOffsetsExtractor | ✅ | ✅ LINKED |
| PAL build | ✅ | ✅ Header gen complete (object lib — .objs in WebCore) |
| JSC LowLevelInterpreter.cpp | ✅ | ✅ GNU driver (AT&T assembly, `deps = gcc`, `-MD -MF`) |
| JSC MacroAssemblerX86_64.cpp | ✅ | ✅ GNU driver (`-imsvc`→`-isystem` via patch script) |
| **JavaScriptCore** (overall) | ✅ (ARM) | 🔄 **Compiling** — JSC unified sources at ~8/111 steps, only warnings |
| **CMake 4.0 rules workaround** | N/A | ✅ `patch-build-ninja-gnu.ps1` auto-scans for missing rules |
| Port driver | ✅ | ❌ Not yet |
| Harness appx | ✅ | ❌ Not yet |

---

## 6. Multi-Language UI

**Status:** Complete. Three languages:

| Language | Code | ID |
|----------|------|----|
| 中文 (Chinese) | `zh-Hans` | 0 |
| English | `en-US` | 1 |
| Русский (Russian) | `ru-RU` | 2 |

**Dual-source loading:** `GetStr()` tries `.resw` first, falls back to `kStr[lang][id]` table.

---

## 7. x64 Build Infrastructure ✅

All dependencies installed and verified during June 28-30 sessions:

| Dependency | Status | Notes |
|------------|--------|-------|
| Toolchain | ✅ `Toolchain-x64-UWP-clang.cmake` | clang-cl `x86_64-unknown-windows-msvc` |
| vcpkg x64-uwp (16 pkgs) | ✅ ALL INSTALLED | Community triplet workaround |
| ICU x64-uwp | ✅ `C:\icu-x64-uwp\` | libs + DLLs |
| SQLite3 UWP | ✅ Manually built | `SQLITE_OS_WINRT=1` |
| CMake configure | ✅ FIRST SUCCESS (June 29) | |
| WTF compiled | ✅ | 12+ WK_WINUWP patches applied |
| bmalloc compiled | ✅ | getpid→GetCurrentProcessId |
| PAL headers | ✅ | 1203 ninja steps completed |

**Build environment quirks:**
- C++23 confirmed: `build.ninja` emits `-clang:-std=c++23`
- Perl must be on PATH for Python codegen scripts
- `ninja` locks: always delete `.ninja_lock` + `.ninja_log` after interrupted builds
- Long builds must use `[System.Diagnostics.Process]::Start()` — tool's bash wrapper kills subprocesses

---

## 8. Repository Layout

```
Apotheosis\
├── WebKit\               ← webkitgtk-2.52.4 (gitignored)
├── build-x64-gpu\        ← x64 build output (gitignored)
├── Src\
│   ├── port\             ← Port layer + build scripts (tracked)
│   │   ├── WebCoreDriver.{cpp,h}
│   │   ├── PortChromeClient.{h,cpp}
│   │   ├── LoadingFrameLoaderClient.{h,cpp}
│   │   ├── stubs-*.cpp
│   │   ├── Toolchain-*.cmake (ARM32 + x64)
│   │   ├── configure-gpu*.ps1 / link-driver-gpu*.ps1
│   │   ├── compile-driver-gpu*.ps1
│   │   └── build-harness.ps1
│   ├── harness\           ← UWP app (C++/CX, XAML)
│   │   └── Resources/{en-US,zh-Hans,ru-RU}/Resources.resw
│   ├── tools\             ← Deploy/diagnostic scripts
│   ├── angle\include\     ← ANGLE headers (tracked)
│   └── setenv.ps1         ← Environment setup
├── Doc\                   ← Documentation (tracked)
│   ├── PLAN.md            ← Development plan
│   ├── Summary.md         ← This file
│   ├── WIKI_EN.md         ← English wiki
│   ├── WIKI_RU.md         ← Russian wiki
│   ├── WIKI_CN.md         ← Chinese wiki
│   ├── HANDOFF.md         ← Phase 0 handoff
│   ├── M2-HANDOFF.md      ← GPU rendering details
│   ├── MEDIA-PLAN.md      ← Video/audio roadmap
│   ├── WEBKIT-UPGRADE.md  ← 2.52.4→2.53.4 analysis
│   └── MORNING-STATUS*, NIGHT-LOG*
├── AGENTS.md              ← Codex guidance
├── README.md / _CN.md / _RU.md
```

---

## 9. Recommended Next Steps

### Short-term — Complete x64 Build
1. **Complete JavaScriptCore**: `ninja -C build-x64-gpu JavaScriptCore -j2` → `bin/JavaScriptCore.dll` (currently compiling, ~8/111 steps)
2. **Fix UWP API errors in JSC**: As unified sources compile, remaining UWP-incompatible APIs will surface — patch with `#if !defined(WK_WINUWP)` gates on-the-fly
3. **Build PAL (objects)**: PAL `.obj` files compile as part of `WebCore.dll` — `ninja WebCore` covers it
4. **Build WebCore**: `ninja -C build-x64-gpu WebCore -j2` → `bin/WebCore.dll` (includes PAL .objs). Many UWP patches needed for WebCore sources
5. **Link port driver**: `pwsh Src/port/link-driver-gpu-x64.ps1` → `WebCoreDriver-x64.dll`
6. **Build+run harness**: x64 UWP appx for local debugging

### Medium-term
1. Upgrade to webkitgtk-2.53.4
2. Refactor port layer — consolidate stubs, reduce duplication
3. Add CI

### Long-term
1. Upstream WK_WINUWP patches to WebKit
2. WebGL, Service Worker, PWA support
3. Video/audio playback

---

*"Reviving the Windows Phone web for the people who never let it die."*

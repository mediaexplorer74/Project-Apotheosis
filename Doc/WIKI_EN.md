# Wiki: Project Apotheosis / EdgeHTML Reborn

> What, why, and how we're porting WebKit to Windows 10 Mobile.

---

## 1. What Is WebKit?

**WebKit** is the browser engine that powers Safari (Apple) and used to power Chrome (before the Blink fork). It handles:

- **HTML/CSS** — lays out and styles web pages
- **JavaScript (JSC)** — executes scripts (JSC = JavaScriptCore)
- **2D/3D graphics** — renders via CPU (Cairo) or GPU (OpenGL/ANGLE)
- **Networking** — HTTP/HTTPS via curl, TLS 1.3
- **Video/audio** — media elements (not yet implemented)

**Code size:** ~3 million lines of C++. A massive project developed by Apple, Google, Samsung, and others.

---

## 2. What Does "Porting" WebKit Mean?

WebKit works out of the box on macOS, iOS, Linux (GTK), and old Windows. It does **not** work on:

- Windows 10 Mobile (ARM32, UWP, App Container)
- Lumia 950 and other Windows Phone devices

**Porting** = take the WebKit codebase, add `#if defined(WK_WINUWP)` guards at the right places, point it at Windows SDK headers, compile with ARM32 clang-cl, and feed it to the UWP runtime.

---

## 3. GPU — Does the Lumia 950 Have One?

**Yes.** The Lumia 950 runs on a Qualcomm Snapdragon 810:

| Lumia 950 | Spec |
|-----------|------|
| **GPU** | **Adreno 418** (Direct3D 11 FL 9.3) |
| CPU | 4× Cortex-A57 + 4× Cortex-A53 |

**Adreno** is the same class of GPU found in millions of Android phones. It supports 3D graphics via Direct3D (DXGI).

**GPU rendering vs CPU rendering:**

| Mode | What it does | When active |
|------|-------------|-------------|
| **CPU (Cairo)** | Calculates every pixel on the CPU | Always, as fallback |
| **GPU (TextureMapper+ANGLE)** | Sends commands to the GPU to render/scroll/zoom | After `WebCoreGpuInit()` succeeds |

**Why GPU?** CPU rendering recalculates every pixel on scroll — slow. GPU creates a "texture" (image) and just moves it — 10-100× faster. Smooth scrolling, zoom, animations all require the GPU.

**Pipeline on Lumia:**

```
WebKit (HTML → layers)
    ↓
TextureMapper (WebCore, splits into textures)
    ↓
ANGLE (OpenGL ES → Direct3D 11 FL 9.3 translation layer)
    ↓
Adreno 418 GPU (hardware acceleration)
    ↓
SwapChainPanel (XAML control, displays on screen)
```

ANGLE is needed because WebKit speaks OpenGL ES internally, but Lumia's GPU only understands Direct3D. ANGLE translates.

---

## 4. WK_WINUWP — What Are These Patches?

`WK_WINUWP` is a flag (`#define WK_WINUWP 1`) that **we invented** and inserted into WebKit:

```cpp
#if defined(WK_WINUWP)
    // UWP-specific code
#else
    // normal Windows/Linux code
#endif
```

**What the patches do:**

| # | Change | Why |
|---|--------|-----|
| 1 | `WINAPI_FAMILY=WINAPI_FAMILY_APP` | App Container blocks filesystem, registry, process access |
| 2 | `_HAS_EXCEPTIONS=0` | clang-cl ARM can't lower `cleanupret` for C++ exceptions |
| 3 | Remove `dbghelp.dll`, `winmm.dll` | These DLLs don't exist in App Container |
| 4 | `recv/send` → WinSock | UWP networking uses WinSock, not Berkeley sockets |
| 5 | Remove registry, disk access | No registry or `C:\` in App Container |
| 6 | Link `WindowsApp.lib` | Instead of `kernel32.lib`, `user32.lib` |
| 7 | JIT for ARM32 | JSC generates machine code directly on device |
| 8 | `StackReserveSize=16MB` | JSC CLoop interpreter needs extra stack space |
| 9 | `WindowsExtras.h` — guard `SHGetValueW`/`GetWindowLongPtr`/`SetWindowLongPtr` | UWP has no window handles or registry |
| 10 | `DbgHelperWin` — guard `#include <dbghelp.h>`, stub out `SymFromAddress` | `dbghelp.dll` not available in App Container |
| 11 | `FileSystemWin.cpp` — `SHGetFolderPathW` → `GetEnvironmentVariableW`; `CreateFileW` → `CreateFile2` | UWP has no `shlobj.h`; `CreateFile2` is the UWP-compatible API |
| 12 | `OSAllocatorWin.cpp` — guard `VirtualUnlock` | `VirtualUnlock` not available in App Container |
| 13 | `MemoryPressureHandlerWin.cpp` — guard memory resource notification APIs | App Container blocks these |
| 14 | `SignalsWin.cpp` — guard `AddVectoredExceptionHandler` | Desktop-only API |
| 15 | `RunLoopWin.cpp` — rewrite for UWP: HWND messaging → generic condition-variable loop | UWP has no `GetMessage`/`DispatchMessage`; `USE(GENERIC_EVENT_LOOP)` replaces `USE(WINDOWS_EVENT_LOOP)` |
| 16 | GNU driver for LowLevelInterpreter.cpp + MacroAssemblerX86_64.cpp | These contain AT&T-syntax inline assembly that only clang++ (GNU driver) can parse |

The exact patch locations are tracked in the AI agent's project memory.

---

## 5. Three Build Configurations

| Build dir | JIT | GPU (ANGLE) | Purpose |
|-----------|-----|-------------|---------|
| `build-clang-webcore` | No (CLoop) | No (Cairo) | Baseline — works, slow but reliable |
| `build-clang-jit` | **Yes** | No (Cairo) | Fast JS, software graphics |
| `build-clang-gpu` | **Yes** | **Yes** | **Maximum**: JIT + GPU acceleration. The main dev target |
| `build-x64-gpu` | **Yes** | **Yes** | Same but x64-uwp for PC debugging (new) |

Switched via CMake flags: `-DENABLE_JIT=ON/OFF` and `-DAPOTHEOSIS_GPU=ON`.

---

## 6. ARM32 vs x64 Cross-Platform

| Layer | ARM32 (Lumia 950) | x64 (PC debug) |
|-------|-------------------|----------------|
| **Toolchain** | clang-cl `thumbv7-unknown-windows-msvc` | clang-cl `x86_64-unknown-windows-msvc` |
| **System** | UWP App Container | UWP App Container (same) |
| **Graphics** | ANGLE → D3D11 FL 9.3 → Adreno | ANGLE → D3D11 → any GPU |
| **Libraries** | vcpkg `arm-uwp` | vcpkg `x64-uwp` |
| **ICU** | `C:\icu-arm-uwp` | `C:\icu-x64-uwp` |

Our `setenv.ps1` and `Harness.vcxproj` can switch path sets based on `APOTHEOSIS_ARCH`.

---

## 7. Build Process (Step by Step)

```
WebKit Source (webkitgtk-2.52.4 + WK_WINUWP patches)
    ↓
CMake + Ninja + clang-cl + Toolchain-*.cmake
    ↓
WebCore.dll + JavaScriptCore.dll + WTF.obj + PAL.obj
    ↓
link-driver-gpu.ps1 (lld-link)
    ↓
WebCoreDriver-gpu.dll (our C ABI driver)
    ↓
MSBuild + Harness.vcxproj (v143 ARM/x64)
    ↓
Harness.appx  ← READY!
    ↓
deploy-launch.ps1 → Lumia 950
```

**Note:** PAL is an OBJECT library — no `PAL.lib` file. PAL `.obj` files are compiled and linked directly into `WebCore.dll`.

Available ninja targets:

| Target | Produces | Status |
|--------|----------|--------|
| `ninja WTF` | Object lib objects | ✅ Compiled |
| `ninja bmalloc` | Object lib objects | ✅ Compiled |
| `ninja PAL` | Headers only | ✅ Header gen complete |
| `ninja JavaScriptCore` | `bin/JavaScriptCore.dll` | 🔄 Next |
| `ninja WebCore` | `bin/WebCore.dll` | ❌ |

---

## 8. What Already Works on Lumia 950

- ✅ WTF + JSC CLoop (basic JS engine)
- ✅ WebCore + Cairo (rendering Bing, GitHub, Apple, real sites)
- ✅ Clicks, forms, links, scrolling, keyboard input
- ✅ JSC JIT (fast JavaScript)
- ✅ ANGLE + TextureMapper (GPU-composited rendering)
- ✅ Smooth scrolling + pinch-to-zoom
- ✅ Multi-language UI (EN / ZH / RU)

---

## 9. What Remains

- **Complete x64-uwp build** for PC debugging (JavaScriptCore → WebCore → driver → appx)
- **Fix UWP compilation errors** in JSC/WebCore as they appear (each new target reveals WK_WINUWP patch sites)
- **Video/audio playback** (planned — see `MEDIA-PLAN.md`)
- **WebGL** (partially working via ANGLE)
- **Service Workers / PWA**
- **Upgrade to webkitgtk-2.53.4** (low-medium effort)
- **Upstream WK_WINUWP patches** to official WebKit

---

## 10. x64 Build Status (July 1, 2026)

| Step | Status | Notes |
|------|--------|-------|
| LLVM/clang-cl 22.1.8 | ✅ Installed | |
| Ninja | ✅ | Via VS2022 |
| WebKit source (webkitgtk-2.52.4) | ✅ | |
| ANGLE x64 binaries | ✅ | NuGet → `Src\angle\x64\` |
| vcpkg x64-uwp deps (16 pkgs) | ✅ ALL INSTALLED | Community triplet |
| ICU x64-uwp | ✅ | `C:\icu-x64-uwp\` |
| SQLite3 x64-uwp | ✅ | Manually built |
| CMake configure | ✅ | `build-x64-gpu` |
| WTF compiled | ✅ | C++23, clang-cl; 12+ WK_WINUWP patches across 8 files |
| bmalloc compiled | ✅ | getpid→GetCurrentProcessId |
| PAL headers generated | ✅ | 1203 ninja steps |
| JavaScriptCore (WTF) | ✅ | All ~370 WTF .cpp files compile |
| JavaScriptCore (LowLevelInterpreter) | ✅ | Compiled with clang++ GNU driver (AT&T assembly) |
| JavaScriptCore (MacroAssemblerX86_64) | 🔴 | Built with GNU driver — `-imsvc` flag blocks it |
| JavaScriptCore (unified sources) | 🔄 | Compiling with clang-cl |
| WebCore | ❌ | Blocked by JSC |
| Port driver | ❌ | |
| Harness appx | ❌ | |

### GNU driver approach

LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp contain AT&T-syntax inline assembly that **only clang++ (GNU driver)** can parse. Two custom ninja rules (`_gnu_Release`) use `clang++.exe --target=x86_64-unknown-windows-msvc` with `deps = gcc`, `-MD -MF $out.d`, `-o $out -c -- $in`. The `-imsvc` flag that clang-cl accepts must be replaced with `-isystem` in `INCLUDES` for these rules since clang++ rejects `-imsvc`.

### Key technical findings

1. **PAL is an OBJECT library**: No `PAL.lib` produced. PAL's `.obj` files link directly into WebCore.
2. **C++23 confirmed**: `build.ninja` uses `-clang:-std=c++23`, all files compile clean.
3. **Ninja lock discipline**: Always delete `.ninja_lock` after interrupted builds.
4. **Build launch**: Long builds need `[System.Diagnostics.Process]::Start()` — the tool's bash wrapper kills subprocesses on timeout.
5. **Stale processes**: Zombie `clang-cl.exe` instances can stall indefinitely. Kill before each build.

---

*Written for the retro Windows Phone fans who won't let the platform die.  
The Lumia 950 is not a dead brick. It's a tiny computer with an Adreno GPU that just needed a proper browser.*

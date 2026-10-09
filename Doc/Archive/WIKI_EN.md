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
| `build-arm32-webcore` | No (CLoop) | No (Cairo) | Baseline — works, slow but reliable |
| `build-arm32-jit` | **Yes** | No (Cairo) | Fast JS, software graphics |
| `build-arm32-gpu` | **Yes** | **Yes** | **Maximum**: JIT + GPU acceleration. The main dev target |
| `build-x64-gpu` | **Yes (+FTL_JIT)** | **No** (Cairo-only) | PC debug (x64-uwp) — GPU path TBD |

Switched via CMake flags: `-DENABLE_JIT=ON/OFF` and `-DAPOTHEOSIS_GPU=ON`.

---

## 6. ARM32 vs x64 Cross-Platform

| Layer | ARM32 (Lumia 950) | x64 (PC debug) |
|-------|-------------------|----------------|
| **Toolchain** | clang-cl `thumbv7-unknown-windows-msvc` | clang-cl `x86_64-unknown-windows-msvc` |
| **System** | UWP App Container | UWP App Container (same) |
| **JIT** | JSC JIT | JSC JIT + **FTL_JIT** |
| **Graphics** | ANGLE → D3D11 FL 9.3 → Adreno | **Cairo SW only** (GPU path not yet configured for x64) |
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
link-driver-gpu-arm32.ps1 (lld-link)
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

- **Fix `CSSValueAggregates.h` blocker** to finish WebCore compilation (x64)
- **Complete x64-uwp build** for PC debugging (WebCore → driver → appx)
- **Fix UWP compilation errors** in WebCore as they appear
- **Video/audio playback** (planned — see `MEDIA-PLAN.md`)
- **WebGL** (partially working via ANGLE)
- **Service Workers / PWA**
- **Upgrade to webkitgtk-2.53.4** (low-medium effort)
- **Upstream WK_WINUWP patches** to official WebKit

---

## 10. x64 Build Status (July 10, 2026)

| Step | Status | Notes |
|------|--------|-------|
| LLVM/clang-cl 22.1.8 | ✅ Installed | |
| Ninja | ✅ | Via VS2022 |
| WebKit source (webkitgtk-2.52.4) | ✅ | |
| ANGLE x64 binaries | ✅ | NuGet → `Src\angle\x64\` |
| vcpkg x64-uwp deps (16 pkgs) | ✅ ALL INSTALLED | Community triplet |
| ICU x64-uwp | ✅ | `C:\icu-x64-uwp\` |
| SQLite3 x64-uwp | ✅ | Manually built |
| CMake configure | ✅ | `build-x64-gpu` (CMake 4.3, Ninja generator) |
| WTF compiled | ✅ | C++23, clang-cl; 12+ WK_WINUWP patches |
| bmalloc compiled | ✅ | getpid→GetCurrentProcessId |
| PAL headers generated | ✅ | 1203 ninja steps |
| **JavaScriptCore** | ✅ **FTL_JIT ON** | `JavaScriptCore.dll` 18.8 MB; all unified sources + two GNU-driver AT&T-asm files |
| **WebCore** | 🔄 **All 156 source files compiled** | Linker blocked: 20 unresolved externals (port stubs) |
| Port driver | ❌ | |
| Harness appx | ❌ | |

### GNU driver approach

LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp contain AT&T-syntax inline assembly that **only clang++ (GNU driver)** can parse. Two custom ninja rules (`_gnu_Release`) use `clang++.exe --target=x86_64-unknown-windows-msvc` with `deps = gcc`, `-MD -MF $out.d`, `-o $out -c -- $in`. The `-imsvc` flag that clang-cl accepts must be replaced with `-isystem` in `INCLUDES` for these rules since clang++ rejects `-imsvc`.

Both files **compile cleanly** with the GNU driver as of July 3. The `-imsvc`→`-isystem` conversion is handled by `patch-build-ninja-gnu.ps1`.

### Key technical findings

1. **PAL is an OBJECT library**: No `PAL.lib` produced. PAL's `.obj` files link directly into WebCore.
2. **C++23 confirmed**: `build.ninja` uses `-clang:-std=c++23`, all files compile clean.
3. **Ninja lock discipline**: Always delete `.ninja_lock` after interrupted builds.
4. **Build launch**: Long builds need `[System.Diagnostics.Process]::Start()` — the tool's bash wrapper kills subprocesses on timeout.
5. **Stale processes**: Zombie `clang-cl.exe` instances can stall indefinitely. Kill before each build.

---

*Written for the retro Windows Phone fans who won't let the platform die.  
The Lumia 950 is not a dead brick. It's a tiny computer with an Adreno GPU that just needed a proper browser.*

## 11. The layers, the sandbox, and where the mines are

Written 2026-08-22 for someone about to touch this tree for the first time. Everything below was paid
for at least once.

### 11.1. WTF is not part of WebCore

Four things get built, in this order, and each depends only on the ones before it:

```
WTF  →  JavaScriptCore  →  WebCore  →  the port layer (Src/port) + the harness (Src/harness)
```

`WTF` ("Web Template Framework", `Source/WTF`) is the bottom: strings, containers, threads, time,
allocators. It is **not** inside WebCore, and confusing the two leads to rebuilding the wrong thing for
hours. You can see the separation in the build output: `WTF.lib`, `JavaScriptCore.dll`, `WebCore.dll`
are three distinct artefacts.

When someone here says "build all three targets", they mean `ninja WTF JavaScriptCore WebCore` — three
ninja targets in one build directory. It has nothing to do with three platforms. **There are exactly
two platforms in this project: x64-UWP and ARM32-UWP.** No Linux, no GTK, ever.

### 11.2. Then why is the source tarball called webkitgtk?

Because `webkitgtk-2.52.4` is a release snapshot of the **whole** WebKit tree, every port included —
GTK, WPE, WinCairo, PlayStation, Apple's. The name says which port that release was cut for, not what
the archive contains. We build a port that is **not** in there.

This is the single most useful thing to understand about the work: WebKit's cross-platform design puts
the portability in the **interface**, not in the implementation. `wtf/MemoryFootprint.h` declares one
function; beside it sit `MemoryFootprintLinux.cpp` (reads `/proc/self/smaps`), `MemoryFootprintWin.cpp`
(calls `QueryWorkingSet`), `MemoryFootprintCocoa.mm` (`task_info`). CMake picks one by platform. So
"WebKit is portable" is true and does not mean any given file will compile for you.

We build on Windows, so the `win/` implementations get selected — and those were written for **desktop
Win32**, for the WinCairo and WebKitLegacy ports. Every place one of them assumes a desktop process is
a place this fork has to adapt. That is not a defect in WebKit's portability; it is the cost of adding
a platform upstream does not have.

### 11.3. An App Container is a smaller Win32, not a restricted one

`WINAPI_FAMILY=WINAPI_FAMILY_APP` is not a permission model. It is a set of `#if` gates in the SDK
headers that **delete API surface at compile time**. So the failure mode is not "access denied at
runtime" — it is:

```
MemoryFootprintWin.cpp(52,51): error: unknown type name 'PSAPI_WORKING_SET_INFORMATION'
```

`OpenProcess`, `QueryWorkingSet`, `LoadLibraryA`, `GetModuleHandleW`, `CertOpenSystemStoreW` — all
present in the SDK, all outside the partition. Which ones exactly also depends on the SDK version,
which is why this project pins one (see CLAUDE.md).

The visible consequence is `Src/port/stubs-*.cpp`: ten files, about two thousand lines, covering crypto,
network, pasteboard, accessibility, the loader and GDI. Each line exists because some function turned
out to be on the other side of that boundary.

### 11.4. The mine map

These are not hypotheticals. Each one has cost this project real hours, and none of them announces
itself before you step on it.

**A build directory is a hand-carried artefact.** `build-arm32-gpu/build.ninja` is *older than the CMake
sources it was generated from*, and the configuration it holds is the only one known to produce an
engine that runs on the phone. Editing any `WebKit/Source/cmake/*.cmake` file makes ninja regenerate it
— and the regenerated configuration links shared Cairo (a DLL that cannot load on Windows 10 Mobile),
drags in `user32/gdi32/msimg32` that collide with our own GDI stubs, and drops the bindings stamp so any
target becomes a 1365-task rebuild. Copy `build.ninja` and `cmakeconfig.h` aside **before** touching
CMake. Cost when learned: one full ARM WebCore rebuild.

**A file that has not been rebuilt is not a file that builds.** `wtf/win/MemoryFootprintWin.cpp` joined
WTF's source list around 2026-08-18 and had never once been compiled for ARM32 — only WebCore was ever
rebuilt afterwards, so nobody found out for four days. It cannot compile in an App Container at all.
The same shape hit the x64 line: deleting `CMakeCache.txt` — the ordinary way to make CMake re-search
for a moved dependency — revealed that Ruby was not on `PATH`, that `sqlite3.h` existed only inside an
unrelated ICU folder, and that the harfbuzz include directory had never been wired for x64.

**"Found" is not "reachable".** CMake reported `Found HarfBuzz ... version 14.3.0` and the compiler then
said `'hb-icu.h' file not found`, because in this port the include directory arrives via Cairo's
interface and that wiring existed for one architecture only.

**A missing DLL looks like a crash in your code.** If LocalState comes back holding only
`settings.ini` — not even `stage-1-main.txt` — the app died in the loader before `main`. That is always
an unpackaged dependency, never a bug in the harness. It happened the moment `USE_HARFBUZZ` turned on
and `WebCore.dll` gained an import of `harfbuzz-icu.dll` that the package did not ship.

**The bench and the device are not the same experiment.** Five build-level divergences are catalogued in
`Doc/HARFBUZZ-ICU-DIVERGENCE.md`, and two more are in the *tooling*: `x64-cycle.ps1` seeds
`gpudefault=0`, so every "the bench survives this" result was obtained with the GPU path switched off;
and the bench is driven by scripts, so it never generated the gesture that turned out to be the trigger
— one tap on the phone produced a Click **and** an Enter `KeyDown` 28 ms apart and loaded every page
twice. A scripted destination is not a scripted gesture.

**When the trace stops, the process may still be alive.** Comparing the last log line against
`heartbeat.txt` showed the app living another 4 to 7 seconds after the engine thread went quiet. Three
days of hunting memory corruption were spent because "the trace ends here" was read as "it crashed
here". It was a hang. Check the heartbeat gap before theorising about a crash site.

### 11.5. If you only remember four things

1. Back up `build.ninja` and `cmakeconfig.h` before editing anything under `WebKit/Source/cmake/`.
2. `ninja -j1`, always — 4 GB of RAM and unified translation units over 1 GB each.
3. Every upstream edit gets `#if defined(WK_WINUWP)` and an `Apotheosis:` comment, so the next person
   can find it.
4. Verify on the device, and when the device disagrees with the bench, suspect the platform difference
   before suspecting the port — but first check that the bench was actually in the device's
   configuration.

### 11.6. Three event loops, none of them ours

The hardest defects in this port live where event loops meet, so it is worth knowing how many there are.

On a desktop Windows port (WinCairo) there is one loop: the ordinary window message loop. On the GTK
port there is one: GLib's main context, native to the platform. **This port has three, stacked, and owns
only the middle one:**

1. **The XAML `CoreDispatcher`** on the UI thread. Not ours — the framework owns it. Blocking it freezes
   the application, so WebCore cannot run on it.
2. **`WebEngine`'s job queue** (`Src/harness/MainPage.xaml.cpp`), one dedicated thread draining a FIFO.
   This exists precisely because of (1), and it is why the iron rule "every C ABI call runs on the one
   engine thread" exists. Each job now carries a label, which is how a wedged one gets named.
3. **WTF's generic `RunLoop`**, running *inside* jobs from (2). The build sets
   `USE_GENERIC_EVENT_LOOP=1` deliberately: the GTK port would use GLib here, and we do not want GLib.
   This is why the code says `RunLoop::cycle()` and not `g_main_context_iteration`.

Two consequences that have both bitten:

- **A wedge in loop 3 looks like a dead application, not a dead loop.** The engine thread stops
  answering while the UI timer keeps beating, so the app lives on for seconds and is then killed by a
  timeout. See `Doc/PUMPLOOP-SILENT-DEATH.md`.
- **Waiting across loops 1 and 2 deadlocks.** ANGLE marshals surface create/resize back to the panel
  dispatcher — loop 1 — so if the engine thread synchronously waits for the UI thread while the UI thread
  waits for the engine, neither moves, and the timeout in ANGLE's helper calls `std::terminate`. Under
  `_HAS_EXCEPTIONS=0` that is an immediate `abort`: no exception, no dump. Hence the rule in CLAUDE.md
  that present happens only on the engine thread and the UI thread never waits on it.

If you are new here and looking for one place where this port is genuinely harder than a desktop
Windows port, it is this. Not the ARM32 instruction set, not the compiler, not the sandbox: the fact
that there is no native event loop we are allowed to own.

# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## What is this

**Project Apotheosis/EdgeHTML Reborn** -- Ported modern WebKit/WebCore (webkitgtk-2.52.4) to **Windows 10 Mobile/Lumia 950 (ARM32, UWP, App Container)**, allowing Windows Phone abandoned by Microsoft to run real modern Web pages, with JIT and GPU synthesis.It is the browser engine component of the "resurrecting the Win10M Ecosystem" plan.

The real machine (Lumia 950, Win10M 15254) has been verified to run through: WTF+JSC CLoop → WebCore +Cairo soft rendering (Bing/GitHub/Apple and other real sites) → real event interaction →JSC JIT → ANGLE (D3D11 FL9_3)+ TextureMapper GPU synthesis Straight rendering to SwapChainPanel → Smooth scrolling + pinch zoom.The UI is currently evolving to the Safari/Edge form.

## Key constraints (read first, you must step on the pit if you violate it)

- **Пути**: все скрипты используют `$env:APOTHEOSIS_ROOT` (устанавливается `Src\setenv.ps1`). Старые жёсткие `E:\Apotheosis\` исправлены на относительные через эту переменную. Репозиторий может быть в любом ASCII-пути без пробелов.
- **Три тулчейна, не смешивать**:
  - WTF/JSC/WebCore = **clang-cl** (`--target=thumbv7-unknown-windows-msvc` для ARM, `x86_64-unknown-windows-msvc` для x64)
  - `port/*.cpp` = clang-cl, линковка **lld-link**
  - harness (C++/CX UWP) = **MSVC v143 (14.44.35207)**. Для ARM32: `arm32-uwp-env.ps1` настраивает INCLUDE/LIB из SDK 19041.
-**C++ exception must be closed**: clang's thumbv7-windows-msvc backend cannot lower`cleanupret` (Windows exception expansion)→'_HAS_EXCEPTIONS=0'+`/EHs-c-`.
-**All upstream WebKit changes are annotated with `#if defined(WK_WINUWP)` guard +`Apotheosis:`**, which only affects this port and does not pollute upstream semantics.
-**Software rendering is a universal base, and the GPU is switched when it is running**: The synthesis switch is strictly set at 'G_GPUACTIVE` (the default is false, only `WebCoreGpuInit' is successfully set to true).The GPU does not start back to Cairo soft rendering + EmptyChromeClient (zero regression).Once the synthesis was turned on unconditionally, the real machine flashed back silently (`__fastfail`, no dump).

## Warehouse layout / What is being tracked

The warehouse**only tracks the porting layer and the host**, excluding GB-level upstream and rewritable binaries：

-'src/port/`-- WebCore driver + Port layer client + each stub + build/link script.Core source code:
  - `WebCoreDriver.cpp/.h` — C ABI драйвера
  - `PortChromeClient.h/.cpp` — ChromeClient для GPU
  - `LoadingFrameLoaderClient.h/.cpp` — FrameLoaderClient
  - `stubs-*.cpp` — платформенные заглушки
  - `Toolchain-*.cmake` — тулчейны ARM32/x64
  - `configure-*.ps1` / `link-*.ps1` — скрипты сборки
  - `vcpkg-triplets/` — триплеты arm-uwp / x64-uwp
  - (прочие `repro_*` / `mangle-repro*` / `_*.bat` удалены)
-'src/harness/`--UWP host App (C++/CX, XAML,'Package.appxmanifest`, signing certificate`.cer`/`.pfx`).
-'src/tools/`--Device Portal (WDP) Remote deployment / Catch crash dump / Automatic diagnostic script.
-`angle/include`--ANGLE header (tracking);`angle/arm',`angle-windowsstore', gitignore (can be downloaded again).
-**Not in the warehouse**: `WebKit/` (sparse webkitgtk-2.52.4, GB level, gitignore; the upstream ARM32/App-Container patch list is recorded in the **project memory** instead of the warehouse),`build-arm32-*/``build-release/``deps-build/` (build output), font,`*.pfx`,`*.log`.

**Core porting layer source code** (see'src/port/`)：

- `WebCoreDriver.cpp` / `WebCoreDriver.h`--The engine's external C ABI + resident Page session (navigation, real event distribution, link extraction, soft/hard presentation).
- `PortChromeClient.{h, cpp}`--non-final`ChromeClient' subclasses (EmptyChromeClient synthesis hook is`final` and cannot be overwritten), open GPU synthesis, capture the root'GraphicsLayer`,'triggerrenderingupdate' and setneedsPresent.
- `LoadingFrameLoaderClient.{h,cpp}`--`FrameLoaderClient` of the true policy callback (not Empty,`PolicyAction::Use`).
-'portplatformstrategies` / 'portnetworkstoragesession`--Install LoaderStrategy, network storage session.
- `stubs-*.cpp`-The platform does not implement the symbol stub (crypto/network/pasteboard/ax/loader/other).
- `Toolchain-ARM32-UWP-clang.cmake` / `arm32-uwp-env.ps1` / `clang-cl-arm-shim.h`-tool chain and environment.

## Architecture (large picture, you have to read multiple files to see clearly)

Three layers decoupled by C ABI：

```
Harness (C++/CX UWP, MSVC v143)         Src/harness/
  · Main Page: Address bar/toolbar + touch gestures → Engine scroll/ click/zoom/select
  · GpuPanel (SwapChainPanel) ← GPU Direct Rendering |RenderImage (WriteableBitmap) ← Software fallback
        │  C ABI = WebCoreDriver.h  (extern "C")
WebCoreDriver (Src/port/, clang-cl → WebCoreDriver-gpu.lib)
  ·Resident Page/Frame/Framview session, real event distribution, link extraction
  ·Two rendering paths: Cairo paintToRGBA (software) | TextureMapper → ANGLE swap chain (GPU)
  · PortChromeClient / LoadingFrameLoaderClient / Port*Strategies
        │
WebCore / JavaScriptCore / WTF (clang-cl, thumbv7-windows-msvc, App Container)
  · All ARM32/ App Container patches are guarded by WK_WINUWP; the upstream source is not in this warehouse
```

-**Thread iron law**: present** is only in the engine thread**; UI thread** will never synchronize the wait engine** (ANGLE returns the surface create/resize marshal to the panel dispatcher, mutual waiting=deadlock,'runonuithread' timeout will be'std::terminate`).All C ABI calls are serialized in a single engine thread.
-**C ABI has two copies**:'port/WebCoreDriver.h` and' harness/WebCoreDriver.h`.When adding/changing the export** The two copies must be synchronized**, otherwise the ABI is inconsistent.
-**GPU synthesis recipe** (`WebCoreComposite`, mirroring WebKit's `WCScene::update`):`flushCompositingStateIncludingSubframes`→`updateBackingStoreIncludingSubLayers`→`applyAnimationsRecursively`→`beginPainting`/`paint`/`endPainting`→`eglSwapBuffers` (direct rendering) or` glReadPixels` (off-screen readback verification).Root layer='portchromeclient::rootLayer()`, which is actually synchronizing'GraphicsLayerTextureMapper` (`USE_COORDINATED_GRAPHICS=0').

The engine has **three build configurations** (the same WebKit source with WK_WINUWP patch, different CMake switches)：

| Build directory | Configuration | Purpose |
|---|---|---|
|`build-arm32-webcore` /Cairo soft rendering |Phase 1b baseline |
|`build-arm32-jit` / +JSC JIT | JIT line |
|`build-arm32-gpu` / + GPU (TextureMapper + ANGLE) + JIT | ** Current development line ** |

Branch: 'gpu-path1' = current development line (corresponding to `build-arm32-gpu`); 'master' = pure JIT archive line.

## Common commands (Windows 7 / pwsh)

Перед сборкой: `. .\Src\setenv.ps1` (устанавливает `$env:APOTHEOSIS_ROOT`).

Изменив `port/*.cpp`, пересобрать + перелинковать GPU-драйвер:

```powershell
pwsh -File $env:APOTHEOSIS_ROOT\Src\port\link-driver-gpu-arm32.ps1
```

Компиляция одного файла для быстрой проверки ошибок:

```powershell
pwsh -File $env:APOTHEOSIS_ROOT\Src\port\compile-driver-gpu-arm32.ps1 $env:APOTHEOSIS_ROOT\Src\port\WebCoreDriver.cpp $env:APOTHEOSIS_ROOT\Src\port\WebCoreDriver.gpu.obj
```

После изменения ядра WebCore (WK_WINUWP-патчи) — инкрементальная пересборка:

```powershell
. $env:APOTHEOSIS_ROOT\Src\port\arm32-uwp-env.ps1
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C $env:APOTHEOSIS_ROOT\build-arm32-gpu WebCore
pwsh -File $env:APOTHEOSIS_ROOT\Src\port\link-driver-gpu-arm32.ps1
```

Build harness appx (x64, single msbuild invocation):

```powershell
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true /p:MinimalTest=true
# Appx is in Src/harness/AppPackages/Harness/Harness_<ver>_x64_Test/ (or x64/Release/Harness/Harness_<ver>_x64_Test/)
```

For the full build (with engine DLLs + XAML UI):

```powershell
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true
```

Deploy to the real machine and start (interactive test, no polling)：

```powershell
pwsh -File $env:APOTHEOSIS_ROOT\Src\tools\deploy-launch.ps1 -Ip <device IP> -Ver <version number>
```

Fully automatic diagnostic loop (unload→install→start→ pull the dump/BMP screenshot of`LocalState`; only if the device has'autodiag.Triggered when txt`)：

```powershell
pwsh -File $env:APOTHEOSIS_ROOT\Src\tools\auto-diag2.ps1
```

-**JIT (non-GPU) line** Corresponds to:'src\port\configure-jit-arm32.ps1`/`link-driver-jit-arm32.ps1`/`compile-driver-jit-arm32.ps1`; use'src\port\configure-gpu-arm32.ps1` for the first engine configuration, etc.
-Upgrade the version number to'src\harness\Package.appxmanifest`, deploy script`-Ver` should be matched.
-Measure the size of appx with `.Length`(**Don't use`ls-la`**, Windows owner name with spaces will bias the column).
-This is **x64 build machine, ARM32 appx can't run**-the only engine verification depends on the real machine.Equipment often loses WiFi due to power saving, and the deployment is easy to break in half. Use'src\tools\Deploy-Robust.ps1` fault-tolerant retry; only appx is produced remotely and handed over to the user for deployment.
-HTTPS has no system certificate library in the App Container → Package'cacert.pem`, `WebCoreSetCACertPath' is injected at startup (CURL/OpenSSL comes with TLS 1.3, and the Schannel that does not rely on the OS is only 1.2).

## WK_WINUWP Compilation Pitfalls (x64-uwp, clang-cl)

Из опыта сборки WebCore на x64-uwp (clang-cl, c++23, `_HAS_EXCEPTIONS=0`):

### Crypto key types (CryptoKeyEC.h, CryptoKeyRSA.h)
При заглушке платформенных crypto-типов **`PlatformRSAKey` / `PlatformECKey` должны быть указателями**, не `uintptr_t`:
```cpp
// ❌ BAD: m_platformKey.get() returns std::monostate*, can't convert to uintptr_t
using PlatformRSAKey = std::uintptr_t;
// ✅ GOOD:
using PlatformRSAKey = std::monostate*;
using PlatformRSAKeyContainer = std::unique_ptr<std::monostate>;
```

### Unified source ordering bugs
Некоторые `.cpp` полагаются на случайный порядок включения в unified source batch. При смене группировки падает с `incomplete type`. Фикс: добавить явный `#include` под `#if defined(WK_WINUWP)`.
- `DocumentLoader.cpp` → `#include "EventLoop.h"` (`EventLoopTaskGroup` incomplete)

### Win32 API not available in UWP App Container
Функции Win32, доступные в desktop Windows, могут быть недоступны в UWP (`WINAPI_FAMILY=WINAPI_FAMILY_APP`).
- `GetSystemMetrics(SM_MENUDROPALIGNMENT)` → guarded out in `EventHandler.cpp`

### Missing include dirs in PlatformWinUWP.cmake
Платформенные заголовочные директории добавляются в `WebCore_PRIVATE_INCLUDE_DIRECTORIES` через `PlatformWin*.cmake`. Если какой-то include не находится — проверь, добавлен ли этот путь.
- `platform/video-codecs/BitReader.h` → добавлен в `PlatformWinUWP.cmake`
- `platform/image-decoders/ScalableImageDecoder.h` → добавлен туда же

### SecurityOrigin.h warnings (harmless)
`SecurityOrigin.h:76-77`: `returning reference to local temporary object` — `m_data.protocol()`/`m_data.host()` возвращают `String` по значению, а method обещает `const String&`. Это upstream баг, не блокирует сборку (`-Wreturn-stack-address`).

### Build speed & memory
4GB RAM недостаточно для параллельной сборки WebCore (unified source файлы >1 ГБ каждый при компиляции). **Всегда используй `ninja -j1`** на этой машине.

## x64-gpu mode (updated: Jul 2026)

The **x64-gpu build directory** (`build-x64-gpu`) is a full x64-uwp build with GPU (TextureMapper + ANGLE) + JIT.
`Harness.exe` (44.5 MB, in `Src/harness/x64/Release/Harness/`) now builds with **zero linker errors** and launches successfully.

**GPU compositing objects are now compiled**: All 21 texmap .obj files + EGL files + PlatformDisplayWin are in WebCoreFull.lib. Root cause was missing `GL_GLEXT_PROTOTYPES` define and ninja not re-run after cmake reconfigure.

**Status (Aug 2026)**: all XAML/init/font crashes are resolved — appx v0.1.8.51 launches, engine init is clean, real text renders (CJK too), and the full 8-test trace suite passes (see the CJK section below). The current known issue is **GPU whole-content distortion** (fixed 720×1080 surface stretched to ContentArea — see the "Rendering distortion" section). Historical blockers below are kept for reference only.

Historical: Post-init crash (0xc0000409) after `Page::create` — fixed across tiers 1-7 (XAML activation, BCryptGenRandom NTSTATUS inversion, font NULL deref, Frame::Navigate null type-provider, App.xbf LoadComponent, PLM survival, MinimalTest isolation).

### Full build pipeline (x64-gpu)
```powershell
. .\Src\setenv.ps1
ninja -C build-x64-gpu WebCore -j1                           # WTF+JSC+WebCore DLLs
pwsh -File Src/port/link-driver-gpu-arm32.ps1                      # WebCoreDriver-gpu.lib (Src/port/)
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true /p:MinimalTest=true
```
The `MinimalTest=true` flag builds with code-based UI (no XAML `LoadComponent`) and **no engine DLL linking** — used for crash isolation.

For the **full build** (engine DLLs + XAML UI):
```powershell
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true
```
Default `MinimalTest=false` links `WebCoreDriver-gpu.lib`, all engine DLLs, vcpkg DLLs, and ANGLE.

### Debugging: crash narrowing with DebugView
Use `OutputDebugStringA` in WebCoreDriver.cpp to narrow crashes. Capture via DebugView (must run as admin).
Download DebugView: `$env:TEMP\dbview\Dbgview.exe`. Start with `Start-Process -Verb RunAs`.
**Historical crash**: 0xc0000409 after `Page::create ok` — long fixed (tiers 1-7 below). The current font/init line is clean.

### Cairo soft-render flake (ARM-only)
On ARM32, Cairo can produce corrupted output (null/corrupt cairo_surface_t) after float overflow in `Canvas::setImageInterpolation`. Root cause: ARM NEON float-to-int conversion with `saturation` differs from x87. Guarded in WebCore with `#if defined(__ARM_PCS_VFP) || defined(__thumb__)`: `// Apotheosis: clamp to avoid NEON saturation overflow`.

### Harness linking (x64-gpu): three-pronged solution

**Problem**: The harness `WebCoreDriver-gpu.lib` references:
- ~15 WebCore internal symbols NOT exported from `WebCore.dll` (e.g. `DocumentWriter::begin`, `EventHandler::handleMouseMoveEvent`)
- 9 Cairo public API functions (`cairo_image_surface_create`, etc.) — vcpkg `cairo.lib` is an incomplete static lib (48 .objs, missing ~30 internal `.obj` files)
- 4 WTF data globals NOT exported from `WTF.dll` (`s_emptyAtomString`, `emptyStringData`, `nullStringData`, `nullAtomData`)

**Solution** (three parts, all in `Src/harness/Harness.vcxproj` x64 config):

#### 1. WebCoreFull.lib — all WebCore .objs compiled with /MD

Create from all WebCore build .obj files (947 .objs, compiled with `/MD`, no `libcmt`):
```powershell
$list = @(); dir build-x64-gpu\Source\WebCore\*.obj -Recurse | % { $list += "`"$($_.FullName)`"" }
& "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\lib.exe" /OUT:build-x64-gpu\lib\WebCoreFull.lib $list
```
**Resolves**: all ~15 missing WebCore internal symbols.  
**Note**: Keep `WebCore.lib` (import lib) alongside — the port layer uses `__declspec(dllimport)` for WebCore functions, requiring `__imp_*` thunks.

#### 2. Cairo: cairo-complete-x64-uwp.lib with CRT override

Replace vcpkg `cairo.lib` (incomplete static lib at `C:\icu-x64-uwp\lib\cairo.lib`) with complete Cairo lib:
```powershell
build-x64-gpu\libs\cairo-complete-x64-uwp.lib
```
This lib was built from Cairo 1.18.4 source with `/MT` (uses `libcmt.lib`). Add CRT override to avoid mismatch with harness's `/MD`:
```
/NODEFAULTLIB:libcmt.lib
```
**Note**: `/FORCE:MULTIPLE` is still needed for Cairo public API duplicates (symbols exist in both `WebCore.dll` exports and `cairo-complete-x64-uwp.lib` static code).
**Deployment**: Remove `cairo-2.dll` and `fontconfig-1.dll` from x64 deployment (Cairo now static; fontconfig import lib still linked but DLL missing from vcpkg).

#### 3. WTF data globals stub

Four WTF data globals are referenced by inline functions in WTF headers but NOT exported from WTF.dll:
| Symbol | Mangled name |
|--------|-------------|
| `WTF::StringImpl::s_emptyAtomString` | `?s_emptyAtomString@StringImpl@WTF@@2VStaticStringImpl@12@A` |
| `WTF::emptyStringData` | `?emptyStringData@WTF@@3UStaticString@1@B` |
| `WTF::nullStringData` | `?nullStringData@WTF@@3UStaticString@1@B` |
| `WTF::nullAtomData` | `?nullAtomData@WTF@@3UStaticAtomString@1@B` |

Created `Src/harness/wtf-string-stubs.cpp` with:
- `class StaticStringImpl` (not `struct`) — matches WebKit's `class StaticStringImpl`
- `extern const` linkage — MSVC `const` at namespace scope has internal linkage by default
- Proper base class layout: `StringImplShape{ uint32_t m_refCount; unsigned m_length; mutable unsigned m_hashAndFlags; }`

#### Verification
```powershell
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=false /t:Build /v:minimal
```
Produces `Harness.exe` (46 MB) with zero linker errors. Only `MSB3030` errors for missing deployment assets (fonts fixed).

### XAML compiler quirks (VS 2022 Community — SDK 10.0.26100.0)
- The XAML compiler (`Windows.UI.Xaml.Markup.Compiler.exe`) may fail with "error MSB4181" when `Package.appxmanifest` contains `<rescap:Capability Name="runFullTrust"/>` (SeDebugPrivilege). Remove it for UWP builds.
- The build succeeds in a single `MSBuild.exe Harness.vcxproj` invocation (no need for separate `MarkupCompilePass1; MarkupCompilePass2`).
- `Windows.metadata` resolution fails when building XAML targets separately; the full-build pipeline resolves it correctly.
- **`.g.h` vs `.g.hpp`**: VS 2022 XAML compiler generates declarations in `.g.h` files (not `.g.hpp`). Source files (`App.xaml.cpp`, `MainPage.xaml.cpp`) must `#include "App.g.h"` / `"MainPage.g.h"`. Implementations are still in `.g.hpp` files but NOT auto-compiled — you must compile them explicitly (e.g. via a helper `.cpp` like `XamlGimpl.cpp` that includes both `.g.hpp` files).
- **`.g.hpp` overwrite problem**: Even with `XamlMarkupCompileEnabled=false`, MSBuild may still run the XAML compiler and overwrite `MainPage.g.hpp` with a broken version. Solution: set `DisableEmbeddedXbf=true` (Xbf files go loose, not embedded), and use pre-generated `.g.hpp` files. The `.g.hpp.backup` copies in `Src/harness/` are the canonical pre-VS2022 versions.
- **`DisableEmbeddedXbf=true` is essential**: Without it the linker tries to embed XBF into `.rsrc` section which doesn't exist in the EXE output. With `DisableEmbeddedXbf=true`, the XAML compiler places `.xbf` files loosely in `$(OutDir)`, and the `InjectXbfForPackaging` target (line 233) ensures they're copied to the final appx package.

### VS version notes
The dev machine now uses **VS 2022 Community** (MSVC 14.44.35207). Key paths:
- `vcvarsall.bat` in `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\`
- `MSBuild.exe` in `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\`
- `lib.exe` in `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\`
- Harness.vcxproj uses `<PlatformToolset>v143</PlatformToolset>`
- Win SDK 10.0.26100.0 provides the XAML compiler (`Windows.UI.Xaml.Markup.Compiler.exe`)
- The XAML compiler (MarkupCompilePass2) places `.xbf` files in `$(OutDir)` directly when `DisableEmbeddedXbf=true`
- CRT DLLs for deployment: `MSVCP140.dll`, `MSVCP140_2.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` — all present in `C:\Windows\System32` (real 64-bit; `SysWOW64` lacks `VCRUNTIME140_1.dll`)

## x64 AppContainer launch — crash progression (Jul 2026)

Appx v0.1.8.17–v0.1.8.19 install and run on Win11 29617. The crashes form a **progression** — each fix reveals the next deeper issue:

### Crash tier 1: XAML activation (0xc000027b) — ALL newly-built EXEs
- **Symptom**: App window never appears → crash in `Windows.UI.Xaml.dll` at offset `0x916eff`
- **Exception**: `0xc000027b` (E_BOUNDS)
- **Original cause**: VS 2022 XAML compiler (`Microsoft.Windows.UI.Xaml.Build.Tasks.dll`) missing from SDK 10.0.19041.0 → `CompileXaml` MSB4181
- **Fix**: Switch `WindowsTargetPlatformVersion` to `10.0.26100.0` (which has the XAML compiler). Set `XamlMarkupCompileEnabled=false` and `DisableEmbeddedXbf=true`. Use pre-generated `.g.hpp` files (from VS2022 era) to bypass broken VS18 XAML compiler output.
- **Ongoing problem** (historical, pre-VS2022): With VS18's XAML compiler, even `XamlMarkupCompileEnabled=false` didn't prevent overwriting `MainPage.g.hpp` with a broken version. Solution: use `DisableEmbeddedXbf=true` with pre-generated `.g.hpp` files (`.g.hpp.backup` copies in `Src/harness/`).
- **Hybrid appx workaround** (historical — eliminated in v0.1.8.24+):
  1. `makeappx unpack /p Harness_0.1.8.18_x64.appx /d appx18/` (last working appx)
  2. Replace `appx18/JavaScriptCore.dll` with newly built fixed version
  3. Delete `appx18/AppxSignature.p7x` + `appx18/AppxBlockMap.xml`
  4. `makeappx pack /d appx18/ /p hybrid.appx`
  5. `signtool sign /fd SHA256 /a /f cert.pfx /p apotheosis hybrid.appx`
  6. `Add-AppxPackage hybrid.appx`

### Crash tier 2: BCryptGenRandom NTSTATUS inversion (0xc0000409) — **FIXED**
- **Symptom**: XAML loads fine → window appears → `WebCoreSessionLoad` → `FrameLoader::init()` → `__fastfail(FAST_FAIL_FATAL_APP_EXIT)` = 0xc0000409 in `ucrtbase!abort`
- **Exception**: `0xc0000409` (subcode `0x7 FAST_FAIL_FATAL_APP_EXIT`)
- **Root cause** (crash dump analysis, Harness.exe.9784.dmp): `WTF::RandomDevice::cryptographicallyRandomValues` in `JavaScriptCore.dll` calls `BCryptGenRandom` which returns **NTSTATUS** (0 = STATUS_SUCCESS, non-zero = failure). But the code uses the `!` operator — `if (!BCryptGenRandom(...)) CRASH()` — which treats 0 as false (failure). Since `BCryptGenRandom` SUCCEEDS and returns 0, `!0` = true → `CRASH()` on success.
- **Disassembly confirmed**:
  ```
  call BCryptGenRandom
  test eax, eax
  je   abort  ← jumps to abort when eax == 0 (STATUS_SUCCESS!)
  ```
- **Fix** (one line in `WebKit/Source/WTF/wtf/RandomDevice.cpp:115`):
  ```cpp
  // Before (WRONG — NTSTATUS treated as BOOL):
  if (!BCryptGenRandom(nullptr, buffer.data(), buffer.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
      CRASH();
  // After (CORRECT — NTSTATUS 0 = success):
  if (BCryptGenRandom(nullptr, buffer.data(), buffer.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
      CRASH();
  ```
- **Rebuild**: `ninja -C build-x64-gpu JavaScriptCore -j1` (46 steps, clean).
- **Verification**: App now survives past JSC init (confirmed by hybrid appx test — no more 0xc0000409).

### Crash tier 3: Cairo font NULL dereference (0xc0000005) — **REPRODUCED, DUMPED & FIX-ROOTED on the font-patched build (Aug 7, 2026)**
- **Symptom**: App window activates, engine init proceeds past XAML + JSC, then access violation during font/about:home style resolution.
- **Full appx v0.1.8.25** (`Harness_0.1.8.25_x64.appx`, 68 MB) built with the **font-patched WebCore.dll (45.25 MB)** + all 5 bundled fonts (`Assets\fonts\{segoeui,arial,times,cour,simhei}.ttf`), installed & launched on the Win11 x64 dev machine. Engine init is 100% clean (log.txt: fonts OK ×5, FONTCONFIG_FILE set, CACert 189462 B, `WebCoreSetCACertBlob` ok, "loop ready"). Crash occurs **only** once `WebCoreRenderHtml(about:home)` → `FrameLoader::init` → `DocumentWriter::end` → `resolveStyle` runs.
- **Exception**: `0xc0000005` — **"Attempt to execute non-executable address" at `WebCore+0x0`** (the PE module base). Logged by the harness UEF handler; full dump captured via `WER LocalDumps` → `cdb .ecxr; kb` gives the exact stack:
  ```
  Harness!WebCoreRenderHtml+0x253 → FrameLoader::init → DocumentLoader::startLoadingMainResource
    → maybeLoadEmpty → finishedLoading → DocumentWriter::end
    → HTMLDocumentParser::finish → prepareToStopParsing
    → Document::finishedParsing → updateStyleIfNeeded → Document::resolveStyle
    → Style::TreeResolver::resolve → Style::Scope::resolver → createDocumentResolver
    → Style::Resolver::Resolver::initialize (Style::Resolver::initialize)
    → FontCascade::primaryFont → FontCascadeFonts::primaryFont
    → FontCascadeFonts::realizeFallbackRangesAt → FontCache::lastResortFallbackFont
    → FontPlatformData::FontPlatformData+0x834  (patched ctor)
    → cairo_scaled_font_create+0x1f8 → cairo_scaled_font_create_in_error+0x20c
    → RIP = WebCore+0x0 (executing the MZ header → the PE base is non-executable)      ← CRASH
  ```
- **Root-cause interpretation** (historical, Aug 7 2026): the **patched `FontPlatformData::FontPlatformData` ctor** (`WebKit/Source/WebCore/platform/graphics/FontPlatformData.cpp`, `#if WK_WINUWP`) calls `apotheosisBundledFontFace()` → `cairo_ft_font_face_create_for_ft_face(ftFace, 0)` then `cairo_scaled_font_create(face,…)`. `cairo_scaled_font_create` could hit the FT backend and return an ERROR scaled font (`cairo_scaled_font_create_in_error`), whose internal function-pointer dispatch lands on a **NULL/garbage vtable slot → execution jumps to the module base** (non-executable). **FIXED** by the toy-face rebuild fallback in the ctor (lines 209-221) + the `if (size)` sentinel fix below — about:home has rendered cleanly since v0.1.8.25.
- **Key evidence (Aug 7 2026)**:
  - `cairo-complete-x64-uwp.lib` (2.26 MB, rebuilt **07.08 04:21**) **contains the real FreeType backend**: `cairo-ft-font.x64.obj` (57 KB, member @ `0x1C33 E6`) — inverts the old note "cairo-ft-stub only". `dumpbin /LINKERMEMBER` shows the real backend table + `cairo_ft_font_face_create_for_ft_face` come from member `cairo-ft-font.x64.obj`.
  - **freetype is fully shipped & imported at runtime**: appx contains `freetype.dll` (0.67 MB); the installed `WebCore.dll` imports `FT_Bitmap_Convert / FT_Done_Face …` → FreeType runtime works in AppContainer.
  - `stubs-font-uwp.cpp` `createFontPlatformData()` returns `nullptr`, BUT `lastResortFallbackFont` builds a `FontPlatformData` via the **patched ctor** → the bundled-TTF path is what's active.
- **TIMEOUT verification — CRITICAL correction (Aug 2026)**: The earlier diagnosis assumed the crash path was DWrite/GDI (`createCairoDWriteFontFace` in `FontPlatformDataWinCairo.cpp` ← `lastResortFallbackFont` in `FontCacheWin.cpp:353`). **That path is NOT in this build.** Verified against `build-x64-gpu` (`WK_WINUWP=1`):
  - The upstream win-font files (`FontPlatformDataWinCairo.cpp`, `FontPlatformDataWin.cpp`, `FontCacheWin.cpp`) are **NOT compiled** into `WebCoreFull.lib`/`WebCore.dll`. The real font path is **`Src\port\stubs-font-uwp.cpp`** + the patched `FontPlatformData` ctor.
  - Cairo in this build is `cairo-complete-x64-uwp.lib` with the real FT backend + `cairo-font-face-twin` (built-in "twin" vector font). There is **no DWrite / GDI font backend** compiled (`createCairoDWriteFontFace` absent from `WebCoreFull.lib`). Do NOT reintroduce it — unguarded upstream code not compiled here.

### Crash tier 3b: CJK hang — infinite loop in `FontDataCacheKeyTraits::emptyValue` — **ROOT-CAUSED & FIXED (Aug 2026)**

The real blocker behind the "font height is strange / CJK tofu" reports was not a crash at all — it was a **hang (infinite spin, watchdog at 40s)** once a CJK character reached the system-fallback path.

- **Root cause** (`WebKit/.../graphics/FontCache.cpp`): `FontDataCacheKeyTraits::emptyValue()` (lines 100-104) constructs `FontPlatformData(0.f, false, false)` as the hash-table EMPTY sentinel. With `emptyValueIsZero=true` (line 97), WTF HashTable **memset-0** its slots (`zeroBytes`/`zeroedMalloc`) and tests emptiness via `slot == Traits::emptyValue()` → `FontPlatformData::operator==` → `platformIsEqual` → compares **`m_scaledFont` pointers**. The WK_WINUWP ctor gave the size-0 sentinel a **live** cairo scaled font, so a zeroed slot's null pointer never matched the sentinel → the linear probe never terminated → infinite loop.
- **The fix (keep it)** (`FontPlatformData.cpp:182`): scaled-font creation is wrapped in `if (size)` so a size-0 FPD keeps `m_scaledFont` null and matches a memset-0 bucket. All other members' defaults already equal zero-fill (size=0, Horizontal=0, RegularWidth=0, Auto=0, null refptrs). `Font::platformInit` early-returns for size 0; `GlyphPage::fill` handles a null scaledFont.
- **simhei system fallback** (`stubs-font-uwp.cpp` `systemFallbackForCharacterCluster`): sets `apotheosisSetForcedBundledFontName("simhei.ttf")`, builds `FontPlatformData(description.computedSize(), false, false)`, clears the forced name, returns `fontForPlatformData`. The ctor's `apotheosisBundledFontFace()` (FontPlatformData.cpp:58-145) honors the forced name, loads simhei.ttf via FreeType (`FT_New_Memory_Face` → `FT_Set_Pixel_Sizes(0,16)` → `FT_Select_Charmap(UNICODE)` → `cairo_ft_font_face_create_for_ft_face`), and falls back to segoeui/arial/simhei → toy face.
- **Verification**: full 8-test trace suite passes — `ALIVE` + `afterLoad=True` for single/mixed/wan-meta/kana-n/br-kana/mixed-e/br-euro/cjk-b. CJK/U+306E/U+2026/U+20AC/U+2500 resolve with `gdc-> c=U+4E00 g=1078 ok=1`, `adv w=48.00`, soft-render `nonwhite=1115`. All temporary `sfb/fpd/fpf/fcreate/fill:` traces removed; kept only `platformInit:` end-log (stubs-other.cpp:351), `fill: MISS` (563), `fill: no/invalid scaledFont` (521), `fill: lock_face returned null` (527).

### Rendering distortion: GPU whole-content stretch — **FIXED (Aug 16 2026)**

- **Symptom**: on a real site, **all** content (text weight, images, layout) is rendered too wide — a horizontal/vertical disproportion, not a font-metrics bug.
- **Root cause** (historical): `WebCoreGpuInit` (`WebCoreDriver.cpp:2160`) created the ANGLE/swapchain surface **once** at the harness-passed size (kW×kH, initially 720×1080 — the phone portrait viewport). GPU mode never resized it: `ApplyViewportSize`/`OnContentAreaSizeChanged` bailed on `m_gpuPresent` (MainPage.xaml.cpp:1572), and `WebCoreSessionResize` only updated `g_gpuW/g_gpuH` + `glViewport` (WebCoreDriver.cpp:2541-2545), NOT the swapchain buffers. The `SwapChainPanel` (Stretch) then stretched the fixed 720×1080 portrait surface to fill ContentArea → non-uniform x/y scale → everything fat/wide. Log evidence: `contents=720x1080` persisted after GPU enable; software mode (Stretch=None, 1:1) had no distortion.
- **Fix — `WebCoreGpuResize(nativeWindow, w, h, outBuf)`** (`WebCoreDriver.cpp`, after `WebCoreGpuInit`; declared in BOTH `Src\port\WebCoreDriver.h` and `Src\harness\WebCoreDriver.h` — the two C ABI copies must stay in sync): recreate the GLContext/surface from a NEW harness `PropertySet` carrying `EGLRenderSurfaceSizeProperty = (w,h)`, plus a fresh TextureMapper, then `finishInteractionPaint`. Tear-down order matters: the old TextureMapper is freed while the old context is still current (its GL texture ids belong to it), the old context/surface is then destroyed, the new context is created + made current, and a fresh TextureMapper is built on it. `g_gpuScrollFast=false` forces `gpuPrepare`→`forceDirtyTree` so every layer's backing store regenerates on the new context. Engine-thread-only, serialized by the job queue (ANGLE marshals surface create/destroy to the panel dispatcher internally, same as init). On context-recreate failure it falls back to Cairo soft rendering (`g_gpuActive=false`). Returns 0 on success.
- **Harness wiring** (`MainPage.xaml.cpp`): `ApplyViewportSize` no longer bails on `m_gpuPresent` — it builds a fresh `PropertySet` (GpuPanel + new size) on the UI thread and posts `WebCoreGpuResize(win, w, h, rgba)` to the engine; on rc==0 it adopts kW/kH and stores the new props in `m_gpuProps` (the old props are only released after the old surface died inside the driver). `EnableGpu` success now calls `ApplyViewportSize()` so the surface catches up to ContentArea right after GPU init; the `m_pendW/m_pendH` park + `OnNavDone` mechanism covers a resize that lands while a re-navigate is loading. A 600 ms `DispatcherTimer` re-enters `ApplyViewportSize` for the no-session + about:home edge case.
- **Verification (x64, Aug 16)**: `ApplyViewportSize: engine rc=0` across many live window-resize cycles (1024→768→1081→1179→593→885→634×694) with no crash (context recreation is safe); post-resize diag shows `contents=1024x694` — the surface + viewport now track ContentArea, so the SwapChainPanel stretch is 1:1 and fixed 200×200 px test boxes stay perfect squares at any window size. Remaining caveat: real-device (Lumia 950, D3D11 FL9_3) validation still pending — the ARM32 WebCore tree (`build-arm32-gpu`) did not survive the machine reinstall and must be reconfigured + rebuilt before the phone test.
- **Do NOT** interpret this as a font issue — fonts are correct (see tier 3b); the glyphs themselves are undistorted, the whole frame is.
- **Tooling note**: `winget install Microsoft.WinDbg` drops a `cdb.exe` in the appx `WindowsApps\…\amd64\` but it **fails to run in place** (ERROR_MODULE_NOT_FOUND). **Copy `.dll` + `.exe` from that dir to a normal temp dir**, then `cdb -z <dump>.dmp -c ".ecxr; kb 30"` works. `procdump64 -ma` here yields corrupt/truncated dumps (app is killed mid-write) → use `-mm` mini dumps. WER `LocalDumps` gives 0-byte files for this UWP app too.
- **Tooling note**: `winget install Microsoft.WinDbg` drops a `cdb.exe` in the appx `WindowsApps\…\amd64\` but it **fails to run in place** (ERROR_MODULE_NOT_FOUND). **Copy `*.dll` + `*.exe` from that dir to a normal temp dir**, then `cdb -z <dump>.dmp -c ".ecxr; kb 30"` works. `procdump64 -ma` here yields corrupt/truncated dumps (app is killed mid-write) → use `-mm` mini dumps. WER `LocalDumps` gives 0-byte files for this UWP app too.

### Crash tier 4: XAML activation — Frame::Navigate (0xc000027b) — **FIXED**
- **Symptom**: App.activates → `App::OnLaunched` → `Frame::Navigate(MainPage::typeid)` → uncatchable fatal crash (not a `Platform::Exception^`; cannot be caught by try/catch)
- **Exception**: `0xc000027b` (STATUS_STACK_BUFFER_OVERRUN) or fast-fail depending on context
- **Root cause**: `IXamlMetadataProvider::_AppProvider` returns `nullptr` in non-MinimalTest builds when the XAML type info provider hasn't been initialized. `Frame::Navigate` internally queries the type provider for `MainPage` type info; null provider → crash deep inside XAML runtime.
- **Fix**: Replace `Frame::Navigate(MainPage::typeid)` with `rootFrame->Content = ref new MainPage()` in `App::OnLaunched` (`Src/harness/App.xaml.cpp:37`). Direct page creation bypasses the type-resolution path entirely. This fix is permanent — `Navigate` is never used in any code path.
- **Verification**: MainPage constructor now executes (confirmed by diagnostic file writes to `LocalState\constructor-entry.txt`).

### Crash tier 5: App.xbf LoadComponent — **FIXED**
- **Symptom**: `App::InitializeComponent()` → `Application::LoadComponent(this, "App.xbf")` fails because App.xbf cannot be embedded (EXE has no `.rsrc` section)
- **Diagnosis**: App.xaml is an empty `<Application>` element with zero resources. The XAML compiler generates App.xbf but cannot embed it in the PE when `DisableEmbeddedXbf=false` (XamlCompiler tries to add a `.rsrc` section that the linker doesn't create).
- **Fix**: Skip `Application::LoadComponent` entirely in `App::InitializeComponent()`. With empty resources, the call is a no-op functionally. (`Src/harness/App.xaml.cpp:42-48`)
- **XBF handling**: Set `DisableEmbeddedXbf=true` in vcxproj; the XAML compiler places loose `.xbf` files in `$(OutDir)`. The `InjectXbfForPackaging` target (line 233) copies from `$(IntermediateOutputPath)XamlG\*.xbf` to `$(OutDir)` and adds to `AppxPackagedFile` — XBFs in the final appx are verified: `App.xbf` (437 B), `MainPage.xbf` (12 572 B). The target is kept for safety even though the XAML compiler may place them directly in `$(OutDir)`.

### Crash tier 6: AppContainer PLM survival — **FIXED**
- **Symptom**: App loaded and MainPage constructor ran, but process terminated after ~6 seconds (PLM suspend timeout)
- **Root cause**: UWP AppContainer terminates apps that don't signal readiness to the Process Lifetime Manager (PLM). The app needs `Window::Current->Activate()` and an active message pump to survive.
- **Fix**: Ensure `Window::Current->Activate()` is called after setting content. The MainPage constructor with proper XAML element tree (even minimal code-built) keeps the dispatcher loop alive.
- **Verification**: App now lives 15+ seconds, confirmed by file timestamps in `LocalState\`.

### Crash tier 7: `MinimalTest` architecture — **APPLIED**
- **Problem**: The full MainPage.xaml.cpp (~2900 lines, full browser) depends on engine DLLs, GpuProbe, JitProbe, fontconfig setup, etc. — any of which could crash during static init. Debugging the full stack blind is impossible.
- **Solution**: `MinimalTest=true` build mode:
  - `App.xaml.cpp`: `_AppProvider::get()` returns `nullptr` under `#if defined(MINIMAL_TEST)` — this prevents type resolution (safe because no `Navigate` is used).
  - `MainPage.xaml.cpp`: The entire constructor has `#if defined(MINIMAL_TEST) / #else / #endif` blocks. In MinimalTest mode, the constructor writes a marker file, then returns immediately — no engine init, no DLL loading, no font config.
  - `MainPage.minimal.cpp`: A separate compilation unit (conditionally compiled) that builds a code-only XAML tree (Grid + TextBlock with "Minimal test — XAML works"), no XBF loading needed.
  - `GpuProbe.cpp`, `wtf-string-stubs.cpp`: Excluded from compilation under MinimalTest.
  - Linker: No `WebCoreDriver-gpu.lib`, no engine DLLs, no ANGLE libs — only `windowsapp.lib`.
  - AppX packaging still includes engine DLLs (`WebCore.dll`, `JavaScriptCore.dll`) even in MinimalTest mode (lines 186-194 of vcxproj) for crash isolation during DLL static init.
- **Status**: v0.1.8.24 builds and runs in MinimalTest mode. Full build (`MinimalTest=false`) is the next target.

### Key diagnostic discoveries (Jul 2026)
- **`ApplicationData::Current` works in App constructor** — confirmed by diagnostic file writes. The UWP `LocalFolder` API is functional in AppContainer at app startup.
- **No `.rsrc` section in linker output** — MSVC linker does not create `.rsrc` from XBF even with `DisableEmbeddedXbf=false`; `DisableEmbeddedXbf=true` with loose XBF files is the reliable approach.
- **XBF extraction test**: Both `App.xbf` and `MainPage.xbf` are present in the appx package (verified by `makeappx unpack`). XBF loading from loose files should work for `MainPage::InitializeComponent()`.
- **Frame::Navigate crash is UNCAUGHT** — The XAML runtime uses `__fastfail` internally, not a catchable `Platform::Exception^`. Standard try/catch around `Navigate` does NOT help.
- **`_AppProvider` returns `nullptr` not just in MinimalTest** — Even in full builds, the provider is created lazily (`__provider = ref new ...`). The `Navigate` path expects it to be non-null synchronously during page creation. Direct `ref new MainPage()` avoids this.
- **CRT DLLs for deployment**: `MSVCP140.dll`, `MSVCP140_2.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` — all found in `C:\Windows\System32` (real 64-bit). `SysWOW64` (32-bit) lacks `VCRUNTIME140_1.dll`. Must reference `System32` directly (not `Sysnative`, which is only for 32-bit MSBuild).
- **No `cairo-2.dll` or `fontconfig-1.dll` on x64** — These are not in `C:\vcpkg\installed\x64-uwp\bin\`. Cairo is statically linked via `cairo-complete-x64-uwp.lib`. Fontconfig is NOT a runtime dependency on x64: `WebCoreFull.lib` references **0** fontconfig symbols (verified via `llvm-nm -u`), and no `fontconfig.dll` ever ships in the x64 appx — removed `fontconfig.lib` from the x64 `AdditionalDependencies` in the vcxproj.
- **`icudt75.dll` vs `icudt75l.dat`** — ICU data comes as both `icudt75.dll` (in `build-x64-gpu\bin\`) and `icudt75l.dat` (in `C:\icu-x64-uwp\bin\`). The vcxproj packs both (lines 153-154).

### Memory constraints (4 GB RAM)
- Surface Pro 5 (i5-7300U): **only ~320–440 MB free** after boot, 18.3 GB free disk
- Compilation goes best if system is freshly rebooted
- All port files compiled successfully after reboot (15.8 MB lib)
- Use `ninja -j1` for all WebKit builds; never parallel
- clang-cl compile of `WebCoreDriver.cpp` (2321 lines, heavy header chain) takes > 15 min and risks OOM without PCH

## Toolchain reinstall recovery (Aug 2026)

After a full Windows reinstall on the build machine the OS-level tools and vcpkg were wiped,
but the repo + prebuilt `build-x64-gpu` survived. Restoring the x64-wup line:

### Winget toolchain (via `Src\port\reinstall-env.ps1`)
- `winget`: Git 2.55.0.3, `LLVM.LLVM` 22.1.8 (clang-cl/lld-link), `Kitware.CMake`, `Ninja-build.Ninja`, `Henry++.MemReduct` (4GB RAM pressure).
- **Ninja placement matters**: `configure-gpu-x64.ps1` etc. hardcode ninja. Copy it to BOTH `C:\ProgramData\chocolatey\bin\ninja.exe` AND `C:\Program Files\CMake\bin\ninja.exe`.
- `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` (repo scripts rely on it). Then `. .\Src\setenv.ps1` works.

### vcpkg x64-uwp (target libs)
- `C:\vcpkg` clone + `bootstrap-vcpkg.bat`; force `git checkout -f` only after a partial clone.
- `vcpkg install freetype libxml2 libpng zlib brotli pixman libjpeg-turbo libwebp bzip2 expat openssl curl --triplet x64-uwp --allow-unsupported`.
- **harfbuzz and fontconfig cannot install via vcpkg** (meson cannot cross-compile to UWP; BUILD_FAILED). Do NOT rely on vcpkg for them (see "import libs" below).

### ICU — version trap (critical)
- Prebuilt `WebCore.dll`/`JavaScriptCore.dll` import **icuuc75.dll / icuin75.dll (ICU 75)**. The engine is compiled against ICU 75.
- vcpkg's `icu` port now builds **ICU 78** (`icuuc78/lib`). Linking/appx against ICU 78 while the engine wants ICU 75 would load TWO ICU copies + two data-file registries in one process → guaranteed conflict.
- **Correct fix**: rebuild ICU **75** import libs from the preserved working DLLs (ICU 75 DLLs still live in the old AppPackages unpack). `Src\port\make-icu75-libs.ps1` runs `dumpbin /EXPORTS` → `.def` → `lib.exe /machine:X64 /def` → produces `C:\icu-x64-uwp\lib\{icuuc,icuin,icudt}.lib` bound to `icuuc75.dll` (`__IMPORT_DESCRIPTOR_icuuc75`). Populate `C:\icu-x64-uwp\bin` with `icuuc75.dll, icuin75.dll, icudt75.dll, icudt75l.dat` (from the preserved AppPackages). This is exactly the layout the x64 vcxproj expects.

### harfbuzz — use import lib, not source build
- harfbuzz source build through meson fails on two fronts: Windows cmd-line-length limit when ninja runs `gen-harfbuzz-world.py` (huge argument list), plus GBK-locale archiver-detection bug (`detect_static_linker` → `'NoneType' has no attribute 'upper'`; fix by `ar = llvm-lib` and `LC_ALL=C.UTF-8`).
- **Simpler, correct**: the preserved working appx already ships the exact `harfbuzz.dll` (x64, no freetype/fontconfig import). Generate `harfbuzz.lib` as an **import lib** from that DLL (same dumpbin→def→lib.exe procedure), copy `harfbuzz.dll` into `C:\vcpkg\installed\x64-uwp\bin\`.
- Run `llvm-nm -u <WebCoreFull.lib>` to confirm which external symbols the harness link must actually resolve: jpeg(16), webp(10), libxml2(93), cairo(229), ICU75(165). `freetype`, `pixman`, `harfbuzz`, `fontconfig` = 0 → those four can be dropped from the x64 `AdditionalDependencies`.

### fontconfig — not needed on x64
- `WebCoreFull.lib` + harness reference **0** `Fc*` symbols and no `fontconfig.dll` ships. The `fontconfig.lib` entry was removed from the x64 link line. (fontconfig only exists as a config-file writer in `MainPage.xaml.cpp` — it writes a `.conf` and sets `FONTCONFIG_FILE`; no API calls.)

### Modern link fixes (verified on VS 14.44, 2026)
- **ICU version shadow**: vcpkg's `icu` port produces `icuuc.lib`/`icuin.lib`/`icudt.lib` bound to **ICU78** (`icuuc78.dll`) in `C:\vcpkg\installed\x64-uwp\lib`. The harness library dir order puts vcpkg **first**, so `icuuc.lib` etc. resolved to ICU78 → **80+ LNK2001** for every `u*_75` symbol (engine + WebCoreFull are compiled against ICU **75**). Fix: **delete the vcpkg ICU libs** (`icuuc/icuin/icudt/icuio.lib`) so the linker falls through to `C:\icu-x64-uwp\lib` (ICU75 import libs bound to `icuuc75.dll`). Do NOT reinstall vcpkg `icu`, and do not recreate those libs.
- **MSVC STL intrinsics (gdi-stubs.cpp)**: `__std_min_element_f`, `__std_smf_hypot3f`, `__std_find_trivial_*`, `__std_reverse_trivially_swappable_*`, `__std_mismatch_1` etc. are now part of the MSVC STL runtime (`msvcprt.lib`/`msvcp140.dll` via default `/MD`). The MSVC 14.44 headers redeclare them `extern "C"` with *new* signatures (`const void*` + `bool`, `noexcept`, `_Min_max_element_t`/`_Min_max_t` returns), so a hand-written stub with the old `float`/`double` signatures fails with **C2733** (cannot overload an `extern "C"` function). Fix: removed the STL-stub block entirely from `Src\harness\gdi-stubs.cpp` — the GDI stubs (lines 1-103) are still needed for Cairo's win32 backend, but STL intrinsics resolve from msvcp140 automatically.

### ARM32 line — full recovery runbook
The ARM32 environment (build trees, `C:\icu-arm-uwp`, `C:\vcpkg\installed\arm-uwp`,
`Src\angle\arm`, MSVC `lib\arm`, Ruby) was **fully wiped** by the same reinstall and no preserved
ARM32 artifact remains on this machine. The step-by-step rebuild checklist is
**`Doc/ARM32-RECOVERY.md`** (verified Aug 16). The two external blockers: ICU 75 **ARM** DLLs
(no official ARM32 ICU binaries; must come from the original author / a source build) and
**ANGLE ARM** (`Src\angle\arm`, provenance not recorded). Everything else (SDK 19041 `um\arm` +
`ucrt\arm`, LLVM, WebKit source, vcpkg arm triplet, the ARM scripts) is present; the missing
`lib\arm` CRT + Ruby are one-time installs.

## Plan / task policy (Aug 2026)

- **GitHub-related tasks are OPTIONAL** - everything under CI / GitHub Actions / auto-releases / PR pipelines / GitHub-hosted runners is deferred and non-blocking. Do not schedule them as required milestones; treat them as "nice-to-have, only if everything else is done". Local builds and local verification always come first.
- **All ARM32-related dev/build tasks are FULLY LOCAL** - they target the dev machine (this repo + `build-arm32-*` + `build-x64-gpu`), using the local toolchains (clang-cl / lld-link / MSVC v143) and local scripts (`arm32-uwp-env.ps1`, `configure-gpu-*.ps1`, `link-*.ps1`). No cloud CI, no GitHub Actions, no remote runners for ARM32. Anything that needs a real device is a manual on-device test via `Deploy-Robust.ps1` + WDP, not a pipeline.
- Roadmap ordering follows the same policy: local x64 verification -> local ARM32 build artifacts -> manual real-device validation -> only then (optional) any CI/GitHub automation.

## Project memory (the deep background is here)

The **Root cause / trial and error / real machine data points of each milestone**, and **upstream WebKit patch list** are all in the Codex project memory (`MEMORY.md 'Index + each `.md`), not in the warehouse. Scan before you do it `MEMORY.md`. There is also `in the warehouse `HANDOFF.md` (Phase 0, too early),`M2-HANDOFF.md` (GPU rendering details), `README.md `.
---

### AI agents contributing

- **opencode** - https://opencode.ai · model deepseek-v4-flash-free (opencode/deepseek-v4-flash-free) · Aug 2026
- **Claude Opus 5** - (to be added)

# Project Apotheosis — Engineering Research Summary

> Compiled by an AI code agent during a deep-dive session (June 28, 2026).
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
| Live interactive session | ✅ | Real mouse events, form input, scroll, keyboard |
| JSC JIT | ✅ | `codeGeneration` capability enables JIT (~5-50×) |
| GPU compositing (ANGLE + TextureMapper) | ✅ | Direct present to SwapChainPanel |
| Smooth scroll / pinch-zoom | ✅ | GPU-backed, real-time scale + re-rasterize |
| Browser shell (tabs, URL bar, settings) | ✅ | Version 0.1.8+ |
| Multi-language UI (en/ru/cn) | 🆕 **Added** | See `PLAN.md` |

---

## 2. Source & Origin

- **GitHub:** [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis)
- **Reddit announcement:** [r/windowsphone](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- **Author:** Jimmy Xiao (GitHub: `Jimmyxiao2009`)
- **License:** MIT (port layer); LGPL-2.1/BSD (upstream WebKit + dependencies)

The upstream WebKit source is **webkitgtk-2.52.4** (released June 2, 2026), available at:
- GitHub tag: [`webkitgtk-2.52.4`](https://github.com/WebKit/WebKit/tree/webkitgtk-2.52.4) (commit `7acdf5e`)
- Tarball: `https://github.com/WebKit/WebKit/archive/refs/tags/webkitgtk-2.52.4.tar.gz`
- webkitgtk.org: [Stable tarball](https://webkitgtk.org/)

---

## 3. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│  Harness — UWP App (C++/CX, MSVC v143 ARM)                    │
│   · MainPage: toolbar, address bar, gestures → engine         │
│   · GpuPanel (SwapChainPanel) ← GPU | RenderImage ← SW fallback│
└────────────────────────────┬─────────────────────────────────┘
                             │  C ABI (WebCoreDriver.h)
┌────────────────────────────▼─────────────────────────────────┐
│  WebCoreDriver (clang-cl → WebCoreDriver-gpu.lib/.dll)        │
│   · Resident Page/Frame session, event dispatch               │
│   · Cairo paintToRGBA | TextureMapper GPU composite           │
│   · PortChromeClient / FrameLoaderClient / Strategies         │
└────────────────────────────┬─────────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────────┐
│  WebKit / WebCore / JSC / WTF (clang-cl, thumbv7-windows-msvc)│
│   · ARM32/App Container patches guarded by WK_WINUWP          │
│   · Source: webkitgtk-2.52.4 (not in repo, GB-scale)          │
└──────────────────────────────────────────────────────────────┘
```

**Key architectural decisions:**
- Three layers decoupled by a stable **C ABI** (`WebCoreDriver.h`)
- Engine thread: **all WebCore/JSC calls serialized** on a single background thread
- UI thread: **never** synchronously waits on the engine (deadlock prevention)
- **Two paint paths**: Cairo software (universal fallback) | TextureMapper GPU (runtime switch via `g_gpuActive`)
- ANGLE (D3D11 FL9_3) as the OpenGL ES 2.0 wrapper for UWP App Container

---

## 4. Build System Deep Dive

### 4.1 Three Toolchains (must keep separate)

| Layer | Compiler | Target | Output |
|-------|----------|--------|--------|
| WTF/JSC/WebCore | **clang-cl** (LLVM 22.1.7) | `thumbv7-unknown-windows-msvc` | Static `.lib` |
| Port driver | **clang-cl** + **lld-link** | ARM32 UWP | `WebCoreDriver-gpu.dll` |
| Harness (UWP app) | **MSVC v143** (14.44.35207) | ARM | `Harness.appx` |

**Critical constraint:** VS2022+ removed ARM32 vcvars. The project uses `arm32-uwp-env.ps1` to manually set INCLUDE/LIB/PATH from SDK 22621 (the last SDK with ARM32 libraries — SDK 26100 deleted them).

### 4.2 WebKit Build Configuration

Three parallel build directories from one patched WebKit tree:

| Dir | CMake Flags | Purpose |
|-----|-------------|---------|
| `build-clang-webcore` | Cairo only, no JIT | Phase 1b SW baseline |
| `build-clang-jit` | JSC JIT ON | JIT line |
| `build-clang-gpu` | JIT + TextureMapper + ANGLE | **Active dev line (gpu-path1)** |

### 4.3 The Complete Dependency Chain

```
webkitgtk-2.52.4 source (E:\Apotheosis\WebKit\)
  │
  ├── WTF.lib  ──┐
  ├── JavaScriptCore.lib ─┤
  ├── PAL.lib ────────────┤
  └── WebCore.lib ────────┤
                          │
port/*.cpp (12 source files) ──┐
                               │
  ANGLE (libEGL.lib, libGLESv2.lib) ──┤
  Cairo + pixman ──────────────────────┤
  FreeType + fontconfig + HarfBuzz ────┤
  libcurl + OpenSSL ───────────────────┤
  ICU 78 (icuuc.lib, icuin.lib, icudt.lib) ──┤
  libxml2, sqlite3, zlib, bzip2, brotli ─────┤
  libjpeg-turbo, libpng, libwebp ─────────────┤
  WindowsApp.lib ─────────────────────────────┤
                                               │
                    lld-link /DLL /MACHINE:ARM
                               │
                    WebCoreDriver-gpu.dll
                          (the product)
                               │
                    Harness.appx (UWP app)
```

### 4.4 Dependencies: Where Each Comes From

| Dependency | Source | Location |
|------------|--------|----------|
| Cairo, pixman, FreeType, fontconfig, expat, HarfBuzz, libjpeg-turbo, libpng, libwebp, libxml2, sqlite3, zlib, bzip2, brotli, libcurl, OpenSSL | **vcpkg** (`arm-uwp` triplet, VS2017 v141) | `C:\vcpkg\installed\arm-uwp\` |
| ICU 78 | **Custom cross-build** (manually assembled) | `C:\icu-arm-uwp\` |
| ANGLE (libEGL, libGLESv2) | **Pre-built** (Windows Store ANGLE NuGet / manual build) | `E:\Apotheosis\angle\arm\` |
| WebKit source (WTF/JSC/PAL/WebCore) | **Git sparse checkout** of webkitgtk-2.52.4 | `E:\Apotheosis\WebKit\` (gitignored) |

---

## 5. Key Engineering Discoveries

### 5.1 The Critical Patches

All upstream WebKit modifications use `#if defined(WK_WINUWP)` guards with `Apotheosis:` comments. The key categories:

| Area | Changes |
|------|---------|
| **Memory allocation** | `VirtualAlloc` → `VirtualAllocFromApp` (App Container sandbox) |
| **File I/O** | `CreateFileW` → `CreateFile2` (App Container allowed API) |
| **Crypto** | `CryptGenRandom` → `BCryptGenRandom` |
| **Threading** | Remove SEH `__try` (clang ARM can't lower `cleanupret`), remove VEH |
| **Networking** | Stub out `DNSResolveQueuePlatform`, use generic `RunLoop`/`MainThread` |
| **Graphics** | Cairo over DirectWrite (fontconfig+FreeType+HarfBuzz for font shaping) |
| **C++ exception** | `_HAS_EXCEPTIONS=0` + `/EHs-c-` — clang ARM can't lower Windows EH |
| **mpark::variant** | Replaced with `std::variant` (clang MS-ABI mangler can't handle pack expansion in mpark::variant) |

### 5.2 The "Pack Expansion" Compiler Wall

The **hardest bug** in the project: clang's `thumbv7-windows-msvc` backend cannot mangle variadic pack expansions in function template signatures when using `mpark::variant`. The fix was to replace `WTF::Variant` (= `mpark::variant`) with `std::variant` under `WK_WINUWP` guard. This was blocking ~80% of WebCore compilation.

### 5.3 Why x64 Build Doesn't Work Yet

The project has **no x64-uwp dependencies**. All third-party libraries were cross-compiled for ARM32 only:

| Component | ARM32 | x64-uwp |
|-----------|-------|---------|
| vcpkg deps | ✅ `arm-uwp` triplet | ❌ Need `x64-uwp` triplet |
| ICU 78 | ✅ Custom cross-build | ❌ Need x64 ICU |
| ANGLE | ✅ Pre-built ARM | ❌ Need x64 ANGLE Windows Store |
| WebKit source | ✅ Same source tree | ✅ Same source works |
| WebCore config | ✅ `build-clang-gpu` | ❌ Need `build-x64-gpu` |
| Port driver | ✅ `link-driver-gpu.ps1` (ARM) | ❌ Need x64 link script |
| Harness appx | ✅ ARM | ❌ Need x64 appx packaging |

**The WebKit source itself is the same** — only the toolchain target triple changes to `x86_64-unknown-windows-msvc`.

---

## 6. Multi-Language UI Implementation

**Status:** Complete. Dual-source: `.resw` (primary) + hardcoded table (fallback).

| Language | Code | ID |
|----------|------|----|
| 中文 (Chinese, original) | `zh-Hans` | 0 |
| English | `en-US` | 1 |
| Русский (Russian) | `ru-RU` | 2 |

**Resource files created:**
- `Src/harness/Resources/en-US/Resources.resw`
- `Src/harness/Resources/zh-Hans/Resources.resw`
- `Src/harness/Resources/ru-RU/Resources.resw`

**Key design decision:** `GetStr(int lang, int id)` tries `ResourceLoader::GetString()` first (from .resw), falls back to `kStr[lang][id]` table. This ensures operation on Win10M even if UWP resource lookup fails.

**All hardcoded Chinese toasts replaced:** Every user-visible string (loading, timeout, cancelled, bookmarked, copied, cleared, find, share, download status, update check, export) now uses `GetStr(m_uiLang, S_XXX)`.

**6 new string IDs added:** `S_TOAST_BOOKMARKED`, `S_TOAST_UNBOOKMARKED`, `S_TOAST_HIST_CLEARED`, `S_TOAST_FAV_CLEARED`, `S_TOAST_DL_CLEARED`, `S_TOAST_CANNOT_FIND` (~78 total).

**Files modified:**
- `Package.appxmanifest` — resource declarations for en-US, ru-RU, zh-Hans
- `MainPage.xaml` — ComboBox language selector in Settings
- `MainPage.xaml.h` — `m_uiLang`, `ApplyLanguage()`, `OnLangChanged()`
- `MainPage.xaml.cpp` — complete string table, dual-resource loading, language persistence

**Future:** Migrate static XAML labels to `x:Uid` binding; auto-detect system language.

---

## 7. x64 Build Infrastructure

Created during this session to support x64 UWP builds:

| File | Purpose |
|------|---------|
| `Src/port/Toolchain-x64-UWP-clang.cmake` | clang-cl targeting `x86_64-unknown-windows-msvc` |
| `Src/port/vcpkg-triplets/x64-uwp.cmake` | vcpkg overlay triplet (VS2022 v143, WinStore) |
| `Src/port/configure-gpu-x64.ps1` | CMake configure for `build-x64-gpu` |
| `Src/port/link-driver-gpu-x64.ps1` | lld-link → `WebCoreDriver-x64.dll` |

**Prerequisites:** Install x64-uwp deps via vcpkg (same set as ARM), build ICU 78 for x64-uwp, acquire x64 ANGLE binaries.

## 8. WebKit Upgrade Research

WebKitGTK 2.53.4 (June 23, 2026) analyzed against our TextureMapper-based port:

- **Mostly Skia-focussed** — low impact on our `USE_TEXTURE_MAPPER` path
- **Thread sync fixes** for scrolling — potentially relevant, needs commit review
- **Overall effort:** Low-Medium (~2-3 days)
- Full report in `Doc/WEBKIT-UPGRADE.md`

## 9. Recommended Next Steps

### Short-term (days)
1. **Finish x64 build**: install vcpkg x64-uwp deps, build ICU + ANGLE for x64-uwp, configure + build WebKit for x64 (scripts created: `Toolchain-x64-UWP-clang.cmake`, `vcpkg-triplets/x64-uwp.cmake`, `configure-gpu-x64.ps1`, `link-driver-gpu-x64.ps1`)
2. **Clean up repo**: remove repro/experiment files from `port/`
3. **Translate `L"文本文件"` in filer picker** and remaining static XAML strings

### Medium-term (weeks)
1. **Upgrade to webkitgtk-2.53.4** (released June 23, 2026) — see `WEBKIT-UPGRADE.md` for analysis (Low-Medium effort, mostly Skia-focused)
2. **Refactor the port layer** — consolidate stubs, reduce code duplication between ARM/x64
3. **Add CI** — build verification for both ARM and x64

### Long-term (months)
1. **Upstream the WinUWP port** — submit `WK_WINUWP` patches to WebKit project
2. **Add WebGL support** — already partially working through ANGLE
3. **Service Worker / PWA support** — needed for modern web apps

---

## 8. Repository Layout (Simplified)

```
E:\Apotheosis\              ← ASCII path only! (Ruby/meson break on non-ASCII)
├── WebKit\                 ← webkitgtk-2.52.4 sparse checkout (gitignored)
├── port\                   ← Port layer sources + build scripts (tracked)
│   ├── WebCoreDriver.{cpp,h}
│   ├── PortChromeClient.{h,cpp}
│   ├── LoadingFrameLoaderClient.{h,cpp}
│   ├── PortPlatformStrategies.cpp
│   ├── PortNetworkStorageSession.{cpp,h}
│   ├── stubs-*.cpp         ← 146 platform stubs
│   ├── Toolchain-ARM32-UWP-clang.cmake
│   ├── Toolchain-x64-UWP-clang.cmake            ← New: x64 toolchain
│   ├── vcpkg-triplets/
│   │   ├── arm-uwp.cmake
│   │   └── x64-uwp.cmake                        ← New: x64 triplet
│   ├── arm32-uwp-env.ps1
│   ├── configure-gpu.ps1 / link-driver-gpu.ps1 / configure-gpu-x64.ps1 / link-driver-gpu-x64.ps1
│   └── *.repro* / *.bat    ← Debug artifacts (can be cleaned)
├── harness\                ← UWP host app (tracked)
│   ├── MainPage.xaml / .h / .cpp
│   ├── Package.appxmanifest
│   ├── Resources/
│   │   ├── en-US/Resources.resw                  ← New: English strings
│   │   ├── zh-Hans/Resources.resw                ← New: Chinese strings
│   │   └── ru-RU/Resources.resw                  ← New: Russian strings
│   └── Generated Files\
├── tools\                  ← WDP deploy / diagnostics (tracked)
├── angle\include\          ← ANGLE headers (tracked)
├── build-clang-*/          ← Build outputs (gitignored)
├── Doc\                    ← Documentation
│   ├── HANDOFF.md
│   ├── M2-HANDOFF.md
│   ├── PLAN.md
│   ├── Summary.md              ← This file
│   └── WEBKIT-UPGRADE.md       ← New: 2.52.4→2.53.4 analysis
├── AGENTS.md               ← Codex/Copilot guidance
├── CLAUDE.md               ← Legacy agent notes
├── README.md               ← Expanded English README
├── README-CN.md            ← Chinese README
└── README-RU.md            ← Russian README
```

---

*"Reviving the Windows Phone web for the people who never let it die."*

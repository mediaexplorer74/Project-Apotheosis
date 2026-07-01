# Project Apotheosis — EdgeHTML Reborn

> Porting modern **WebKit/WebCore** (webkitgtk-2.52.4) to **Windows 10 Mobile (ARM32, UWP)**.
> Bringing JIT-accelerated, GPU-composited web browsing back to the Lumia 950.

## Status

### Real device (Lumia 950, Win10M 15254)

| Feature | Status |
|---------|--------|
| WTF + JavaScriptCore CLoop | ✅ |
| WebCore + Cairo software rendering | ✅ |
| Live interactive session (click, form, scroll, keyboard) | ✅ |
| JSC JIT (~5-50× speedup) | ✅ |
| GPU compositing (ANGLE D3D11 FL9.3 + TextureMapper) | ✅ |
| Smooth scrolling / pinch-zoom | ✅ |
| Browser shell (tabs, URL bar, settings) | ✅ |
| Multi-language UI (en/ru/zh) | ✅ |

### x64 PC Debug Build (in progress)

| Component | Status |
|-----------|--------|
| Dependencies (vcpkg 16 pkgs, ICU, SQLite, ANGLE) | ✅ Installed |
| WebKit CMake configure | ✅ First success (June 29) |
| WTF + bmalloc compilation | ✅ Compiled (12+ WK_WINUWP patches) |
| PAL headers | ✅ Generated |
| GNU driver (clang++) for AT&T asm files | ✅ Both LowLevelInterpreter.cpp & MacroAssemblerX86_64.cpp ✅ |
| JavaScriptCore → `bin/JavaScriptCore.dll` | 🔄 Compiling; ~8/111 unified sources, warnings only |
| CMake 4.0 missing rules workaround | ✅ Auto-scanner in `patch-build-ninja-gnu.ps1` |
| WebCore → `bin/WebCore.dll` | ❌ Blocked by JSC |
| Port driver → `WebCoreDriver-x64.dll` | ❌ |
| Harness.appx | ❌ |
| WebCore → `bin/WebCore.dll` | ❌ |
| Port driver → `WebCoreDriver-x64.dll` | ❌ |

## Architecture

```
Harness (UWP C++/CX App)
   · SwapChainPanel ← GPU | WriteableBitmap ← SW fallback
        │  C ABI (WebCoreDriver.h)
WebCoreDriver (port layer)
   · Page/frame management, event dispatch
   · Cairo software | TextureMapper GPU rendering
        │
WebKit / WebCore / JSC / WTF
   · WK_WINUWP patches for ARM32 UWP App Container
```

## Repository

This repo tracks only the **port layer and harness** — not the GB-scale upstream WebKit source.

```
Src/
├── port/        ← WebCore driver, stubs, build scripts, toolchains
├── harness/     ← UWP host app (C++/CX, XAML)
├── tools/       ← WDP deploy + diagnostics
├── angle/include/ ← ANGLE headers
└── setenv.ps1   ← Environment setup
Doc/             ← Documentation (PLAN, Summary, Wiki in 3 languages)
```

## Build

```powershell
. .\Src\setenv.ps1
pwsh -File Src/port/link-driver-gpu.ps1       # ARM32
pwsh -File Src/port/build-harness.ps1          # Appx
pwsh -File Src/tools/deploy-launch.ps1 -Ip ... # Deploy to Lumia
```

For x64 debugging:
```powershell
. .\Src\setenv.ps1
Set-Item -Path env:APOTHEOSIS_ARCH -Value x64
ninja -C build-x64-gpu JavaScriptCore WebCore
pwsh -File Src/port/link-driver-gpu-x64.ps1
```

## Credits

- [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis) — original project
- [Reddit: Porting WebKitGTK 2.52.4 to Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- WebKitGTK team — upstream engine

## License

MIT (port layer); LGPL-2.1/BSD (upstream WebKit + dependencies).

---

*As is. No support. RnD only. DIY.*

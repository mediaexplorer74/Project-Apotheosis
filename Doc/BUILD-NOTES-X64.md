# Build notes — x64-uwp (`build-x64-gpu`)

Engineering detail extracted from the old AGENTS.md so the instruction files can stay short. This is
a reference, not a runbook: the commands to actually build are in [CLAUDE.md](../CLAUDE.md). The
verbatim previous versions of both instruction files are `Doc/AGENTS-legacy-2026-08.md` and
`Doc/CLAUDE-legacy-2026-08.md`.

Everything below was verified on the x64-uwp line with clang-cl (C++23, `_HAS_EXCEPTIONS=0`) and
MSVC 14.44.35207.

## WK_WINUWP compile pitfalls

**Crypto key types** (`CryptoKeyEC.h`, `CryptoKeyRSA.h`). When stubbing the platform crypto types,
`PlatformRSAKey` / `PlatformECKey` must be **pointers**, not `uintptr_t`, because `m_platformKey.get()`
returns `std::monostate*` and that does not convert:

```cpp
// BAD
using PlatformRSAKey = std::uintptr_t;
// GOOD
using PlatformRSAKey = std::monostate*;
using PlatformRSAKeyContainer = std::unique_ptr<std::monostate>;
```

**Unified-source ordering.** Some upstream `.cpp` files rely on the incidental include order of their
unified-source batch; regrouping breaks them with `incomplete type`. Fix by adding the explicit
include under a `WK_WINUWP` guard — e.g. `DocumentLoader.cpp` needs `#include "EventLoop.h"`
(`EventLoopTaskGroup` is otherwise incomplete).

**Win32 APIs missing in an App Container** (`WINAPI_FAMILY=WINAPI_FAMILY_APP`). Example:
`GetSystemMetrics(SM_MENUDROPALIGNMENT)` is guarded out in `EventHandler.cpp`.

**Include dirs live in `PlatformWinUWP.cmake`.** Platform header directories reach WebCore through
`WebCore_PRIVATE_INCLUDE_DIRECTORIES`. If an include is not found, check that its directory was added
there — `platform/video-codecs/BitReader.h` and `platform/image-decoders/ScalableImageDecoder.h` both
had to be.

**Harmless warning.** `SecurityOrigin.h:76-77` "returning reference to local temporary object":
`m_data.protocol()` / `m_data.host()` return `String` by value while the method promises
`const String&`. Upstream bug, does not block the build.

## Memory constraints on the build box

4 GB RAM, ~320–440 MB free after boot. Unified-source translation units exceed 1 GB each while
compiling, so **`ninja -j1` always**; parallel clang-cl OOMs. Compilation goes best right after a
reboot. `WebCoreDriver.cpp` alone (heavy header chain) takes over 15 minutes and risks OOM without a
PCH.

## Cairo software-render flake (ARM only)

On ARM32 Cairo could produce a corrupt `cairo_surface_t` after a float overflow in
`Canvas::setImageInterpolation` — ARM NEON float-to-int saturation differs from x87. Guarded in
WebCore with `#if defined(__ARM_PCS_VFP) || defined(__thumb__)` and the comment
`// Apotheosis: clamp to avoid NEON saturation overflow`.

## Harness link: the three-pronged solution

`WebCoreDriver-gpu.lib` references three groups of symbols the ordinary link cannot supply:

- ~15 WebCore internal symbols that `WebCore.dll` does not export (`DocumentWriter::begin`,
  `EventHandler::handleMouseMoveEvent`, …).
- 9 Cairo public API functions — vcpkg's `cairo.lib` is an incomplete static lib (48 objects, missing
  ~30 internal ones).
- 4 WTF data globals that `WTF.dll` does not export, referenced by inline functions in WTF headers.

All three fixes live in the x64 configuration of `Src/harness/Harness.vcxproj`.

**1. `WebCoreFull.lib` — every WebCore object, built `/MD`.** Archive all 947 WebCore build objects
into one lib:

```powershell
$list = @(); dir build-x64-gpu\Source\WebCore\*.obj -Recurse | % { $list += "`"$($_.FullName)`"" }
& "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\lib.exe" /OUT:build-x64-gpu\lib\WebCoreFull.lib $list
```

Keep the `WebCore.lib` import library alongside it: the port layer declares WebCore functions
`__declspec(dllimport)` and needs the `__imp_*` thunks.

**2. Cairo — `cairo-complete-x64-uwp.lib` plus a CRT override.** Use
`build-x64-gpu\libs\cairo-complete-x64-uwp.lib` (built from Cairo 1.18.4 with `/MT`) instead of
vcpkg's incomplete `cairo.lib`, and add `/NODEFAULTLIB:libcmt.lib` so its `/MT` CRT does not fight the
harness's `/MD`. `/FORCE:MULTIPLE` is still required, because the Cairo public API exists both as
`WebCore.dll` exports and as static code in that lib. Cairo is therefore static: `cairo-2.dll` and
`fontconfig-1.dll` are not deployed on x64.

**3. `Src/harness/wtf-string-stubs.cpp` — the four WTF data globals.**

| Symbol | Mangled name |
|---|---|
| `WTF::StringImpl::s_emptyAtomString` | `?s_emptyAtomString@StringImpl@WTF@@2VStaticStringImpl@12@A` |
| `WTF::emptyStringData` | `?emptyStringData@WTF@@3UStaticString@1@B` |
| `WTF::nullStringData` | `?nullStringData@WTF@@3UStaticString@1@B` |
| `WTF::nullAtomData` | `?nullAtomData@WTF@@3UStaticAtomString@1@B` |

Three details make it link: `class StaticStringImpl` (not `struct`, matching WebKit); `extern const`
linkage, because MSVC gives namespace-scope `const` internal linkage by default; and the exact base
layout `StringImplShape { uint32_t m_refCount; unsigned m_length; mutable unsigned m_hashAndFlags; }`.

**Which externals actually have to be resolved** — run `llvm-nm -u` on `WebCoreFull.lib`. Counts
measured on this line: jpeg 16, webp 10, libxml2 93, cairo 229, ICU 75 165, and **zero** for freetype,
pixman, harfbuzz and fontconfig, which is why those four were dropped from the x64
`AdditionalDependencies`. fontconfig survives only as a config-file writer in `MainPage.xaml.cpp`
(it writes a `.conf` and sets `FONTCONFIG_FILE`; no API calls).

## XAML compiler quirks (VS 2022 Community, SDK 10.0.26100.0)

- **`.g.h`, not `.g.hpp`.** The VS 2022 XAML compiler puts declarations in `.g.h`, so `App.xaml.cpp` /
  `MainPage.xaml.cpp` include `App.g.h` / `MainPage.g.h`. The implementations still land in `.g.hpp`
  but are **not** compiled automatically — a helper `.cpp` (`XamlGimpl.cpp`) includes both.
- **`.g.hpp` gets overwritten.** Even with `XamlMarkupCompileEnabled=false`, MSBuild may still run the
  XAML compiler and replace `MainPage.g.hpp` with a broken version. Fix: `DisableEmbeddedXbf=true`
  plus the pre-generated files; the `.g.hpp.backup` copies in `Src/harness/` are the canonical
  pre-VS2022 versions.
- **`DisableEmbeddedXbf=true` is essential.** Without it the linker tries to embed XBF into an
  `.rsrc` section the EXE does not have. With it, `.xbf` files are written loose into `$(OutDir)` and
  the `InjectXbfForPackaging` target copies them into the appx.
- `<rescap:Capability Name="runFullTrust"/>` in `Package.appxmanifest` makes the XAML compiler fail
  with `MSB4181`. It has no place in a UWP build.
- One `MSBuild.exe Harness.vcxproj` invocation is enough — no separate
  `MarkupCompilePass1`/`MarkupCompilePass2`. Building the XAML targets separately breaks
  `Windows.metadata` resolution.

## VS 2022 paths (MSVC 14.44.35207)

- `vcvars64.bat` / `vcvarsall.bat` — `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\`
- `MSBuild.exe` — `…\2022\Community\MSBuild\Current\Bin\`
- `lib.exe` — `…\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\`
- `Harness.vcxproj` uses `<PlatformToolset>v143</PlatformToolset>`; the XAML compiler
  (`Windows.UI.Xaml.Markup.Compiler.exe`) comes from Win SDK 10.0.26100.0.
- CRT DLLs to deploy: `MSVCP140.dll`, `MSVCP140_2.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` —
  take them from `System32` (the real 64-bit ones); `SysWOW64` has no `VCRUNTIME140_1.dll`.

## Toolchain reinstall recovery

After a Windows reinstall the OS tools and vcpkg were wiped while the repo and the prebuilt
`build-x64-gpu` survived. What it took to restore the x64-uwp line:

**winget toolchain** (`Src\port\reinstall-env.ps1`): Git, `LLVM.LLVM` 22.1.8 (clang-cl / lld-link),
`Kitware.CMake`, `Ninja-build.Ninja`, `Henry++.MemReduct` (RAM pressure). **Ninja placement matters**:
`configure-gpu-x64.ps1` and friends hardcode it, so copy `ninja.exe` to *both*
`C:\ProgramData\chocolatey\bin\` and `C:\Program Files\CMake\bin\`. Then
`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`, after which `. .\Src\setenv.ps1` works.

**vcpkg x64-uwp**: clone to `C:\vcpkg`, `bootstrap-vcpkg.bat`, then

```powershell
vcpkg install freetype libxml2 libpng zlib brotli pixman libjpeg-turbo libwebp bzip2 expat openssl curl --triplet x64-uwp --allow-unsupported
```

`harfbuzz` and `fontconfig` **cannot** be installed this way (meson cannot cross-compile to UWP).

**ICU — the version trap, and the single most expensive one here.** The prebuilt `WebCore.dll` and
`JavaScriptCore.dll` import `icuuc75.dll` / `icuin75.dll`; the engine is compiled against ICU 75.
vcpkg's `icu` port now builds ICU **78**, and the harness library search order puts vcpkg first, so
`icuuc.lib` resolves to ICU 78 and the link dies with 80+ `LNK2001` for every `u*_75` symbol. Two
ICU copies in one process would also mean two data-file registries. Fix: **delete the vcpkg ICU libs**
(`icuuc/icuin/icudt/icuio.lib`) so the linker falls through to `C:\icu-x64-uwp\lib`, and never
reinstall vcpkg's `icu`. Those ICU 75 import libs are regenerated from the preserved working DLLs by
`Src\port\make-icu75-libs.ps1` (`dumpbin /EXPORTS` → `.def` → `lib.exe /machine:X64 /def`), bound to
`icuuc75.dll` via `__IMPORT_DESCRIPTOR_icuuc75`. `C:\icu-x64-uwp\bin` holds `icuuc75.dll`,
`icuin75.dll`, `icudt75.dll`, `icudt75l.dat`, which is the layout the x64 vcxproj expects.

**harfbuzz — import lib, not a source build.** The meson build fails twice over: the Windows
command-line length limit when ninja runs `gen-harfbuzz-world.py`, and a GBK-locale archiver
detection bug (`detect_static_linker` → `'NoneType' has no attribute 'upper'`, worked around with
`ar = llvm-lib` and `LC_ALL=C.UTF-8`). Instead take the working `harfbuzz.dll` out of the preserved
appx, generate an import lib from it with the same dumpbin → def → lib.exe procedure, and drop the
DLL into `C:\vcpkg\installed\x64-uwp\bin\`.

**MSVC STL intrinsics (`gdi-stubs.cpp`).** `__std_min_element_f`, `__std_smf_hypot3f`,
`__std_find_trivial_*`, `__std_reverse_trivially_swappable_*`, `__std_mismatch_1` and friends now ship
in the MSVC STL runtime (`msvcprt.lib` / `msvcp140.dll` via `/MD`). The 14.44 headers redeclare them
`extern "C"` with new signatures, so the old hand-written stubs fail with `C2733` (cannot overload an
`extern "C"` function). The STL-stub block was removed; the GDI stubs at the top of the file are still
needed for Cairo's win32 backend.

## ARM32 recovery

The ARM32 environment (build trees, `C:\icu-arm-uwp`, `C:\vcpkg\installed\arm-uwp`, `Src\angle\arm`,
MSVC `lib\arm`, Ruby) was wiped by the same reinstall, with no preserved ARM32 artifact left on the
machine. The step-by-step rebuild checklist is **`Doc/ARM32-RECOVERY.md`**. Two blockers are external:
ICU 75 **ARM** DLLs (no official ARM32 ICU binaries exist — they must come from the original author or
a source build) and **ANGLE ARM** (`Src\angle\arm`, provenance never recorded). Everything else — SDK
19041 `um\arm` + `ucrt\arm`, LLVM, the WebKit source, the vcpkg arm triplet, the ARM scripts — is
present; the missing `lib\arm` CRT and Ruby are one-time installs.

## Plan / task policy

- **GitHub and CI work is OPTIONAL**: CI, GitHub Actions, auto-releases, PR pipelines and hosted
  runners are all deferred and non-blocking. Local builds and local verification come first.
- **All ARM32 development is fully local**: this repo, `build-arm32-*`, `build-x64-gpu`, the local
  toolchains and the local scripts. Anything that needs a real device is a manual on-device test via
  `Deploy-Robust.ps1` + Device Portal, not a pipeline.
- Roadmap order follows from that: local x64 verification → local ARM32 build artifacts → manual
  real-device validation → optionally, much later, any CI automation.

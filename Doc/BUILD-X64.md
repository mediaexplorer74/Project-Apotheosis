# x64 build line — `build-x64-gpu`

> **Current (rewritten 2026-09-26).** The shipping product is the **C# UWP app** (`Src/Apotheosis`),
> which does *not* link the engine — it loads `WebCoreDriver.dll` and friends from the appx package
> root. Everything below is either an engine-build constraint that is still true, or a driver-DLL link
> step. The C++/CX harness build record that used to live in this file is now
> [Archive/2026-08-17-x64-harness-build-notes.md](Archive/2026-08-17-x64-harness-build-notes.md).
>
> The ARM32 counterpart is [BUILD-ARM32.md](BUILD-ARM32.md); both lines share the source tree and the
> SDK but **nothing else** — not objects, not generated bindings, not `build.ninja`, not the CMake cache.

## 0. The three build stages, and which one you actually need

| Stage | Command | Cost | When you run it |
|---|---|---|---|
| Engine (WebKit) | `ninja -C build-x64-gpu -j1` | **hours** | only when WebKit or the port stubs change |
| Driver DLL | `pwsh -File Src/port/build-driver-dll-arm32.ps1 -Arch x64` | minutes | every native change |
| C# appx | `MSBuild /restore /p:Configuration=Debug /p:Platform=x64 Src/Apotheosis/Apotheosis.csproj` | minutes | every managed or packaging change |

```powershell
. .\Src\setenv.ps1
. .\Src\port\x64-uwp-env.ps1
cmake -S . -B build-x64-gpu -G Ninja -DCMAKE_TOOLCHAIN_FILE=$env:APOTHEOSIS_PORT/Toolchain-x64-UWP-clang.cmake
ninja -C build-x64-gpu -j1
```

Because the C# app links no engine, stage 1 is almost never the one you want. A driver-DLL rebuild
does **not** require the multi-hour WebKit build — the engine archives already exist in
`build-x64-gpu/lib` and `build-x64-gpu/libs`.

**`ninja -j1` is not optional on this box** (4 GB RAM, 320–440 MB free after boot; unified-source TUs
exceed 1 GB each while compiling). Parallel clang-cl OOMs. Compilation also behaves best right after a
reboot, and `WebCoreDriver.cpp` alone takes over 15 minutes without a PCH.


## 1. `WebCoreFull-x64-19041.lib` — the static WebCore archive

`WebCoreDriver.dll` contains WebCore **statically** and does not import `WebCore.dll`. A sibling
`WebCore.dll` sitting in the package is unused ballast. The archive that makes this possible is every
WebCore object in one lib (currently 939 objects, 3.2 GB):

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File build-x64-gpu\run-webcorefull-archive.ps1
# WEBCOREFULL_EXIT=0
```

It is driven by a response file, `build-x64-gpu\WebCoreFull-x64-19041.rsp`, fed to `lib.exe /OUT:...`
(`PAL-x64-19041.rsp`, 24 entries, is the companion). Rebuilding takes ~90 s, so "the static library is
stale" is cheap to disprove.

> **Warning: the response files store absolute member paths.** Renaming the build tree therefore does
> **not** finish until every `*.rsp` is rewritten. See [LESSONS.md](LESSONS.md). Verify a rename by
> enumerating the rsp members with `Test-Path` *and* running the script, not by compiling something.

Keep `WebCore.lib` (the import library) alongside it: the port layer declares WebCore functions
`__declspec(dllimport)` and needs the `__imp_*` thunks.

**The port stubs are WebCore members, not a separate library** — `stubs-ax.cpp`, `stubs-crypto.cpp`
and friends compile into
`Source/WebCore/CMakeFiles/WebCore.dir/C_/.../Src/port/stubs-*.cpp.obj`. So a stale engine tree means
stale port stubs, and stage 1 rebuilds the port layer too.

## 2. The other two link gaps

`WebCoreFull.lib` alone does not close the link. Two more inputs are required:

**Cairo — `cairo-complete-x64-uwp.lib` plus a CRT override.** Use
`build-x64-gpu\libs\cairo-complete-x64-uwp.lib` (Cairo 1.18.4, `/MT`) rather than vcpkg's incomplete
`cairo.lib` (48 objects, ~30 internals missing), and add `/NODEFAULTLIB:libcmt.lib` so its `/MT` CRT
does not fight `/MD`. `/FORCE:MULTIPLE` is still required because the Cairo public API exists both as
`WebCore.dll` exports and as static code in that lib. Cairo is therefore static on x64: neither
`cairo-2.dll` nor `fontconfig-1.dll` is deployed. This lib was rebuilt with **relative members** so the
link no longer depends on a particular build-tree name.

**`Src/harness/wtf-string-stubs.cpp` — four WTF data globals** referenced by inline functions in WTF
headers and not exported by `WTF.dll`:

| Symbol | Mangled name |
|---|---|
| `WTF::StringImpl::s_emptyAtomString` | `?s_emptyAtomString@StringImpl@WTF@@2VStaticStringImpl@12@A` |
| `WTF::emptyStringData` | `?emptyStringData@WTF@@3UStaticString@1@B` |
| `WTF::nullStringData` | `?nullStringData@WTF@@3UStaticString@1@B` |

## 3. ICU 78 — how the version is pinned now

> **The ICU 75 instructions that used to live here are obsolete. Do not follow them.** They described
> deleting vcpkg's ICU libraries and falling through to `C:\icu-x64-uwp`. That directory holds
> **ICU 75** (`icuuc75.dll`, `icuin75.dll`, `icudt75l.dat`) and the engine no longer wants it.
> `make-icu75-libs.ps1` is likewise only for 75.

The measured state on 2026-09-26:

| | |
|---|---|
| Engine objects (`build-x64-gpu`) | compiled against **ICU 78** |
| `build-x64-gpu\bin\WebCore.dll` | imports `icuuc78.dll`, `icuin78.dll` |
| Shipped `WebCoreDriver.dll` | imports `icuuc78.dll`, `icuin78.dll` |
| `C:\vcpkg\installed\x64-uwp\lib\icuuc.lib` | **import library bound to `icuuc78.dll`** |
| `C:\icu-x64-uwp\lib\icuuc.lib` | import library bound to `icuuc75.dll` — do not use |

`llvm-nm --defined-only` on the vcpkg `icuuc.lib` looks alarming — ~12 000 "defined" symbols — but the
first archive member is literally named `icuuc78.dll` and only 3 030 entries are `__imp_` thunks; the
rest are `.idata$` bookkeeping. **It is an import library, not static ICU.** That is what
`$IcuLibDir = C:\vcpkg\installed\x64-uwp\lib` is for, and it is the same arrangement the ARM line uses,
so the two architectures are now consistent at 78.

The real trap here is a *mismatch*, not a missing library: the old symptom was 80+ `LNK2001` for
`u_*_75` names because the search order handed the linker a 78 library while the objects wanted 75, or
vice versa. Today both sides are 78. If you ever see `u_*_75` unresolved, something reintroduced
`C:\icu-x64-uwp` into `LIB`/`/LIBPATH` — check `$IcuLibDir` first.

`harfbuzz` and `fontconfig` **cannot** be built through vcpkg for UWP (meson will not cross-compile).
Both arrive as import libraries generated from preserved working DLLs by the
`dumpbin /EXPORTS` → `.def` → `lib.exe` procedure; the DLLs themselves are staged in
`C:\vcpkg\installed\x64-uwp\bin\`. `harfbuzz-icu.lib` is a separate library and is easy to forget —
`LocaleICU.cpp` calls `hb_icu_script_to_script` from it, not from `harfbuzz.lib`.

## 4. WK_WINUWP compile pitfalls (engine stage)

- **Crypto key types** (`CryptoKeyEC.h`, `CryptoKeyRSA.h`). `PlatformRSAKey` / `PlatformECKey` must be
  **pointers**, not `uintptr_t`, because `m_platformKey.get()` returns `std::monostate*`:
  ```cpp
  // BAD
  using PlatformRSAKey = std::uintptr_t;
  // GOOD
  using PlatformRSAKey = std::monostate*;
  using PlatformRSAKeyContainer = std::unique_ptr<std::monostate>;
  ```
- **Unified-source ordering.** Some upstream `.cpp` files rely on the incidental include order of their
  unified-source batch; regrouping breaks them with `incomplete type`. Fix by adding the explicit
  include under a `WK_WINUWP` guard — `DocumentLoader.cpp` needs `#include "EventLoop.h"`.
- **Win32 APIs missing in an App Container** (`WINAPI_FAMILY=WINAPI_FAMILY_APP`), e.g.
  `GetSystemMetrics(SM_MENUDROPALIGNMENT)` in `EventHandler.cpp`.
- **Include dirs live in `PlatformWinUWP.cmake`**, reaching WebCore through
  `WebCore_PRIVATE_INCLUDE_DIRECTORIES`. `platform/video-codecs/BitReader.h` and
  `platform/image-detoders/ScalableImageDecoder.h` both had to be added there.
- **Harmless warning.** `SecurityOrigin.h:76-77` "returning reference to local temporary object":
  `m_data.protocol()` / `m_data.host()` return `String` by value where `const String&` is promised.
  Upstream bug, does not block the build.

## 5. Building the C# appx

The C# project is `Src/Apotheosis/Apotheosis.csproj`, and its failure modes are its own. The
diagnostics that actually cost time are collected in [LESSONS.md](LESSONS.md); the three that decide
whether a build even starts:

- `VisualStudioVersion` must match the installed XAML targets folder
  (`MSBuild\Microsoft\WindowsXaml\v17.0`). A stale `14.0` makes the `Import` resolve to nothing and
  every type fails with `WMC9999`, which mimics a missing platform reference. **No NuGet package fixes
  this.**
- `.NET Native` does not compile `System.Threading.Thread`: `ILT0003` makes the constructor
  `FailFast`. Use `Task.Factory.StartNew(loop, TaskCreationOptions.LongRunning)`. The build *succeeds*,
  so this hides in a wall of `CS4014` warnings.
- `TargetPlatformMinVersion` must be **≥ 15138** or the UWP targeting pack is required, because C#
  resolves platform references through NuGet. The C++/CX harness can target 14393; the C# project
  cannot.

## 6. Packaging and verification

Native DLLs must sit at the **package root**. `<Content Include="native\x64\*.dll">` without
`<Link>%(Filename)%(Extension)</Link>` stages them under `native\x64\` inside the appx, and
`DllImport("WebCoreDriver.dll")` then fails in the AppContainer — there is no `SetDllDirectory` there.

`JavaScriptCore.dll` is still required at runtime. `WebCore.dll` is not (see §1).

The appx is the ground truth: `Add-Type -AssemblyName System.IO.Compression.FileSystem` plus
`ZipFile.OpenRead(...).Entries` verifies DLL placement, manifest identity and architecture without
installing anything. A fresh driver DLL must be checked against the one inside the package by hash —
a stale `native/x64` copy is the single most common cause of "my fix did nothing".

## 7. The UWP ("store") CRT is a trap on this machine (2026-09-26)

**Symptom.** A freshly linked `WebCoreDriver-x64.dll` imported
`MSVCP140_APP.dll`, `MSVCP140_2_APP.dll`, `VCRUNTIME140_APP.dll`, `VCRUNTIME140_1_APP.dll` — none of
which exist anywhere: not in `System32`, not under the Windows Kits, not in vcpkg. The previously
shipped driver imported the ordinary `MSVCP140.dll` / `VCRUNTIME140.dll`, and the payload stages
those. A link that "succeeds" with EXIT=0 can therefore still produce a DLL that cannot load.

**Cause.** `Src/port/x64-uwp-env.ps1` puts `…\MSVC\14.44.35207\lib\x64\store` **first** in `$env:LIB`.
That directory is the AppContainer flavour of the MSVC CRT, and the `msvcprt.lib` in it binds to the
`_APP` DLLs. Measured directly with `llvm-nm --undefined-only`:

```
lib\x64\store\msvcprt.lib  ->  MSVCP140_APP.dll
lib\x64\msvcprt.lib        ->  MSVCP140.dll
```

**Fix — x64 only.** `build-driver-dll-arm32.ps1` reorders `$env:LIB` for the **link step only** on x64,
dropping the `…\lib\x64\store` entry so the desktop CRT wins, and restores the original value in a
`finally`. The compile step still sees the store includes, which it needs. After the change the driver
imports `MSVCP140.dll`, `MSVCP140_2.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, all four of which
the payload already carries.

This is a **regression fix, not a new dependency**: the driver that actually ran and reached
`Frame::init()` used the desktop names.

**ARM deliberately keeps the store CRT.** The two lines differ in a way that matters here: x64 links
the hand-built `cairo-complete-x64-uwp.lib`, while ARM links vcpkg's `cairo.lib`, which was built
*against the store CRT*. Applying the same reorder to ARM was tried and fails — every cairo object
comes back with an unresolved `__security_cookie`, the guard variable the store CRT defines. So the
condition is `$Arch -eq 'x64'`, and whether a Lumia under W10M can resolve the `_APP` DLLs stays an
open question for tomorrow. If the ARM app fails to start, check that first.

**General rule.** After any driver link, verify the import set against the payload before rebuilding
the appx:

```powershell
llvm-readobj --coff-imports Src\port\WebCoreDriver-x64.dll   # what it needs
Get-ChildItem Src\Apotheosis\native\x64                       # what the package has
```

`build-driver-dll-arm32.ps1` now does the staging itself and prints a SHA256 comparison, so a
built-but-not-staged driver can no longer masquerade as a fix that "did nothing".

## 8. `WebCore.dll` is no longer part of the payload

Since the driver links WebCore statically, `llvm-readobj --coff-imports` on the built DLL lists
`JavaScriptCore.dll` and **not** `WebCore.dll` — that file is 44 MB of ballast. It was removed from
`native\x64` and is now `Exclude`d in `Apotheosis.csproj` for both architectures, so re-staging it
cannot silently creep back in. The original is still in `build-x64-gpu\bin\` if it is ever needed.

On a 1 GB Lumia this matters twice over: the package is smaller, and the driver no longer forces the
loader to map a 44 MB image it never calls into.

## 9. Auditing a payload — the import table lies to you

`native\<arch>` is easy to judge by eye and easy to judge wrongly. Two rules, both learned the hard
way on 2026-09-26:

**1. The import table is not the dependency list.** `icudt78.dll` is 31.6 MB — the whole ICU UCD
dataset — and `WebCoreDriver-arm.dll` does **not** import it. `icuuc78.dll` references the string
`icudt78` and loads it by name at runtime, and the string `icudt78.dat` is absent from the binary
entirely. So:

- our ICU build keeps the data **inside `icudt78.dll`**; the `.dat` form belongs to the
  `ICU_DATA_ON_DISK` configuration and survives only in the dead `C:\icu-x64-uwp\bin\icudt75l.dat`
- a payload audit driven purely by `llvm-readobj --coff-imports` will happily pass a package with no
  ICU data, and the failure then shows up as nonsense in font handling
- always add the dynamically loaded data library to the audit list by hand

**2. Audit each architecture against *its own* driver, and expect the file lists to differ.** The
measured result:

| | x64 | ARM |
|---|---|---|
| needed (imports + `icudt78.dll`) | 24 | 20 |
| missing | none | none |
| extra | `brotlicommon`, `brotlidec`, `brotlienc`, `bz2`, `iconv-2`, `icuio78`, `libsharpyuv` | none |
| payload | 31 files, 108.5 MB | 21 files, 85.2 MB |

ARM is **cleaner** than x64, not poorer. The list differs for legitimate reasons: OpenSSL is
`libcrypto-3-x64.dll` vs `-3-arm`, x64 links the desktop CRT (`MSVCP140.dll` family) while ARM links
the store CRT (`*_APP` family), and x64 carries ~1.8 MB of brotli/bz2/iconv/libyuv/icuio leftovers
from `link-driver-gpu-x64.ps1` — its own comment records that those names "sat in the x64 command for
months without anyone noticing". They were left in place deliberately: they cost 1.8 MB, and
deleting a DLL on the strength of "nothing imports it" repeats exactly the mistake this section is
about.

A blank-looking `native\ARM` used to mean "never staged". It now means "staged", and the way to tell
them apart is the audit above, not the file count.

## 10. Plan / task policy

- **GitHub and CI work is optional.** Local builds and local verification come first; anything
  needing a real device is a manual on-device test.
- **All development is local.** The repo, `build-x64-gpu`, `build-arm32-gpu`, the local toolchains and
  the local scripts. Roadmapped order: local x64 verification → local ARM32 artifacts → manual
  real-device validation → optionally, much later, CI.

| `WTF::nullAtomData` | `?nullAtomData@WTF@@3UStaticAtomString@1@B` |

Three details make it link: `class StaticStringImpl` (not `struct`, matching WebKit); `extern const`
linkage, because MSVC gives namespace-scope `const` internal linkage by default; and the exact base
layout `StringImplShape { uint32_t m_refCount; unsigned m_length; mutable unsigned m_hashAndFlags; }`.

**Which externals actually must be resolved** — run `llvm-nm -u` on `WebCoreFull.lib`. Measured on
this line: jpeg 16, webp 10, libxml2 93, cairo 229, ICU 75 165, and **zero** for freetype, pixman,
harfbuzz and fontconfig — which is why those four were dropped from `AdditionalDependencies`.
fontconfig survives only as a config-file *writer*: the driver sets `FONTCONFIG_FILE` natively, and no
fontconfig API is called.

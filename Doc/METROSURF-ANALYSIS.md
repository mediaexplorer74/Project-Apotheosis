# MetroSurf - external analysis

Date: 2026-10-27
Subject: `MetroSurf/MetroSurf`, commit `ca30d0d`, release `v2.0.1`
Comparator: Apotheosis `0.2.3.94` on WebKitGTK 2.52.4

## Scope and source hygiene

This analysis uses only the public repository `MetroSurf/MetroSurf` and its
published release assets. A third-party fork that briefly existed was removed by
its owner after the upstream maintainer objected to it; it is not cited, mirrored
or used as a source. There is no code from that fork in this document.

The upstream repository is a **source drop**, not a complete application. The
findings below separate what is verifiable from the published tree from what can
only be inferred from the release notes and the shipped binary.

## Summary

- The public tree is a WebKit 2.54.1 overlay (176 files), not a buildable
  browser: the `wkwp` shell, build scripts and manifest are missing, and there is
  no license.
- MetroSurf is an independent sibling port, not an Apotheosis descendant. Only
  the shared upstream and 19 overlapping basenames connect them.
- The cheapest reusable idea is the `K32GetProcessMemoryInfo` probe for M5.1's
  zero `memoryFootprint()` reading (section 8.1).
- The most interesting future idea is the WP81 media-engine seam: a
  `MediaPlayerFactory` registered from the port, with the native decoder behind
  `MediaPlayerPrivateInterface` (section 8.6). The player itself is unpublished.
- Do not target WP8.1 and do not republish the code. Revisit only when one of the
  triggers in section 9.1 fires.

## 1. Repository facts

| Field | Value |
|---|---|
| Repository | `MetroSurf/MetroSurf` |
| Description | "MetroSurf is a powerful browser for Windows Phone and Windows Mobile." |
| Created | 2026-10-18 |
| Last push | 2026-10-18 |
| Commits | 1 (`ca30d0d`, "MetroSurf 2: WebKit source") |
| Language / size | C++ / 1,696 KB |
| Files | 176 files (236 tree entries) |
| Top-level content | `webkit/` (122 files), `webkit-open/` (53 files), `about.txt` |
| Stars / forks / open issues | 19 / 2 / 1 |
| License | none declared |
| Release | `v2.0.1`, 2026-10-18 |
| Asset | `MetroSurf_2_0_1.appx`, 41,488,544 bytes, 280 downloads |
| Dependency | `Microsoft.VCLibs.ARM.12.00.Phone.appx`, 645,330 bytes, 149 downloads |

`about.txt` states the base exactly:

> These files replace or add to WebKit at tag webkitgtk-2.54.1
> (https://github.com/WebKit/WebKit, commit
> 628d6ce1850e17225185a5807a9d0aa375bae2be).
> `webkit/` mirrors the WebKit tree. `webkit-open/` is code from WebKit built
> into `webkit.dll`.

The public tree therefore publishes **only the WebKit-side overlay**. The
application shell, build scripts, package manifest, assets and project files are
not present.

## 2. What the source drop contains

### `webkit/` - 122 overlaid upstream files

The drop mirrors the paths of the upstream tree, but only the files that differ
are published. The distribution by area:

| Area | Files |
|---|---|
| `Source/WebCore/platform` | 44 |
| `Source/WTF/wtf` | 11 |
| `Source/WebCore/page` | 9 |
| `Source/JavaScriptCore/runtime` | 8 |
| `Source/JavaScriptCore/dfg` | 6 |
| `Source/JavaScriptCore/jit` | 5 |
| `Source/bmalloc/mimalloc` | 4 |
| `Source/ThirdParty/skia` | 3 |
| `Source/WebCore/dom` | 3 |

The emphasis is WebCore platform integration plus a substantial JavaScriptCore
ARMv7 cluster. WTF changes are fewer than in Apotheosis, which suggests MetroSurf
leans on the existing Windows port for low-level plumbing and concentrates on
WebCore, JSC and the graphics path.

### `webkit-open/` - 53 implementation files

Despite the name, `about.txt` says this code is built **into `webkit.dll`**, not
into a separate `webkit-open.dll`. The namespace is `WKWP` ("WebKit Windows
Phone"). The modules are:

| Module | Files | Role |
|---|---|---|
| Storage | 17 | `StorageAreaImpl`, `StorageNamespaceImpl`, `StorageSyncManager`, `StorageThread`, `StorageTracker`, `WebDatabaseProvider`, `WebStorageNamespaceProvider` |
| CacheStorage | 15 | Cache API backend, disk/memory stores, registry, record, keys, coders |
| WebSockets | 4 | `WPWebSocketChannel`, `WPWebSocketTask` (WinRT task bridge) |
| History | 2 | `WPBackForwardList` |
| Compositor | 2 | `CoordinatedSceneState` |
| Scheduler | 3 | `WebResourceLoadScheduler` and its error path |
| Hooks | 3 | `WPOpenHooks`, `PingHandle`, `config.h` |
| IndexedDB | 2 | `InProcessIDBServer` |

`WPOpenHooks` is a table of function pointers exported from `webkit.dll` and
filled by the host: storage session, IndexedDB directory, IndexedDB space
request. This is a deliberate seam between a closed shell and the engine, so the
engine can call host services without linking against the shell.

### What is missing

The CMake build expects a `WP81_ROOT` tree:

```cmake
add_subdirectory(${WP81_ROOT}/src/webkit-open ...)
if (EXISTS ${WP81_ROOT}/src/wkwp)
    add_subdirectory(${WP81_ROOT}/src/wkwp ...)
endif ()
```

The public drop publishes `webkit-open/` at the repository root, not under
`src/`, and has no `wkwp` directory at all. The `wkwp` target is the missing
embedding layer that would host WebCore. There is also no README, no build
script, no toolchain file, no CI and no license.

**Consequence:** the repository cannot be built as published. The expected
layout has to be reconstructed, and the shell that supplies the `WPOpenHooks`
implementations is simply absent.

## 3. Architecture as published

```
 WP8.1 / W10M application shell            (not published: wkwp)
        |
        |  WKWP::Hooks function pointers
        v
 webkit.dll
   +-- WebCore (page, dom, loader, platform)   <- 122 overlaid files
   +-- JavaScriptCore (ARMv7 JIT, DFG, runtime)
   +-- WTF / bmalloc / mimalloc
   +-- webkit-open (storage, cache, sockets, history, compositor)
        |
        +-- Skia + DirectWrite
        +-- EGL / GLES TextureMapper (Coordinated Graphics)
        +-- libcurl + OpenSSL
        +-- WP8.1 Media Object (video)
```

Build profile inferred from the published CMake files:

- `WP81` is a first-class switch, not a downstream hack. It changes definitions,
  feature defaults and the graphics backend.
- `ENABLE_WEBKIT` and `ENABLE_WEBKIT_LEGACY` are both **off** under `WP81`. The
  port bypasses the WebKit2/WebKitLegacy API layers and embeds WebCore through
  its own shell. Apotheosis makes the same architectural choice: its patch set
  contains zero `Source/WebKit/` files and talks to WebCore through `Src/port`.
- Graphics: `USE_COORDINATED_GRAPHICS`, damage tracking, async and kinetic
  scrolling, `USE_TEXTURE_MAPPER`, EGL/GLES instead of ANGLE.
- Video: `ENABLE_VIDEO` and `ENABLE_MEDIA_SOURCE`, with the release notes naming
  the native WP8.1 Media Object pipeline for 720p/60 playback.
- Text: Skia + DirectWrite.
- Network: libcurl with OpenSSL; the release notes credit modern certificates
  and persistent cookies for restoring sites that old WP8.1 browsers lost.

## 4. Relation to Apotheosis

MetroSurf is a **sibling port**, not a descendant. No Apotheosis string, file or
comment appears in the public tree, and there is no shared shell code. Both
projects start from the same upstream WebKitGTK line and solve the same class of
problems independently.

| | Apotheosis | MetroSurf |
|---|---|---|
| Upstream tag | `webkitgtk-2.52.4` | `webkitgtk-2.54.1` |
| Upstream commit | `7acdf5e1` | `628d6ce1` |
| Publication form | one 110-file patch | 122 overlaid files |
| Custom engine files | `Src/port` (in app repo) | `webkit-open/` (53 files) |
| API layer | custom WebCore port | custom WebCore port, `ENABLE_WEBKIT` off |
| Shell | UWP XAML host in this repo | `wkwp`, not published |
| Release | x64/ARM MSIX | ARM APPX, 41.5 MB |
| Target floor | Windows 10 19041 UWP | WP8.1 / W10M |

The version gap is two minor release cycles (2.52 to 2.54). A patch cannot be
moved between them mechanically; any borrowed change needs a rebase against the
local tree and a measured symptom.

### File-level overlap

Nineteen basenames occur in both change sets:

```
arm.rb
ARMv7Assembler.h
BitmapTexture.cpp
CMakeLists.txt
CurlContext.cpp
EventHandler.cpp
GLContext.cpp
JITInlineCacheGenerator.h
jsc.cpp
MachineContext.h
MacroAssemblerARMv7.cpp
MemoryFootprintWin.cpp
neon.h
PlatformDisplayWin.cpp
ResourceHandle.cpp
TextureMapper.cpp
TextureMapperGLHeaders.h
ThreadingWin.cpp
VMAllocate.h
```

The overlap is a basename comparison, not textual identity. It is still useful:
it marks the areas where two independent ports both had to depart from upstream.
The pattern is coherent:

- **JavaScriptCore ARMv7**: assembler, macro-assembler, inline-cache generator,
  machine context, `neon.h`, `VMAllocate`. This is the Lumia execution path.
- **Event and input**: `EventHandler.cpp`; both ports had to adapt pointer and
  touch semantics to the Windows event model.
- **Graphics**: `GLContext`, `BitmapTexture`, `TextureMapper`,
  `TextureMapperGLHeaders`, `PlatformDisplayWin`. Both use the GL/EGL
  TextureMapper path rather than a platform compositor.
- **Network**: `CurlContext`, `ResourceHandle`.
- **Memory and threads**: `MemoryFootprintWin`, `ThreadingWin`.

Convergence on the same files is evidence of shared platform pressure, not of
shared code.

## 5. Build and run status

**Runtime: works.** `v2.0.1` ships a real ARM APPX, and the release notes describe
a functioning WebKit browser with GPU rendering, video, modern certificates and
persistent cookies. A public issue reports a YouTube black screen on an HP Elite
X3; that is consistent with a device-specific GPU/video path, not a dead build.

**Source: not buildable from the repository alone.**

1. No `wkwp` target and no host implementation of the `WPOpenHooks` table.
2. No README, build script, toolchain preset or dependency manifest.
3. `WP81_ROOT` layout does not match the published tree.
4. No license, so redistribution and derivative publication are not granted.
5. One commit with no history; the relationship between the published source and
   the shipped `v2.0.1` binary cannot be audited.

The release notes themselves name three known limits: 512 MB devices can blank
the page under GPU pressure, Instagram Reels fails on missing Media Object APIs,
and some sites crash instead of showing an error page because JavaScript APIs are
missing. These are the ordinary edges of a young port, stated honestly.

## 6. Strengths

- **A real compatibility leap on a dead platform.** Replacing the IE engine with
  WebKit 2.54.1 restores sites that had stopped working on WP8.1, including
  certificate and cookie behaviour.
- **Graphics work is deliberate.** Coordinated Graphics, damage tracking, EGL
  and a GL TextureMapper show a coherent GPU design rather than a checkbox port.
- **Video is treated as a platform feature.** Reusing the WP8.1 Media Object
  keeps 720p/60 playable on low-end hardware instead of forcing software decode.
- **The engine/host seam is explicit.** `WPOpenHooks` keeps optional host services
  out of the engine's link graph.
- **Storage is complete for its generation.** Local storage, Web SQL,
  IndexedDB and CacheStorage are all represented in `webkit-open`.
- **Small surface.** 175 changed or added engine files is a reviewable drop,
  especially for a port of this scope.
- **It ships.** A 41.5 MB APPX with a documented dependency list and 280
  downloads is more evidence than most hobby ports produce.

## 7. Risks and limits

- **No license.** The repository grants no reuse rights. Code cannot be copied
  into Apotheosis; only ideas and public API/ABI patterns can be studied. This
  is the first constraint on every recommendation below.
- **Incomplete publication.** Without `wkwp`, the host integration cannot be
  reproduced or reviewed. The build guards prove a shell exists; they do not
  show how it works.
- **Source/binary gap.** A single commit dated the same day as the release does
  not prove that the APPX was built from this tree.
- **Maintenance is one person deep.** One commit, one release, one open issue.
  There is no roadmap, no security policy and no update channel.
- **Platform coupling.** The Media Object video path and `WP81` storage hooks are
  tied to WP8.1 contracts. They do not transfer to a UWP 19041 target without a
  compatibility layer.
- **Security posture is unknown.** A recent WebKit is a good start, but there is
  no sandbox description, no CVE process and no statement about how engine
  updates will be delivered after the initial drop.
- **Derivative publication is sensitive.** The upstream maintainer objected to a
  third-party fork strongly enough that it was removed. Treat the repository as a
  read-only reference and do not republish its files.

## 8. Reusable ideas, ranked for Apotheosis

### 8.1 `memoryFootprint()` on WP81 - highest value, smallest change

`webkit/Source/WTF/wtf/win/MemoryFootprintWin.cpp` has a `WP81` branch that
bypasses `OpenProcess` entirely:

```cpp
PROCESS_MEMORY_COUNTERS_EX counters { };
counters.cb = sizeof(counters);
if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    return 0;
return counters.PrivateUsage;
```

Apotheosis M5.1 records that `memoryFootprint()` returns 0 in the App Container.
The likely cause is the non-WP81 path's `OpenProcess(PROCESS_QUERY_INFORMATION)`,
which the container denies. Calling `K32GetProcessMemoryInfo` on
`GetCurrentProcess()` avoids that open. `PrivateUsage` is commit charge rather
than private working set, so it is a different number, but an honest non-zero
number is exactly what M5.1 needs.

**Next probe:** call `K32GetProcessMemoryInfo(GetCurrentProcess(), ...)` from the
Apotheosis port layer on the x64 bench and log the result. Only if it is non-zero
should the WTF branch be ported. Cost: a small diagnostic build, no engine
rebuild.

### 8.2 JavaScriptCore ARMv7 cluster - second value, medium cost

The overlap names `ARMv7Assembler.h`, `MacroAssemblerARMv7.cpp`,
`JITInlineCacheGenerator.h`, `MachineContext.h`, `neon.h` and `VMAllocate.h`.
These are the files most likely to contain ARMv7-specific fixes for crashes or
slow paths on Lumia hardware.

**Next probe:** diff each against the local 2.52.4 copy, read only the hunks that
change generated code or context switching, and require a measured symptom before
backporting. Do not bulk-merge; 2.54 assembler changes may depend on
interpreter/DFG changes that 2.52 does not have.

### 8.3 Storage cluster - reference for M2 and future IndexedDB

`StorageAreaImpl`, `StorageNamespaceImpl`, `StorageSyncManager`, `StorageThread`,
`StorageTracker`, `WebDatabaseProvider` and `WebStorageNamespaceProvider` show a
complete Windows Phone storage stack, including the hooks the host must supply.
Apotheosis already has a durable localStorage backend (`PortStorageArea`), so the
value here is architectural: how origins, quotas, sync threads and trackers are
separated.

**Next probe:** read `StorageAreaSync` and `StorageTracker` before designing
IndexedDB; compare their origin/quota model with `PortStorageArea`.

### 8.4 `WPWebSocketTask` - only if WebSockets enter the roadmap

`WPWebSocketTask` bridges WebCore's socket channel to a WinRT task and forwards
cookies through `WKWP::Hooks::storageSession()`. That is the pattern to copy if
Apotheosis adds WebSockets: keep the cookie jar as the single source of truth and
let the platform task own the transport. No need to act while WebSockets are out
of scope.

### 8.5 `WPOpenHooks` - design reference for optional services

Apotheosis already exports 52 driver functions. `WPOpenHooks` is a narrower idea:
a small table of function pointers for services the engine cannot implement
itself (IndexedDB directory, quota request, storage session). If the export
surface keeps growing, grouping optional services into one versioned hook table
would make the ABI easier to evolve than adding flat exports.

### 8.6 Multimedia: the WP81 media-engine seam

This is the most interesting part for a future Apotheosis video feature, even
though the player itself is not published.

`webkit/Source/WebCore/platform/graphics/MediaPlayer.cpp` adds only one
WP81-specific symbol:

```cpp
#if defined(WP81)
MediaEngineRegister* g_wp81RegisterMediaEngines;
#endif
```

and `buildMediaEnginesVector()` calls it after the platform engines:

```cpp
#if defined(WP81)
    if (g_wp81RegisterMediaEngines)
        g_wp81RegisterMediaEngines(addMediaEngine);
#endif
```

The contract is in `MediaPlayer.h`: `MediaEngineRegister` is
`void(MediaEngineRegistrar)`, and `MediaPlayerFactory` supplies `identifier()`,
`createMediaEnginePlayer()`, `getSupportedTypes()` and
`supportsTypeAndCodecs()`. The shell sets the pointer and registers a factory
whose private player wraps the WP8.1 Media Object. `OptionsWin.cmake` confirms
the split: under WP81, `USE_MEDIA_FOUNDATION` is **off** while `ENABLE_VIDEO` and
`ENABLE_MEDIA_SOURCE` are **on**.

What is missing: the `MediaPlayerPrivateInterface` implementation, i.e. the
actual decode/present/seek/audio path. That lives in `wkwp`, which is not
published. The release notes describe 720p/60 playback through Media Object;
issue #2 (black YouTube video on an Elite X3) shows the path is device-sensitive.

**Why this matters for Apotheosis.** Our build currently has
`ENABLE_VIDEO=OFF` and `ENABLE_MEDIA_SOURCE=OFF`, and every page-setup path calls
`settings().setMediaEnabled(false)`. We do not need MetroSurf's WP81 global:
WebKit 2.52.4 already exports
`MediaPlayerFactorySupport::callRegisterMediaEngine(MediaEngineRegister)`. A
future port can register a factory from `Src/port` at startup and implement
`MediaPlayerPrivateInterface` on a UWP backend (WinRT `MediaPlayer` /
`MediaPlayerSurface`, or Media Foundation `IMFMediaEngine`). WebCore keeps
engine selection and the HTML media element; the port owns decode and present.
That is a clean seam and requires no WebCore surgery.

**Scope warning.** This is not a one-week feature. A realistic ladder is:

1. Enable `ENABLE_VIDEO` with a stub player and fix the build/runtime fallout.
2. Progressive MP4/H.264/AAC: play, pause, seek, volume, basic buffering.
3. Lifecycle, fullscreen, surface resize and audio focus.
4. MSE only if YouTube-class sites become a requirement. MSE is a separate
   implementation surface, and the public MetroSurf tree does not expose its
   `MediaSourcePrivate` backend. Treat "YouTube works" as a research item, not a
   deliverable.

The WP8.1 Media Object itself is not the API to target from our UWP 19041
baseline; it is prior art for the *shape* of the solution (native decoder behind
a `MediaPlayerFactory`), not a drop-in backend.

### 8.7 Lower priority

- **CacheStorage**: useful only when offline cache becomes a roadmap item.
- **CoordinatedSceneState / damage tracking**: conceptually relevant to M4/M6,
  but the 2.54 compositor API will not drop into 2.52.

## 9. Recommendations

1. **Do not import code.** There is no license. Use the repository as a public
   design reference only.
2. **Run the memory probe first.** It is the only idea with a direct, measurable
   payoff for an open M5.1 gap and it does not require a WebKit rebuild to test.
3. **Then diff the JSC ARMv7 files.** Accept only changes tied to a reproduced
   Lumia symptom; the version gap makes speculative merges expensive.
4. **Leave storage, sockets and compositor as reading material.** They inform
   design, not the next build.
5. **Keep the fork incident out of the technical record.** The public repo is the
   only citable source; no removed fork content should enter Apotheosis.
6. **Do not target WP8.1.** The test fleet is W10M-class (Lumia 950/640; peers on
   Elite x3 / 950 XL). MetroSurf's 512 MB notes are evidence about where the GPU
   path breaks, not a hardware target. No WP8.1 toolchain work, no 512 MB tuning,
   no WP8.1 packaging.
7. **No fork, mirror or code copy.** The repository has no license and the
   maintainer has already objected to a derivative publication. Keep it as a
   read-only public reference; do not republish its files or resurrect removed
   forks.

### 9.1 Revisit triggers

Return to the source only when one of these is true:

- M5.1 still reports `memoryFootprint() == 0` and the
  `K32GetProcessMemoryInfo` probe is the cheapest next step.
- A Lumia-only JSC crash or stall is reproduced and an ARMv7 file diff can be
  tied to that symptom.
- Video becomes a product requirement on a core site; then read section 8.6 and
  prototype the factory seam, not the WP8.1 player.
- Storage work reaches IndexedDB or quota handling; then read the storage
  cluster.

Otherwise this document is reference material, not a backlog.

## 10. Evidence

| Claim | Source |
|---|---|
| Base tag and commit | `about.txt` in `MetroSurf/MetroSurf` |
| 176 files total; 122 / 53 split | extracted public tree |
| `WP81`, `ENABLE_WEBKIT` off, graphics flags | `webkit/Source/cmake/OptionsWin.cmake`, `OptionsMSVC.cmake` |
| `WP81_ROOT` / `wkwp` expectation | `webkit/Source/CMakeLists.txt` lines 67-70 |
| `WPOpenHooks` table | `webkit-open/WPOpenHooks.h` |
| `memoryFootprint()` WP81 branch | `webkit/Source/WTF/wtf/win/MemoryFootprintWin.cpp` |
| WP81 media-engine hook | `MediaPlayer.cpp` / `MediaPlayer.h`, `OptionsWin.cmake` |
| Apotheosis media support off | `build-x64-gpu/CMakeCache.txt`, `Src/port/*.cpp` `setMediaEnabled(false)` |
| Release assets and notes | GitHub releases API, `v2.0.1` and `v2.0` |
| YouTube black screen on Elite X3 | issue #2, opened 2026-10-26 |
| Apotheosis base and patch size | `WebKitPatches/README.md`, `apotheosis-webkit-2.52.4.patch` |
| 19-file basename overlap | local comparison of the patch file list and the MetroSurf tree |

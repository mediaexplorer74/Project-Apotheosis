# Apotheosis WebKit patch set

This directory contains the **only** WebKit source that is part of this repository.
The engine itself is not vendored: `WebKit/` is a multi-gigabyte checkout and is
ignored by git. The port is published as a small, reviewable patch against an exact
upstream release.

| | |
|---|---|
| Patch | `apotheosis-webkit-2.52.4.patch` |
| Upstream tag | `webkitgtk-2.52.4` |
| Upstream commit | `7acdf5e11af175e98cf7dcfcbcfcde11a50bc6ce` |
| Files changed | 110 (100 modified + 10 new) |
| Patch size | 287,687 bytes |
| Applies to | a clean checkout of the tag above |

The patch changes only `Source/`, not `Tools/`. The working tree used to generate
it also contained six `Tools/` symlink-to-regular-file conversions; those are local
Windows checkout noise and are deliberately excluded from the patch set.

## Fetch, checkout and apply

The supported path is the repository script:

```powershell
pwsh -File Src\tools\fetch-webkit-source.ps1
```

It performs a sparse, shallow, blobless fetch of the exact tag, checks out only the
`Source/` directories touched by the port, and applies the patch. Re-running it is
safe: an already-patched tree is detected with `git apply --check --reverse`.

Manual equivalent:

```powershell
git clone --filter=blob:none --no-checkout --depth 1 --branch webkitgtk-2.52.4 `
    https://github.com/WebKit/WebKit.git WebKit
git -C WebKit sparse-checkout init --no-cone
git -C WebKit sparse-checkout set `
    Source/WebCore Source/JavaScriptCore Source/WTF Source/WebKitLegacy `
    Source/cmake Source/bmalloc Source/ThirdParty Source/Tools `
    Source/CMakeLists.txt Source/PlatformWinUWP.cmake CMakeLists.txt
git -C WebKit checkout webkitgtk-2.52.4
git -C WebKit apply --3way ..\WebKitPatches\apotheosis-webkit-2.52.4.patch
```

Check the result:

```powershell
git -C WebKit apply --check --reverse ..\WebKitPatches\apotheosis-webkit-2.52.4.patch
git -C WebKit status --short --untracked-files=all
```

The reverse check must succeed on a patched tree. `git status` should show the
patch files (110 paths), not the whole engine.

## What the patch contains

The patch is the Windows 10 Mobile/UWP platform layer for WebKitGTK 2.52.4:

- `Source/PlatformWinUWP.cmake`, `Source/cmake/OptionsWinUWP.cmake` and the
  per-component `PlatformWinUWP.cmake` entry points.
- WTF: WinRT threading, run loop, file system, time, memory pressure, assertions
  and stack traces.
- JavaScriptCore: ARMv7 assembler/JIT fixes, WinUWP CMake wiring, the `jsc` shell
  hook, and the `VM`/watchdog changes needed by the UWP runtime.
- WebCore: URL loading through the curl backend, `ResourceHandle`, loader and
  subresource tracing, cairo/ANGLE presentation, font handling, timers, image
  decoders (PNG/JPEG/WebP), and the small rendering/layout fixes recorded in
  `Doc/Archive/`.
- bmalloc and unifdef build wiring.

It does not add the `CookieJarDB` implementation itself: that lives in the
Apotheosis port layer (`Src/port`), not in WebKit. The patch only makes the
platform build and leaves the storage backend selected by the driver.

## Regenerating the patch

After intentionally changing the WebKit working tree, regenerate from the exact
base commit. New files must be marked intent-to-add or `git diff` will omit them:

```powershell
# From the repository root. Review WebKit status first; exclude Tools/ noise.
git -C WebKit add -N (git -C WebKit ls-files --others --exclude-standard -- Source)
git -C WebKit diff --no-ext-diff --binary 7acdf5e1 -- Source > WebKitPatches\apotheosis-webkit-2.52.4.patch
git -C WebKit reset
```

Then verify against a clean copy or at minimum with the reverse check above. Do not
add build outputs, `.git` data, downloaded dependencies, or `Tools/` symlink noise
to the patch.

## Licensing

The patch is a set of modifications to WebKit and is distributed under the same
upstream licenses (LGPL-2.1 for WebCore and the BSD-style licenses of WTF,
JavaScriptCore and bmalloc). The Apotheosis port layer in `Src/` is MIT; see the
root `LICENSE`. This repository does not redistribute the WebKit source tree or
prebuilt WebKit binaries.

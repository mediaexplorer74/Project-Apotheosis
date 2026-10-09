# Symbols that look available from the port layer and are not

Apotheosis-specific. Written 2026-09-18, after `WebCore::Frame::page()` cost a build.

## The rule

The port layer (`Src/port/*.cpp`, clang-cl → `WebCoreDriver-gpu-<arch>.lib`) is **not** part of
WebCore. It links WebCore's **import library** (`build-x64-gpu/lib/WebCore.lib` for x64), so the only
WebCore symbols it can reference are the ones WebCore actually exports:

* **Available:** anything marked `WEBCORE_EXPORT` in a WebCore header.
* **Available:** anything **defined inline** in a header — a template, or an `inline` function such
  as `LocalFrame::protectedDocument()` in `LocalFrameInlines.h`. No symbol is needed, so no export is
  needed.
* **Not available:** an out-of-line function that is *not* `WEBCORE_EXPORT`ed, however plainly it is
  visible in the header. It compiles — the declaration is right there — and fails at **link** time
  with `LNK2019`, in the harness build (`Src\tools\_build-appx-x64.bat`), where the archive is
  actually consumed. Neither the port's own compile nor the archive step (`lib.exe`) notices.

The same rule applies to `JavaScriptCore.dll` and `WTF`, which is how
`StackVisitor::Frame::hasLineAndColumnInfo()` cost a build earlier the same day (see the Diagnostics
section of `CLAUDE.md`).

## How to check, before writing the code

```bash
NM="/c/Program Files/LLVM/bin/llvm-nm.exe"
L="$APOTHEOSIS_ROOT/build-x64-gpu/lib/WebCore.lib"
"$NM" --defined-only "$L" | grep -o "?page@[A-Za-z]*@WebCore@@[A-Za-z0-9_@?$]*" | sort -u
```

`--defined-only` matters: the archive carries both `T ?sym` and `T __imp_?sym` entries for exported
symbols, and grepping without it will match things that are only imported. A hit means the symbol is
there; an empty result means it is not, and the header will not tell you.

## Known cases

| Symbol | Exported | Replacement |
|---|---|---|
| `WebCore::Frame::page() const` | **no** | `Page::forEachPage()` — see below |
| `WebCore::Document::page() const` | **no** | same |
| `WebCore::LocalFrame::page() const` | **no** | same |
| `JSC::StackVisitor::Frame::hasLineAndColumnInfo()` | **no** | its body is `return !!codeBlock();` — inline the test |
| `Page::forEachPage(const Function<void(Page&)>&)` | yes | — |
| `Page::group()` | yes | — |
| `StorageEventDispatcher::dispatchLocalStorageEvents` / `…SessionStorageEvents` | yes | — |
| `WebCore::SecurityOrigin::isolatedCopy()` | yes | — |

`Src/port/PortStorage.cpp` is the worked example of the `Page::forEachPage` replacement: with a null
`PageGroup` for local storage (WebCore reads that as "no grouping" and notifies every page, filtered
by origin and by a source-window identity test — which is the spec for an area shared by all
same-origin pages), and a single-page walk for session storage. Nothing is held between calls, so
there is no pointer to go stale when a `Page` is destroyed.

## What this is not

It is not a statement that the missing symbol is private. `Frame::page()` is an ordinary public
accessor. It is simply not part of the export set WebCore was built with, and the port cannot add to
that set from its side — the choice is a supported replacement or an upstream `WK_WINUWP` patch that
adds `WEBCORE_EXPORT`, and the patch must be recorded in the upstream patch list.

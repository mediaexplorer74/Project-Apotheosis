# Src/Apotheosis — the C# UWP app (the shipping line)

**Status: this is the product.** The C# app drives the engine through the C ABI of
`WebCoreDriver.dll`. On 2026-09-26 it reached the first rendered page: the home page draws its CSS
gradient, rounded cards, shadows and grid, and a real site loads with a parsed title, a live JS
context and no network errors. The C++/CX harness in `Src/harness` is kept as a legacy UI source
and a native reference point; nothing in the shipping app depends on it.

> **What this project is not.** It is not a port of WebKit to C#. The engine stays native C++,
> compiled to `WebCoreDriver.dll`; only the *shell* around it — window, address bar, tabs, settings,
> diagnostics — is C#/XAML. Rewriting the engine itself in C# would be a multi-year effort and
> nothing here is aimed at that. See [Architecture boundary](#architecture-boundary) below for the
> exact line between the two.

This is a deliberately reduced C#/XAML app: address bar and content view. It is not a copy of
`Src/harness` and not a drop-in replacement for it.

## Why it exists

C# would give us readable UI code, catchable managed exceptions, and a smaller XAML surface. The
measured objections to managed marshalling are not currently a reason to declare the idea impossible:
blittable `DllImport` calls are cheap relative to a WebCore tick, and a pinned byte buffer can pass a
frame without an extra managed copy. The unknown is the UWP toolchain, especially ARM32 .NET Native.

The native harness also contains engine-thread plumbing, job labels, heartbeat diagnostics, packaged
test switches, navigation guards, and the GPU/software fallback. Replacing it with a small C# UI would
lose those measurements until they are deliberately ported.

## Architecture boundary

The C# project talks to a **C ABI**, not to WebKit static libraries. The engine is native C++; only the
shell is managed:

```text
C# UWP app  (C#/XAML — window, address bar, tabs, settings, diagnostics)
    ↓ P/Invoke, 30 declarations in WebCoreDriver.cs
WebCoreDriver.dll  (native, the whole engine statically linked in)
    ↓
JavaScriptCore.dll  (the only engine DLL still loaded at runtime)
```

`WebCore.dll` **is no longer part of the package.** Since 2026-09-26 the driver links WebCore
statically through `WebCoreFull-x64-19041.lib` / `WebCoreFull-arm32.lib`, because `stubs-other.cpp`
needs WebCore internals that the DLL does not export (`RenderTheme::RenderTheme()` and
`SystemFontDatabase::SystemFontDatabase()` are `protected` constructors, and
`WebKitFontFamilyNames::standardFamily` is a data global). An `__imp_` thunk is only generated for an
*exported* name, so those can never resolve at load time. Removing `WebCore.dll` saves 44 MB on x64.

`WebCoreFull*.lib` is a native link input, never a C# reference. Do not copy WebKit `.lib` files into
`Apotheosis.csproj`.

A managed probe can be useful for a harmless C ABI round-trip and package check. It is not proof that a
site paints until the real WebKit runtime is loaded and the frame is inspected.

## What the project currently contains

- `App.xaml`, `MainPage.xaml`, and managed UI code;
- `WebCoreDriver.cs` with the 30-call managed ABI surface;
- SDK 19041 UWP project settings and x64/ARM configurations;
- a certificate reference and package assets, but no verified `WebCoreDriver.dll` packaging path;
- no project reference or native library reference to WebCore.

`Src/Apotheosis.sln` contains this project alone. `Apotheosis-mix.sln`, when present, is a visual
research solution only: opening it does not mean that Build Solution produces the browser. The engine
is still built separately with CMake/Ninja/clang-cl, and the driver DLL must be built and packaged
before a managed runtime probe is meaningful.

## If resuming it

0. **Measured state 2026-09-26 (0.2.0.6, x64): builds, installs, runs, and paints.** The home page
   renders its gradient, rounded cards, shadows and grid; `example.com` loads with `title=Example
   Domain`, `js=1/1`, `err=0`. The null call that used to kill the process inside `Frame::init()`
   (`WCRH:frame-init`, faulting address `WebCoreDriver.dll+0x0`) is gone.
   **The open defect is that no text renders.** Shapes, colours and layout are correct; every glyph
   is missing, so a white page with black text looks blank. The linked image contains FreeType in
   full and HarfBuzz barely, but **no fontconfig at all** — and for x64 there is nothing to add,
   because fontconfig will not cross-compile to UWP. See [Doc/PLAN.md](../../Doc/PLAN.md) §0b.
1. ~~Finish the current x64 SDK 19041 WebKit build.~~ Done: `build-x64-gpu`, 1547/1548 objects.
2. ~~Build an empty C# UWP ARM32 package.~~ Superseded: the ARM32 driver DLL now builds
   (`unresolved=0`, 30 exports) and `native\ARM` is staged with 21 audited files. The appx for ARM has
   not been built yet.
3. ~~Build a real `WebCoreDriver.dll` for the chosen architecture.~~ Done for both.
4. ~~Verify architecture, package identity, AppContainer deployment, ABI round-trip.~~ Done for x64:
   the SHA256 of `WebCoreDriver.dll` **inside the appx** is verified against the link output.
5. ~~Connect the UI to `WebCoreSessionLoad` / `WebCoreLiveTick` / the paint buffer.~~ Done.
6. **Next:** give WebKit a working font back-end so text renders. Everything else is subordinate to
   this, because a browser that draws boxes and no letters is not a browser.
7. Port engine-thread plumbing, job queue, heartbeat, navigation state, before calling the managed
   app a complete browser.

## Measurement honesty

A compile-only C# success means "managed project can be compiled." It does not mean WebKit works, ARM32
deployment works, or a page painted. Those are separate acceptance gates and must be reported as such.

The native line remains the shipping path:

```text
WebKit CMake/Ninja → WebCore/JavaScriptCore → port archive → C++/CX Harness.vcxproj → appx
```

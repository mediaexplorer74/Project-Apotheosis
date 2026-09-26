# Apotheosis C# Shell — Skeleton

This is a starting skeleton for the C++/CX → C# migration discussed for
Project-Apotheosis. It is **not** a drop-in replacement yet — every `TODO`
marks a spot that needs the real `port/WebCoreDriver.h` C ABI wired in.

## Folder layout

```
Apotheosis.EngineWinRT/         C++/WinRT bridge component (native)
    EngineSession.idl           WinRT contract — the API surface C# sees
    EngineSession.h / .cpp      Implementation — thin wrapper over WebCoreDriver

Apotheosis.Shell/                C# UWP shell (what you'll spend time in)
    App.xaml(.cs)                 Standard bootstrap, no engine logic
    MainPage.xaml(.cs)             Chrome: toolbar, address bar, status row
    Controls/EngineHost.xaml(.cs)  Owns the SwapChainPanel + EngineSession
    ViewModels/BrowserViewModel.cs Chrome state, no WinRT types in scope
```

## The one rule this skeleton is built around

**`port/` stays untouched.** `EngineSession` calls into WebCoreDriver's
existing C ABI — it does not reimplement, wrap, or "improve" any of it. If a
method on `EngineSession` doesn't have an obvious native counterpart yet,
that's a sign `WebCoreDriver.h` needs a new export, not that C++/WinRT logic
should grow to compensate.

## What to fill in first (in order)

1. **`EngineSession.cpp`'s `extern "C"` block** — replace the illustrative
   `WebCoreDriver_*` declarations with `#include "WebCoreDriver.h"` and fix
   any signature mismatches. This is the single highest-value fix; everything
   else in the skeleton assumes these names are roughly right.
2. **`SetSwapChainPanel` / the swap chain retrieval in `Resize`** — port the
   exact `ISwapChainPanelNative::SetSwapChain` call site from the existing
   C++/CX harness. Don't re-derive it; copy it.
3. **Validate the dispatcher hop** — build a trivial test (e.g. log from
   `HandleNativeTitleChanged` before and after the `TryEnqueue`) and confirm
   you're actually off the engine thread before the WinRT event fires. This
   is the exact bug class `v0.1.7.29` fixed for text input/image loading —
   worth being paranoid about here too.
4. **Wire this into the existing C++/CX harness first, before touching UI.**
   Have the *current* harness call through `EngineSession` instead of
   `WebCoreDriver.h` directly. If the app still works identically, the bridge
   is solid and you can build the C# `MainPage` with confidence. If something
   breaks, you've isolated it to the bridge, not tangled it with a UI rewrite.
5. Only then build out `Apotheosis.Shell` for real and deploy to a Lumia.

## Known gaps in this skeleton (intentional, not oversights)

- No favorites/history/downloads — chrome-only skeleton, add as its own
  view model once navigation itself is proven.
- No error page for failed navigations (`NavigationCompleted.IsSuccess == false`
  currently no-ops — flagged with a TODO in `MainPage.xaml.cs`).
- No tab support — `EngineHost` is written to make multi-instance plausible
  later, but `MainPage` only ever creates one.
- `ManipulationDelta.Scale` → `OnPinchZoom`'s `scaleDelta` parameter mapping
  is assumed, not verified against the native pinch-zoom implementation —
  check this against the harness's existing zoom-anchor-drift fix before
  trusting it.

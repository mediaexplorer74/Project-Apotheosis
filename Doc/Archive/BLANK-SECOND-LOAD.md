# The blank second load: force-compositing removed the software fallback

One line of driver code made every page after the first in a session paint nothing. Found on the Lumia,
reproduced on the x64 bench, fixed and verified there on 2026-08-21. This file exists because the symptom
looked like three different bugs before it turned out to be one, and because the reproduction technique
is reusable.

## Symptom

The first page loaded in a session rendered correctly. Every load after it produced a fully blank frame
while the document itself was perfect: `readyState` Complete, `pending=0`, correct layout height,
stylesheets loaded, scripts executed. Only the pixels were missing.

Measured on the bench, three navigations in one session:

```
example.com  (first)          nonwhite=710656/710656  rs=C pending=0
example.com  (second)         nonwhite=0/710656       rs=C pending=0   <- same page, blank
news.ycombinator.com          nonwhite=0/710656       rs=C pending=0
```

On the device the same shape appeared, which is what first suggested a site-specific rendering problem —
Hacker News was blank, so HN looked guilty. It was not: `example.com` painted as the first load and went
blank as the third. **The variable was position in the session, not the page.**

Earlier the same defect had a more violent face. Before the harness stopped re-navigating automatically
after GPU init, that self-inflicted second load killed the process outright on the Lumia, with no
`UEF:` line and no minidump. Removing the automatic reload turned a crash into a blank frame — same
boundary, quieter symptom.

## Mechanism

`WebCoreSessionLoad` does not reuse sessions: it calls `teardownSession()` and builds a new one on every
load. So each load inherits the value of `g_gpuActive` *at that moment* — and the timing is what matters:

```
09:25:16  navwatch -> example.com                    load 1 built with g_gpuActive == false
09:25:18  EnableGpu: WebCoreGpuInit returned 0       g_gpuActive := true
09:25:18  EnableGpu: first frame after init: EnableCompositing=0 Composite=-12
09:25:41  navwatch -> example.com                    load 2 built with g_gpuActive == true  -> blank
09:26:06  navwatch -> news.ycombinator.com           load 3 built with g_gpuActive == true  -> blank
```

The session build applied:

```cpp
page->settings().setAcceleratedCompositingEnabled(g_gpuActive);
page->settings().setForceCompositingMode(g_gpuActive);      // <- the defect
```

With force-compositing on, WebCore promotes all content into `GraphicsLayer`s. The ordinary `FrameView`
paint that fills the `outRGBA` buffer then has nothing left to draw, so the software present shows a
blank frame. And the GPU present cannot show the layers either: `WebCoreEnableCompositing()` returns 0
(no root layer) and `WebCoreComposite()` returns `kErrNoSession` (-12). The content left the software
path without ever arriving on the GPU one.

## Fix

```cpp
page->settings().setForceCompositingMode(false);
```

`setAcceleratedCompositingEnabled(g_gpuActive)` stays: it *permits* layers, so a page that needs one gets
one and the TextureMapper path can present it. Forcing them is what removed the fallback, and it
contradicted this port's own rule that software rendering is the base and GPU is a runtime opt-in.

Verified with the identical three-navigation sequence:

```
example.com  (first)          nonwhite=710656/710656  PAINTED
example.com  (second)         nonwhite=710656/710656  PAINTED
news.ycombinator.com          nonwhite=587134/710656  PAINTED
```

Note what did **not** change: `Composite` still returns -12 and the harness still logs "staying on
software present". That is the point. The GPU present path was never delivering frames; the defect was
that it took the software path away while failing to replace it.

Left for later, and the honest version of this decision: `g_gpuActive` means "the GPU was initialised at
some point", which is the wrong fact to base a compositing decision on. The right one is "we are
presenting through the GPU right now", a flag set only after a successful `WebCoreComposite`.

## How it was reproduced, and why the obvious tool could not do it

`autodiag.txt` loaded example.com, example.com, Hacker News and example.com in one session and **all four
painted**. That result is not a contradiction — it is the clue. `autodiag` calls `WebCoreSessionLoad`
directly and initialises the GPU offscreen with `WebCoreGpuInit(nullptr, ...)`, so it never runs the
harness navigation path and never has a real `SwapChainPanel`. A defect living in that path is invisible
to it.

So the harness path was made scriptable instead: **write a URL into `LocalState\nav.txt` and the app
navigates to it**, going through `NavigateTo`, the parking logic, `NavRetry` and the GPU-enable callback.
The watcher polls the file's last-write time once a second, and the stamp is seeded at startup so an
existing file does not fire on launch — `nav.txt` means "go here now", while `testurl.txt` means "go here
at startup". With that, the whole sequence runs with no finger on the screen, on the bench or on the
phone over Device Portal.

The general lesson, which cost most of the two days: **a diagnostic harness that bypasses the code under
test proves nothing about it.** Four green loads through `autodiag` sat next to a blank screen on the
device for hours before that was noticed.

### The same trick does not work on the device: this Device Portal cannot write files

`nav.txt` is easy to drive on the bench, where LocalState is an ordinary directory. Over WDP it is not
possible at all: the file-upload endpoint fails on this OS build (15254.603, Windows 10 Mobile RS3).
Measured 2026-08-21 against the installed package:

```
POST /api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=<pfn>&path=%5CLocalState  -> HTTP 500
POST ... &path=LocalState                                                                          -> HTTP 500
POST ... &path=                                                                                     -> HTTP 400
```

The matching GET works fine, which is how every log in this investigation was collected — so **WDP on
this device reads but does not write**. Anything that needs to reach LocalState has to arrive another
way, and the only channel that does arrive is the appx itself.

That points at the shape a device-side test driver has to take: **package the sequence**. A list of URLs
in `Assets\`, copied to LocalState on first run and played back through the harness navigation path with
a delay between entries, would make a device run unattended end to end — deploy, start from the tile,
read the log afterwards. That is what `autodiag.txt` should have been; it drives the engine directly
instead and therefore cannot see defects in the harness path. Not built yet, and deliberately not built
in the same cycle as the fix it would have been verifying.


## Carrying it to ARM32

The fix is in shared code, so ARM needs only a relink, a package and a deploy — no engine rebuild. See
the task list; the confirmation to run on the Lumia is example.com twice and Hacker News, expecting all
three to paint.

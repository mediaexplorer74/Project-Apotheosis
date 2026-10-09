# Revenant: the other W10M WebKit port, and what to take from it

<https://github.com/Starkka15/revenant-browser-w10m> — an independent port of **WebKit 2.36.8**
(WinCairo) to Windows 10 Mobile ARM32 UWP, tested on a Lumia 640 XL. Early alpha, basic browsing
works, well received on 4PDA and Reddit for speed and stability. Read on 2026-08-19.

Its author studied Apotheosis's public approach but did not fork it, and their README states the
differences explicitly. That makes it the most useful external reference this project has: an
independent second data point on the same hardware class, with different choices at almost every
decision we also faced.

## How the two differ, per their README

| Area | Revenant | Apotheosis (this fork) |
|---|---|---|
| Engine | WebKit 2.36.8 | webkitgtk 2.52.4 — newer |
| Toolchain | MSVC **v142**, no clang | clang-cl + MSVC v143 |
| Windows SDK | 10.0.16299.0, min 10.0.15063.0 | 10.0.19041.0, min 10.0.14393.0 |
| LLInt | **hand-written MSVC-ARM32 assembler backend** (WebKit never shipped one) | the existing offline-asm path via clang |
| JIT | DFG, W^X via `VirtualAllocFromApp` / `VirtualProtectFromApp` | JIT incl. FTL; same FromApp W^X approach |
| Resolution | renders at the panel's **native pixel density** | fixed 720x1080 surface, then stretched |
| TLS | libcurl 8.11.1 over **BoringSSL**, HTTP/2, DNS-over-HTTPS, iOS-Safari ClientHello | libcurl over OpenSSL 3.x, packaged `cacert.pem` |
| `crypto.subtle` | implemented | stubbed — SubtleCrypto unavailable |
| Shell | minimal (address bar) | tabs, address bar, find-in-page, diagnostics page |
| Device | Lumia 640 XL (Adreno 305) | Lumia 950 |

They credit this project with "a clean readback-free direct-present fast path", and propose the merged
base out loud: Apotheosis's newer engine, fuller shell and clang-cl toolchain, plus Revenant's
device-pixel-ratio rendering and WebCrypto.

## Three of their choices worth stealing outright

1. **Native pixel density instead of a stretched fixed surface.** We render 720x1080 and scale, which
   is exactly the class of bug that cost this project the `WebCoreGpuResize` work. They render at the
   panel's real DPR. On a 1080p Lumia 950 that is both sharper and avoids the whole stretch problem.
2. **HTTP/2 plus DNS-over-HTTPS.** They measured "149 of 151 responses in a session negotiated h2",
   and DoH is not cosmetic on these devices: their HLS playback fails precisely because it goes
   through the system resolver and bypasses DoH, giving `0x80072ee7`. Our loader is HTTP/1.1 over
   OpenSSL, and hh.ru's stalled resources may be a related story.
3. **`crypto.subtle` implemented.** Ours is a stub, and modern login flows use it.

## What their bug list tells us to expect

Their open problems are a map of what lies ahead of us, since we are further behind on the device:

* **Tall media-heavy pages crash or tile badly** — fragmented, misaligned or blank tiles, sometimes
  closing the app. Their top open bug.
* **Video is H.264-only**; VP9/AV1 give a black player. They fixed a "decoded=38.3fps drawn=0.0fps"
  symptom, which is worth remembering as a distinct failure shape: decode succeeding while nothing
  reaches the screen.
* **YouTube stops after about a minute**, cause unknown, in the MSE path.
* **`window.open` / popups missing**, which breaks OAuth logins; also no file upload.
* **JS is the bottleneck, not painting**: they measure `js=2337ms update=883ms composite=243ms` in a
  worst case and conclude "JS and layout are ~93% of a worst-case frame; rasterisation is ~7%". Any
  optimisation effort here should be aimed accordingly.
* **~250 MB of native heap unattributed** under `USE_SYSTEM_MALLOC`, invisible to their pools and
  untouched by cache release. Note the contrast: we run bmalloc rather than the system allocator, so
  our accounting story is different, but the lesson that memory hides outside tracked pools stands.
* **87% of layer promotions are overlap cascades**; one page reached 50 layers and 39 MB of GPU tiles.

## Their process rules, which read as earned

Quoted because each one matches something this project has paid for:

> Read the whole log, not the lines you expect.
> Verify one change at a time on device.
> A clean compile proves nothing.
> Trace a symbol's callers before writing the call.

Their log tags: `SLOWFRAME` (where `GAP=` should be 0), `mempool:`, `cmplayer:`, `tmsched:`. Also a
practical detail we share: the log must be pulled with the app closed, because the process holds the
file open.

## Suggested R&D phase

Not a merge, and not a rewrite: a measured comparison, in this order.

1. Build their recipe and run it on the Lumia 950 alongside ours. Two engines on one device is the
   only way to compare like with like — their numbers come from a 640 XL with an Adreno 305.
2. Compare on the sites this project already tracks: hh.ru, ya.ru, dzen.ru, news.ycombinator.com. If
   their 2.36.8 finishes loading hh.ru and ours does not, the difference is in the loader, not the age
   of the engine.
3. Port their DPR rendering approach. This is the highest-value single item and it is independent of
   engine version.
4. Decide on HTTP/2 and DoH in our curl configuration, with their h2 measurement as the target.
5. Revisit `crypto.subtle`, which is a known gap here.

Worth being honest about what we have that they do not: a newer engine, a fuller shell, and — as of
2026-08-19 — a documented device-crash workflow (WDP dump plus cdb with our own PDBs) that turned an
unexplained launch failure into a named source line. See `Doc/ARM32-DANGLING-SECURITYORIGIN.md`.

---

# What we take, what we skip, and in what order

Decided 2026-08-21. Three of their choices are worth adopting, one of their measurements changes where
we should spend effort, and several of their problems are theirs alone. Each phase below states what has
to be true before it starts and what measurement closes it, so none of this depends on remembering a
conversation.

## Gate: none of this starts before the second-load defect is fixed

A browser that paints the first page of a session and nothing after it cannot be improved by features.
Adding DPR rendering or HTTP/2 on top of that would only change what the blank frame is made of, and
would make the defect harder to isolate. Fix the second load first — task #14, characterised in the
README status table — then start Phase 1.

**And a second gate, from the v1.0 definition in `Doc/PLAN.md` §0:** v1.0 is "a light browser that
somehow opens a dozen not-useless sites", with authentication and video explicitly out of scope. Measured
against that, only **Phase 2 step 1** — the cheap read of the loader counters, which may explain our own
stall — is v1.0 work. Phase 1 (DPR) makes text sharper rather than opening more sites; Phase 3
(`crypto.subtle`) serves login flows that v1.0 excludes; Phase 4 is optimisation of something that only
has to work at all. They stay in this document as the post-v1.0 queue, deliberately not started early.

## Phase 0 — write down where the two build lines disagree (half a day, no building)

Both known divergences distort measurement, so they belong on paper before any comparison is run.

1. Record the JIT divergence. Measured with the same probe on both: on the Lumia, `jitresult.txt` shows
   paths `[A]` RW→RX and `[C]` RW→RWX both returning 42, so executable memory works the way modern JSC
   needs; on x64 in the AppContainer all three paths fail with the protection applied but execution
   trapped by DEP/ACG, so x64 runs the LLInt interpreter. **The device is faster than the bench in this
   one respect.** Consequence: x64 is a mirror for correctness only, and every speed claim must be
   measured on hardware.
2. Cross-reference the text-stack divergence from `Doc/HARFBUZZ-ICU-DIVERGENCE.md`, since shaping and
   normalisation differ between the lines and both affect what "the page looks right" means.
3. Add both to the top of `Doc/UNIFICATION.md` as tracks, because they are exactly what that document
   exists to close.

Closed when: a reader can tell, from the docs alone, which measurements are valid on which line.

## Phase 1 — device-pixel-ratio rendering (the highest-value single item)

Independent of engine version, and it removes a whole family of surface-size mismatches rather than one
bug. We currently render a fixed 720×1080 buffer and let the panel stretch it; they render at the
panel's real DPR. This is the same class of defect that cost this project the entire `WebCoreGpuResize`
investigation.

1. Measure the baseline: on the device, capture `contents=WxH`, the `SwapChainPanel` size in dip, and a
   screenshot of a text-heavy page. Without this, "sharper" is an opinion.
2. Find every place the 720×1080 default is assumed: `kW`/`kH` in `MainPage.xaml.cpp`, `WebCoreGpuInit`,
   `WebCoreGpuResize`, `MapTapToEngine`, and the software blit path.
3. Introduce one scale factor sourced from `DisplayInformation::RasterizationScale` and thread it
   through the viewport the engine is told about, not through a transform applied afterwards.
4. Verify with the tap path as well as the eye: `OnPageTapped` logs `dip=` and `engine=`, and a correct
   DPR implementation keeps them consistent at every zoom level. A tap that lands in the wrong place is
   the classic symptom of getting this half right.

Closed when: text is sharp at native density, `dip=`/`engine=` agree, and a 200×200 CSS square measures
200×200 CSS pixels on screen.

## Phase 2 — HTTP/2, and then DNS-over-HTTPS (likely relevant to our own stall)

Their measurement was "149 of 151 responses in a session negotiated h2". Ours is HTTP/1.1 over OpenSSL,
and our open loader defect looks like it could be connection-limit shaped: hh.ru sits at `pending=32`
with dozens of resources stuck at status 1 forever.

1. Test the hypothesis before implementing anything: count distinct hosts and concurrent requests in a
   stalled hh.ru session from `port-trace.txt`, which already logs `serve started/inflight/pending/hosts`.
   If inflight sits at the per-host cap while pending stays high, h2 multiplexing is the fix; if not, the
   stall is elsewhere and Phase 2 is not urgent.
2. Enable h2 in the curl configuration — the library is already built with OpenSSL, so this is
   configuration plus ALPN, not a new dependency.
3. Re-measure the same page and compare `pending` over time against the recorded baseline.
4. Only then consider DoH. Their evidence that it matters is concrete: their HLS playback fails with
   `0x80072ee7` precisely because it goes through the system resolver and bypasses DoH. Ours has no
   video path yet, so this is preparation rather than a fix.

Closed when: a page that stalled at `pending=32` either finishes or is proven to stall for a reason
unrelated to connection limits.

## Phase 3 — `crypto.subtle`

Ours is a stub. Modern login flows use it, and this is the concrete cost: the dead
"Зарегистрироваться" button on hh.ru's login page is exactly the class of control that fails when a
login flow cannot compute a digest or a key.

1. Determine what is actually reached: instrument the stub to log which algorithm and operation a real
   site asks for, then visit the login pages we already track.
2. Implement only what those sites use, backed by the OpenSSL already linked in — not the whole WebCrypto
   surface.
3. Verify against the real page, not a synthetic test.

Closed when: a tracked login page gets past the point where it currently stops.

## Phase 4 — spend effort where their profile says it goes

Their worst-case frame is `js=2337ms update=883ms composite=243ms`, i.e. JavaScript and layout are about
93% and rasterisation about 7%. That contradicts where instinct sends you, and it matters directly for
the "unusably slow on lower-end Lumias" complaints around the original project.

1. Reproduce the split on our own engine with our own numbers, on the device, since x64 cannot JIT.
2. If the shape holds, aim optimisation at script execution and layout, and treat further compositor
   work as polish rather than performance.
3. Their memory finding is theirs, not ours: about 250 MB of native heap unattributed under
   `USE_SYSTEM_MALLOC`. We run bmalloc, so the accounting differs — but the transferable lesson is that
   memory hides outside tracked pools, and our own numbers (34-189 MB on real pages) should be checked
   against the OS view, not just our own counters.

Closed when: we have our own three numbers and a decision recorded about where optimisation goes.

## Deliberately not taken

- **Their engine version.** They run 2.36.8; we run 2.52.4. Going backwards to inherit their fixes
  would trade a newer engine for someone else's patch set.
- **Video and MSE.** Their H.264-only playback, the VP9/AV1 black player, and YouTube stopping after a
  minute are a large body of work for a capability we have not started. Their `decoded=38.3fps
  drawn=0.0fps` symptom is worth remembering as a *shape* — decode succeeding while nothing reaches the
  screen, which is what our current blank-frame defect looks like — but not as a feature to port.
- **`USE_SYSTEM_MALLOC`.** We deliberately use bmalloc; adopting their allocator to inherit their
  memory-pool tooling would be a regression.
- **`window.open` and file upload.** Real gaps, but they belong to the shell roadmap rather than to
  anything learned from Revenant.


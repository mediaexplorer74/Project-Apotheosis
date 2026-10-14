// Apotheosis 2026-09-27: a scriptable command channel over LocalState.
//
// nav.txt did this job for the C++/CX harness -- write a URL into LocalState and the browser
// navigates -- but the watcher was never carried into the C# line, where the only hook left is
// testurl.txt, read ONCE at startup. That makes every check cost a full app restart, which is
// unusable for iterating on a scroll or paint problem. The harness alone had synth-scroll.ps1,
// tap-test.ps1, stall-watch.ps1 and a dozen more; none of them can reach this app.
//
// Protocol: write one command per line into LocalState\autocmd.txt. The watcher notices the file
// changed, runs the batch on the engine thread and appends a timestamped result per command to
// LocalState\autocmd-log.txt. A leftover file is never replayed -- the stamp is seeded at startup
// and only a change re-arms it.
//
//   goto <url>               navigate
//   scroll <dx> <dy>         one scroll step, timed (engine path, NOT the touch path)
//   swipe <dy> <steps> <gapMs>  a finger: drives FreeScrollBy on the UI thread, ~60Hz
//   ui <what>              drive the shell: settings | settings-close | menu | menu-close
//                          | general | advanced | tabs
//   lang <0|1|2>           pick a language in the settings combo (en | ru | zh). Goes through
//                          the ComboBox itself, so it is the same path a finger takes: the
//                          SelectionChanged handler, not a direct call to Str.SetLang.
//   scrolln <dx> <dy> <n>    n steps, timed individually -- the scroll benchmark
//   ua <mobile|desktop>      the user agent, through the Settings ToggleSwitch
//   zoom <50..200>           the page zoom, through the Settings slider. The only scale lever
//                            there is (page scale was removed 2026-10-03), so this is how a run
//                            tests any geometry other than the window's own width.
//   dtap                     double-tap-to-fit: toggle between the fit zoom (100 %) and the last
//                            zoom the user chose that was not 100 %. Calls the same DoubleTapFit()
//                            the DoubleTapped handler calls, minus its 250 ms tap-hold -- the gesture
//                            stack cannot be driven from a script on this bench.
//   suggest <text>           put <text> in the address box with the box treated as focused and
//                            let the real chain build the dropdown (`_urlFocused` is set for the
//                            script because a control cannot be focused from here), then log the
//                            row count, the panel's visibility AND its visibility again 700 ms
//                            later -- a shown-then-hidden panel reads the same as one that never
//                            drew if you only look once.
//   enter                    press Enter on the address box -- same CommitUrlBox() as the key
//                            handler -- and log the panel's visibility after it. `suggest` then
//                            `enter` is the reported sequence: type, see the list, commit.
//   wait <ms>                sleep on the engine thread
//   hash                     log the frame hash (equal consecutive hashes = a settled frame)
//   diag                     log the driver's page diagnostics
//
// Every timed command reports elapsed ms, so scroll cost is measured rather than guessed.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Windows.Storage;
using Windows.UI.Core;
using Windows.UI.Xaml;

namespace Apotheosis
{
    public sealed partial class MainPage
    {
        DispatcherTimer _autoCmdTimer;
        string _autoCmdFile;
        string _autoCmdLog;
        DateTime _autoCmdStamp;
        bool _autoCmdRunning;
        int _autoCmdBatch;

        void StartAutoCmd()
        {
            try
            {
                if (_autoCmdTimer != null) return;
                string d = ApplicationData.Current.LocalFolder.Path;
                _autoCmdFile = Path.Combine(d, "autocmd.txt");
                _autoCmdLog = Path.Combine(d, "autocmd-log.txt");
                if (File.Exists(_autoCmdFile))
                    _autoCmdStamp = File.GetLastWriteTimeUtc(_autoCmdFile);

                _autoCmdTimer = new DispatcherTimer() { Interval = TimeSpan.FromMilliseconds(250) };
                _autoCmdTimer.Tick += (s, e) => PollAutoCmd();
                _autoCmdTimer.Start();
            }
            catch { }
        }

        void PollAutoCmd()
        {
            if (_autoCmdRunning) return;
            try
            {
                if (!File.Exists(_autoCmdFile)) return;
                DateTime stamp = File.GetLastWriteTimeUtc(_autoCmdFile);
                if (stamp == _autoCmdStamp) return;
                _autoCmdStamp = stamp;

                var batch = new List<string>();
                foreach (var raw in File.ReadAllLines(_autoCmdFile))
                {
                    string t = (raw ?? "").Trim();
                    if (t.Length > 0 && !t.StartsWith("#")) batch.Add(t);
                }
                if (batch.Count == 0) return;
                _autoCmdBatch++;
                RunAutoBatch(batch);
            }
            catch { }
        }

        // Apotheosis 2026-10-03: the append is serialized, and that is a correctness fix, not
        // tidiness.
        //
        // AutoLog is written from two threads -- the engine thread (the batch loop and the scroll
        // and swipe helpers) and the UI thread (every AutoUi lambda) -- and File.AppendAllText
        // opens the file with FileShare.Read, i.e. it denies a concurrent writer. The losing call
        // throws a sharing violation, and the catch below used to swallow it silently.
        //
        // Measured, and in both directions: a batch of `taplink 14;wait 16000;diag` logged
        // `ui taplink 14 done` while losing the `taplink[14] rect=... url=...` line that said what
        // the tap was aiming at; and a batch of `taplink 999;wait 400` logged the UI thread's
        // `bad index` line while losing the engine thread's `wait 400ms (engine released)` line.
        // A record that has lost the line naming its own target still reads complete, which makes
        // this the same class of defect as the ones in Doc/LESSONS.md: the instrument, not the
        // subject, was wrong. Nothing else in this app writes autocmd-log.txt, so a lock is enough.
        static readonly object _autoLogGate = new object();

        void AutoLog(string text)
        {
            string line = DateTime.Now.ToString("HH:mm:ss.fff") + "  " + text + Environment.NewLine;
            lock (_autoLogGate)
            {
                try
                {
                    File.AppendAllText(_autoCmdLog, line);
                }
                catch
                {
                    // Still possible from outside the process (a reader that does not share), so
                    // the line is retried rather than dropped -- but never indefinitely: this runs
                    // on the UI thread too.
                    try
                    {
                        System.Threading.Tasks.Task.Delay(25).Wait();
                        File.AppendAllText(_autoCmdLog, line);
                    }
                    catch { }
                }
            }
        }

        void RunAutoBatch(List<string> batch)
        {
            _autoCmdRunning = true;
            AutoLog("---- batch " + _autoCmdBatch + " (" + batch.Count + " cmds) ----");
            PostBatchChunk(batch, 0);
        }

        // One hop onto the engine thread. A batch is normally exactly one hop; it becomes two or
        // more only when a `wait` releases the thread and re-posts the remainder. _autoCmdRunning
        // stays set across the hops so a second file write cannot interleave two batches.
        void PostBatchChunk(List<string> batch, int start)
        {
            WebEngine.Instance.Post(() =>
            {
                bool reposted = false;
                try { reposted = RunAutoBatchOnEngine(batch, start); }
                catch (Exception ex) { AutoLog("FATAL " + ex.GetType().Name + ": " + ex.Message); }
                finally { if (!reposted) _autoCmdRunning = false; }
            });
        }

        // Runs on the engine thread: every P/Invoke below needs it.
        //
        // Apotheosis 2026-10-03: `start` is where in the batch to begin. The batch is re-posted
        // from `start` after a `wait`, because a wait that BLOCKS this thread is a wait that
        // prevents the very thing it is waiting for: `goto` is fire-and-forget by design (see
        // there) and its navigation job is queued behind this thread's own queue. `goto X;
        // wait 6000; diag` therefore could not observe its own navigation -- the load ran after
        // the batch ended, so `diag` reported the PREVIOUS page and stage.txt kept the previous
        // line. Measured on 2026-10-03: a 6 s wait read the old page, an 8 s load appeared only
        // 4 minutes and one batch later. Return true to say "re-posted, do not release the run
        // flag"; false means the batch is finished.
        bool RunAutoBatchOnEngine(List<string> batch, int start)
        {
            byte[] frame = NewFrameBuffer();
            for (int i = start; i < batch.Count; i++)
            {
                string cmd = batch[i];
                string[] parts = cmd.Split(' ');
                string verb = parts[0].ToLowerInvariant();
                var sw = Stopwatch.StartNew();
                try
                {
                    if (verb == "goto")
                    {
                        string url = cmd.Substring(4).Trim();
                        AutoLog("goto " + url);
                        // Deliberately NOT gated on _sessionActive. The gate used to be here and
                        // it deadlocked the channel: a visit to Home calls WebCoreCloseSession and
                        // sets _sessionActive=false, and after that the only thing that can bring a
                        // session back is a navigation -- which this check refused to perform. A
                        // twelve-site smoke run produced fifteen batches of "goto ignored: no
                        // session" and not one request. NavigateTo is what creates or replaces the
                        // session, so it is exactly the thing that must stay reachable.
                        //
                        // NavigateTo touches XAML, so it has to run on the UI thread. Calling it
                        // straight from the controller's engine thread threw RPC_E_WRONG_THREAD
                        // (0x8001010E) the moment a page was actually on screen. Fire-and-forget:
                        // the wait verb after it is what observes the load -- and `wait` only
                        // observes anything because it releases this thread (see the comment on
                        // this method).
                        AutoUi("goto " + url);
                    }
                    else if (verb == "scroll")
                    {
                        AutoScrollOne(int.Parse(parts[1]), int.Parse(parts[2]), frame, sw, -1);
                    }
                    else if (verb == "scrolln")
                    {
                        int dx = int.Parse(parts[1]), dy = int.Parse(parts[2]), n = int.Parse(parts[3]);
                        AutoLog("scrolln dx=" + dx + " dy=" + dy + " n=" + n);
                        // `k`, not `i`: the batch loop above owns `i` now that a batch can be
                        // re-posted from an index, and the compiler is right to refuse the shadow.
                        for (int k = 0; k < n; k++)
                        {
                            sw.Restart();
                            AutoScrollOne(dx, dy, frame, sw, k);
                        }
                    }
                    else if (verb == "swipe")
                    {
                        AutoSwipe(int.Parse(parts[1]), int.Parse(parts[2]), int.Parse(parts[3]));
                    }
                    else if (verb == "tap")
                    {
                        // A tap is the one input the app cannot be driven with from outside: XAML's
                        // manipulation stack ignores synthetic OS input on this bench (memory:
                        // `synthetic-input-cannot-reach-uwp`), so a scripted tap has to enter the
                        // app's own handler rather than pretend to be a mouse. It goes through
                        // AutoUi because HandleTapAt touches XAML (hides the suggestion list, and a
                        // fallback path navigates) -- the same reason `goto` does.
                        //
                        // Coordinates are DIP inside ContentArea, which is the space
                        // TappedRoutedEventArgs.GetPosition(ContentArea) reports. `taplink` exists
                        // for the case where guessing them would measure the layout instead of the
                        // tap path.
                        AutoUi("tap " + (parts.Length > 1 ? parts[1] : "") + " " + (parts.Length > 2 ? parts[2] : ""));
                    }
                    else if (verb == "taplink")
                    {
                        // Tap the centre of _pageLinks[n] -- the same table the shell consults as its
                        // fallback when the engine reports no URL change. Self-locating, so the test
                        // does not depend on where the page happens to have laid its anchors.
                        AutoUi("taplink " + (parts.Length > 1 ? parts[1] : ""));
                    }
                    else if (verb == "links")
                    {
                        // The link table in the log. Choosing a tap target by reading the page's own
                        // hit list is a measurement; choosing one by hardcoding a coordinate is a
                        // measurement of the page's current layout.
                        AutoUi("links");
                    }
                    else if (verb == "ui")
                    {
                        AutoUi(parts.Length > 1 ? parts[1] : "");
                    }
                    else if (verb == "lang")
                    {
                        // The settings ComboBox is the real control, so the repaint has to go
                        // through it: setting the index fires SelectionChanged, which is what a
                        // tap on a dropdown item does. That makes this a measurement of the
                        // user's path and not of a private method.
                        AutoUi("lang " + (parts.Length > 1 ? parts[1] : ""));
                    }
                    else if (verb == "ua")
                    {
                        // Desktop vs mobile is a mode the acceptance run has to be able to select,
                        // or "this site renders wrong in desktop mode" cannot be tested at all --
                        // it is a thing the tester could only do by hand-editing settings.ini.
                        // Drives the real ToggleSwitch; see the AutoUi case.
                        AutoUi("ua " + (parts.Length > 1 ? parts[1].Trim().ToLowerInvariant() : ""));
                    }
                    else if (verb == "zoom")
                    {
                        // Page zoom is the only scale lever there is (see CommitPageZoom in MainPage),
                        // so an acceptance run that cannot set it can only test one geometry. Goes
                        // through the real slider; see the AutoUi case.
                        AutoUi("zoom " + (parts.Length > 1 ? parts[1].Trim() : ""));
                    }
                    else if (verb == "dtap")
                    {
                        // Double-tap-to-fit is a gesture, and this bench cannot drive the gesture
                        // stack (memory: `synthetic-input-cannot-reach-uwp`), so without a verb the
                        // feature could only be tested by a finger on a device. The verb calls the
                        // same DoubleTapFit() the DoubleTapped handler calls -- it does NOT take the
                        // 250 ms tap-hold path, which exists only to tell a single tap from the first
                        // half of a pair and has nothing to disambiguate when a script asks.
                        AutoUi("dtap");
                    }
                    else if (verb == "suggest")
                    {
                        // Put text in the real UrlBox and open the real dropdown. The gesture stack
                        // cannot be driven here and the panel hides itself whenever the box is not
                        // focused, so without this the only way to see the suggestion list -- or to
                        // count its rows -- was a finger. It measures the shipping builder: same
                        // ShowSuggestions(), same MakeRow().
                        AutoUi("suggest " + (parts.Length > 1 ? parts[1].Trim() : ""));
                    }
                    else if (verb == "enter")
                    {
                        // Press Enter on the address box: the same CommitUrlBox() the key handler
                        // calls. `suggest` + `enter` reproduces the reported sequence -- type, see
                        // the list, commit -- which is the only way to see whether the list is still
                        // on screen after the commit.
                        AutoUi("enter");
                    }
                    else if (verb == "wait")
                    {
                        int ms = int.Parse(parts[1]);
                        // Thread.Sleep is not available in this UWP profile (only
                        // System.Threading.Tasks is referenced), so the delay goes through
                        // Task.Delay -- and it is deliberately NOT .Wait()ed on. Blocking this
                        // thread is what made `wait` useless (see the comment above this method):
                        // the delay runs on the pool, the engine thread is released, whatever was
                        // queued meanwhile runs, and the rest of the batch is re-posted after it.
                        int rest = i + 1;
                        AutoLog("wait " + ms + "ms (engine released)");
                        System.Threading.Tasks.Task.Delay(ms).ContinueWith(_ =>
                        {
                            AutoLog("wait " + ms + "ms elapsed");
                            if (rest < batch.Count) PostBatchChunk(batch, rest);
                            else _autoCmdRunning = false;
                        });
                        return true;
                    }
                    else if (verb == "hash")
                    {
                        AutoLog("hash 0x" + WebCoreDriver.WebCoreGetFrameHash().ToString("x8"));
                    }
                    else if (verb == "back" || verb == "forward")
                    {
                        // Routed through AutoUi, not run here: NavigateHistory drives the shell's
                        // toolbar state and the engine, so it belongs on the dispatcher like `goto`.
                        // The FIRST version of this verb was added only to AutoUi's inner switch --
                        // the one reached by the `ui <what>` verb -- and a run logged a bare
                        // "unknown verb: back" while looking exactly like a Back that the engine had
                        // refused. A verb that is not in this chain does not exist.
                        AutoLog(verb);
                        AutoUi(verb);
                    }
                    else if (verb == "hist")
                    {
                        // Reads only -- no navigation, no repaint. It exists so an acceptance run can
                        // ask "where does each history stand" BEFORE and AFTER a `back`, which is the
                        // only way to tell "Back was refused" from "Back moved and the page looked
                        // the same". Runs on the engine thread, so the P/Invokes are legal here; the
                        // host stack is plain data and needs no dispatcher.
                        RefreshEngineHistory();
                        AutoLog("hist engine back=" + WebCoreDriver.WebCoreCanGoBack()
                                + " fwd=" + WebCoreDriver.WebCoreCanGoForward()
                                + " cached=" + _engineBack + "/" + _engineFwd
                                + " host idx=" + _navIndex + "/" + (_navStack.Count - 1)
                                + " url=" + _currentUrl);
                    }
                    else if (verb == "diag")
                    {
                        // 4096, not 2048: the diag carries the resource table and the last error, and a
                        // page with a long `res:[...]` list overflows the old size -- measured on
                        // dzen.ru 2026-10-03, 2048 was not enough for one page.
                        byte[] dbuf = new byte[4096];
                        int n = WebCoreDriver.WebCoreGetDiag(dbuf, dbuf.Length);
                        // The port returns snprintf's return value -- the length it WOULD have written,
                        // which can be larger than the buffer it was given -- so `n` is NOT a safe count
                        // for GetString. Used as one it threw ArgumentOutOfRangeException and the whole
                        // `diag` line was lost with it, which is worse than a clipped one: the run showed
                        // an exception where the page's own answer should have been. Read the buffer
                        // whole and trim at the first NUL, the way every other Get* caller in this file
                        // does it. Truncation is SAID, never silent, because a clipped `res:` list reads
                        // as "the page requested nothing".
                        string dtxt = System.Text.Encoding.UTF8.GetString(dbuf).TrimEnd('\0');
                        if (n >= dbuf.Length)
                            dtxt += " [TRUNCATED: " + n + " bytes of diag, " + (dbuf.Length - 1) + " kept]";
                        // Apotheosis 2026-10-03: the shell's own busy state is part of the
                        // diagnosis, not decoration. A `goto` that the user asked for and the app
                        // DROPPED leaves the engine diag looking exactly like a page that simply
                        // did not change, and the drop wrote no line of its own -- that is how a
                        // wedged `_loading` flag cost an afternoon of "the navigation did nothing".
                        // These three bits separate "the engine refused" from "the shell never
                        // asked". Read from the engine thread without a lock on purpose: they are
                        // the shell's own bool/long, and a read one tick stale is still the answer.
                        //
                        // Apotheosis 2026-10-03, later the same day: the first version of this line
                        // read `RenderImage.RenderTransform` from HERE, which is the engine thread,
                        // and every `diag` in the batch died with RPC_E_WRONG_THREAD (0x8001010E) --
                        // a XAML DependencyObject belongs to the UI thread and touching one from
                        // anywhere else throws. The lesson is not "read the transform somewhere
                        // else": it is that this verb may only touch plain fields. `xform=` is
                        // therefore GONE rather than relocated, because there is nothing left to
                        // report -- the live ScaleTransform is cleared by the commit that owns it,
                        // in the same dispatcher call that blits (PostZoomAndRepaint), so it cannot
                        // outlive its gesture the way the old page-scale path let it.
                        //
                        // What replaced the page SCALE this line used to print: with page scale
                        // removed there is one zoom, and it is a zoom the engine re-lays out for.
                        // pzoom = the factor the engine was last told; vz = the factor the shell
                        // WANTS (ViewportZoom, i.e. the setting); zoom = that setting as a percent,
                        // which is what the Settings view shows and what a pinch writes. pzoom != vz
                        // is the honest "a repaint is owed" state, and it is the one a stale zoom
                        // would hide in.
                        AutoLog("diag nav=L" + (_loading ? 1 : 0) + "/I" + (_interacting ? 1 : 0)
                                + "/S" + (_sessionActive ? 1 : 0) + " seq=" + _opSeq
                                + " pzoom=" + (int)(_pageZoom * 1000 + 0.5f)
                                + " vz=" + (int)(_vpZoom * 1000 + 0.5f)
                                + " zoom=" + _defaultZoom
                                + " fit=" + (_fitWindow ? 1 : 0)
                                + " pinch=" + (_pinching ? 1 : 0)
                                + " url=" + _currentUrl
                                + " | " + dtxt);
                    }
                    else
                    {
                        AutoLog("unknown verb: " + cmd);
                    }
                }
                catch (Exception ex)
                {
                    AutoLog("ERR '" + cmd + "' " + ex.GetType().Name + ": " + ex.Message);
                }
            }
            return false;
        }

        // Drive the shell's own UI, so a run can prove the chrome works and not only the
        // engine. Needed on the device too, where there is nobody to tap the menu button.
        // Everything here goes through the dispatcher because these touch XAML state.
        void AutoUi(string what)
        {
            CoreDispatcher disp = Dispatcher;
            try
            {
                disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    try
                    {
                        switch ((what ?? "").ToLowerInvariant())
                        {
                            case "settings": ShowSettings(); break;
                            case "settings-close": HideSettings(); break;
                            case "menu": ShowActionMenu(); break;
                            case "menu-close": HideActionMenu(); break;
                            case "general": ShowSettings(); if (SettingsPivot != null) SettingsPivot.SelectedIndex = 0; break;
                            case "advanced": ShowSettings(); if (SettingsPivot != null) SettingsPivot.SelectedIndex = 1; break;
                            case "tabs": TabSwitcher.Visibility = Visibility.Visible; break;
                            case "home": NavigateTo(_homeUrl, true); break;
                            case "back": NavigateHistory(-1); break;
                            case "forward": NavigateHistory(+1); break;
                            default:
                                if (what != null && what.StartsWith("goto ", StringComparison.OrdinalIgnoreCase))
                                {
                                    string u = what.Substring(5).Trim();
                                    if (u.Length > 0) NavigateTo(u, true);
                                    else AutoLog("ui goto: empty url");
                                }
                                else if (what != null && what.StartsWith("lang ", StringComparison.OrdinalIgnoreCase))
                                {
                                    int lang;
                                    if (SetLangCombo != null && int.TryParse(what.Substring(5).Trim(), out lang)
                                        && lang >= 0 && lang < SetLangCombo.Items.Count)
                                    {
                                        SetLangCombo.SelectedIndex = lang;
                                        AutoLog("lang -> " + lang + " (setlang=" + Str.Lang + ")");
                                    }
                                    else AutoLog("ui lang: bad index '" + what.Substring(5).Trim() + "'");
                                }
                                else if (what != null && what.StartsWith("ua ", StringComparison.OrdinalIgnoreCase))
                                {
                                    string w = what.Substring(3).Trim().ToLowerInvariant();
                                    bool desktop = (w == "desktop");
                                    if (w != "desktop" && w != "mobile")
                                    {
                                        AutoLog("ui ua: bad mode '" + w + "' (want mobile|desktop)");
                                        return;
                                    }
                                    if (SetUaSwitch != null && SetUaSwitch.IsOn != desktop)
                                        SetUaSwitch.IsOn = desktop;   // fires OnUaSwitchToggled
                                    // Belt, and it is load-bearing: a ToggleSwitch raises Toggled only
                                    // when the value actually changes, and _setUaDesktop can already
                                    // be true (settings.ini ua=1) while the control still reads its
                                    // XAML default of false. Without this line that case writes
                                    // "ua -> mobile" and leaves the engine on the desktop user agent.
                                    _setUaDesktop = desktop;
                                    ApplySettings();
                                    AutoLog("ua -> " + w + " (desktop=" + (_setUaDesktop ? 1 : 0) + ")");
                                }
                                else if (what != null && what.StartsWith("zoom ", StringComparison.OrdinalIgnoreCase))
                                {
                                    // The page zoom, driven through the user's own path: open Settings,
                                    // move the slider, close it. Moving the slider only repaints the label
                                    // (OnZoomChanged reads e.NewValue for that and nothing else) -- the
                                    // value that matters is read by HideSettings, which then calls
                                    // ApplySettings -> (fitwindow) AdoptViewport -> ApplyViewport ->
                                    // PostZoomAndRepaint. Assigning _defaultZoom here instead would
                                    // measure a private field and skip the repaint entirely, which is
                                    // exactly the failure mode the acceptance run exists to catch.
                                    int pct;
                                    if (!int.TryParse(what.Substring(5).Trim(), out pct))
                                    {
                                        AutoLog("ui zoom: bad value '" + what.Substring(5).Trim() + "'");
                                        return;
                                    }
                                    if (SetZoomSlider == null)
                                    {
                                        AutoLog("ui zoom: no slider on this page");
                                        return;
                                    }
                                    ShowSettings();
                                    double want = Math.Max(SetZoomSlider.Minimum, Math.Min(SetZoomSlider.Maximum, (double)pct));
                                    if (Math.Abs(SetZoomSlider.Value - want) > 0.01)
                                        SetZoomSlider.Value = want;      // fires OnZoomChanged -> the label
                                    HideSettings();                      // reads the slider, applies, saves
                                    AutoLog("ui zoom -> " + pct + "% (slider=" + (int)(SetZoomSlider.Value + 0.5)
                                            + " default=" + _defaultZoom + ")");
                                }
                                else if (what != null && what.Equals("dtap", StringComparison.OrdinalIgnoreCase))
                                {
                                    // Runs on the dispatcher, where DoubleTapFit's own CommitPageZoom
                                    // is legal (the verb body itself runs on the engine thread and
                                    // may not touch XAML). See the `dtap` verb for why this is not
                                    // the 250 ms handler.
                                    AutoLog("ui dtap: vz=" + _vpZoom.ToString("0.00")
                                            + " default=" + _defaultZoom);
                                    DoubleTapFit();
                                }
                                else if (what != null && what.StartsWith("suggest ", StringComparison.OrdinalIgnoreCase))
                                {
                                    // The real path into the dropdown is a FOCUSED box: typing raises
                                    // TextChanged -> OnUrlChanged -> ShowSuggestions. This verb first
                                    // faked only the text (`_urlSyncing` on, `_urlFocused` untouched),
                                    // which put the shell in a state no finger can reach -- the panel
                                    // was shown and then OnUrlChanged, seeing `_urlFocused == false`,
                                    // hid it again. Measured 2026-10-03: `ui suggest 'dzen' rows=5
                                    // vis=1` in the log, an empty page in the screenshot at 16:33 and
                                    // again at 16:40. A script cannot focus a control
                                    // (`synthetic-input-cannot-reach-uwp`), so `_urlFocused` is set
                                    // here and every line after it is the shipping chain.
                                    string q = what.Substring(8).Trim();
                                    if (UrlBox != null)
                                    {
                                        _urlFocused = true;
                                        UrlBox.Text = q;   // fires OnUrlChanged, which builds the panel
                                    }
                                    // Belt: OnUrlChanged does nothing when the text is unchanged, and
                                    // the verb must still report a row count.
                                    ShowSuggestions(q);
                                    AutoLog("ui suggest '" + q + "' rows="
                                            + (SuggestList != null ? SuggestList.Children.Count : -1)
                                            + " vis=" + (SuggestPanel != null &&
                                                          SuggestPanel.Visibility == Visibility.Visible ? 1 : 0));
                                    // A panel that is shown and then hidden by a later handler looks
                                    // exactly like one that was never drawn. Read it again after the
                                    // posted work has drained: 700 ms is past any layout pass.
                                    var probe = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
                                    probe.Tick += (s2, a2) =>
                                    {
                                        probe.Stop();
                                        AutoLog("ui suggest+700 vis="
                                                + (SuggestPanel != null &&
                                                   SuggestPanel.Visibility == Visibility.Visible ? 1 : 0)
                                                + " h=" + (int)(SuggestPanel != null ? SuggestPanel.ActualHeight : -1)
                                                + " boxfoc=" + (_urlFocused ? 1 : 0));
                                    };
                                    probe.Start();
                                }
                                else if (what != null && what.Equals("enter", StringComparison.OrdinalIgnoreCase))
                                {
                                    // Same path as the Enter key. Log the panel state AFTER the call,
                                    // because "did the commit close the list?" is the whole question.
                                    CommitUrlBox();
                                    AutoLog("ui enter '" + (UrlBox != null ? UrlBox.Text : "?") + "' vis="
                                            + (SuggestPanel != null &&
                                               SuggestPanel.Visibility == Visibility.Visible ? 1 : 0));
                                }
                                else if (what != null && what.StartsWith("tap ", StringComparison.OrdinalIgnoreCase))
                                {
                                    string[] p = what.Substring(4).Trim().Split(' ');
                                    int tx, ty;
                                    if (p.Length >= 2 && int.TryParse(p[0], out tx) && int.TryParse(p[1], out ty))
                                    {
                                        AutoLog("tap dip=" + tx + "," + ty + " area=" + (int)ContentArea.ActualWidth
                                                + "x" + (int)ContentArea.ActualHeight + " session=" + (_sessionActive ? 1 : 0));
                                        HandleTapAt(tx, ty);
                                    }
                                    else AutoLog("ui tap: bad point '" + what.Substring(4).Trim() + "'");
                                }
                                else if (what != null && what.StartsWith("taplink ", StringComparison.OrdinalIgnoreCase))
                                {
                                    string arg = what.Substring(8).Trim();
                                    int idx;
                                    if (!int.TryParse(arg, out idx) || idx < 0 || idx >= _pageLinks.Count)
                                    {
                                        AutoLog("ui taplink: bad index '" + arg + "' (table has "
                                                + _pageLinks.Count + "; run `links`)");
                                        return;
                                    }
                                    var l = _pageLinks[idx];
                                    MapEngineToDip(l.X + l.W / 2, l.Y + l.H / 2, out double tdx, out double tdy);
                                    // The url is logged BEFORE the tap, so the navigation that follows
                                    // can be checked against what was aimed at rather than against
                                    // whatever the page decided to do -- the same distinction the
                                    // harness's `simtap:` line exists to preserve.
                                    AutoLog("taplink[" + idx + "] rect=" + l.X + "," + l.Y + "," + l.W + "," + l.H
                                            + " dip=" + (int)tdx + "," + (int)tdy + " url=" + l.Url);
                                    HandleTapAt(tdx, tdy);
                                }
                                else if (what != null && what.Equals("links", StringComparison.OrdinalIgnoreCase))
                                {
                                    AutoLog("links n=" + _pageLinks.Count);
                                    for (int i = 0; i < _pageLinks.Count && i < 40; i++)
                                    {
                                        var l = _pageLinks[i];
                                        AutoLog("  [" + i + "] " + l.X + "," + l.Y + " " + l.W + "x" + l.H + " " + l.Url);
                                    }
                                }
                                else { AutoLog("ui unknown: " + what); return; }
                                break;
                        }
                        AutoLog("ui " + what + " done");
                    }
                    catch (Exception ex) { AutoLog("ui ERR " + ex.GetType().Name + ": " + ex.Message); }
                });
            }
            catch (Exception ex) { AutoLog("ui post failed " + ex.Message); }
        }

        // Emulate a finger, not a scrollbar.
        //
        // The `scroll` command calls WebCoreScrollBy directly and therefore measures only the engine
        // plus the present. A real drag is a different path: OnImageManipDelta -> FreeScrollBy
        // accumulates a delta on the UI thread, PumpScroll coalesces whatever accumulated into ONE
        // engine call, and when the queue finally drains it runs SyncLinksAfterScroll and restarts the
        // live tick. If a step arrives while _scrollBusy is set it is simply merged into the next
        // pump. That coalescing is where "smooth at first, then it seizes up" would live, so the
        // benchmark has to drive FreeScrollBy and let the real merge logic run.
        //
        // It must therefore run on the UI thread, not the engine thread: from the engine thread
        // FreeScrollBy would Post its own scroll job behind the batch currently executing, and the
        // whole gesture would only replay after the batch finished.
        //
        // The pause between steps is awaited, not blocked on -- Task.Delay().Wait() would freeze the
        // dispatcher, the pending present callbacks could not run, and the measurement would describe
        // a stalled UI instead of a swipe. gapMs=16 approximates a 60Hz finger.
        void AutoSwipe(int totalDy, int steps, int gapMs)
        {
            if (steps < 1) steps = 1;
            int per = (int)Math.Round(totalDy / (double)steps);
            AutoLog("swipe start dy=" + totalDy + " steps=" + steps
                + " per=" + per + " gapMs=" + gapMs);

            CoreDispatcher disp = Dispatcher;
            try
            {
                disp.RunAsync(CoreDispatcherPriority.Normal, async () =>
                {
                    try
                    {
                        if (!_sessionActive) { AutoLog("swipe ignored: no session"); return; }
                        var sw = Stopwatch.StartNew();
                        long last = 0;
                        for (int i = 0; i < steps; i++)
                        {
                            long t = sw.ElapsedMilliseconds;
                            FreeScrollBy(0, per);
                            AutoLog("swipe[" + i + "] stepMs=" + (t - last) + " at=" + t);
                            last = t;
                            if (gapMs > 0) await System.Threading.Tasks.Task.Delay(gapMs);
                        }
                        AutoLog("swipe end totalMs=" + sw.ElapsedMilliseconds);
                        AutoLog("hash 0x" + WebCoreDriver.WebCoreGetFrameHash().ToString("x8"));
                    }
                    catch (Exception ex) { AutoLog("swipe ERR " + ex.GetType().Name + ": " + ex.Message); }
                });
            }
            catch (Exception ex) { AutoLog("swipe post failed " + ex.Message); }
        }

        // One scroll step, timed. The number that matters is the per-step cost as the page grows, so
        // every step logs its own elapsed time rather than an average -- that is precisely the shape
        // of the bench complaint: fine for a while, then grinding to a halt.
        void AutoScrollOne(int dx, int dy, byte[] frame, Stopwatch sw, int index)
        {
            int rc = -999;
            GCHandle h = GCHandle.Alloc(frame, GCHandleType.Pinned);
            try { rc = WebCoreDriver.WebCoreScrollBy(dx, dy, h.AddrOfPinnedObject()); }
            finally { h.Free(); }
            AutoLog((index >= 0 ? "scroll[" + index + "]" : "scroll")
                + " rc=" + rc + " ms=" + sw.ElapsedMilliseconds);
        }
    }
}

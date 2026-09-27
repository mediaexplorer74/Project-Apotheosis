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
//   scrolln <dx> <dy> <n>    n steps, timed individually -- the scroll benchmark
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

        void AutoLog(string text)
        {
            try
            {
                File.AppendAllText(_autoCmdLog,
                    DateTime.Now.ToString("HH:mm:ss.fff") + "  " + text + Environment.NewLine);
            }
            catch { }
        }

        void RunAutoBatch(List<string> batch)
        {
            _autoCmdRunning = true;
            AutoLog("---- batch " + _autoCmdBatch + " (" + batch.Count + " cmds) ----");
            WebEngine.Instance.Post(() =>
            {
                try { RunAutoBatchOnEngine(batch); }
                catch (Exception ex) { AutoLog("FATAL " + ex.GetType().Name + ": " + ex.Message); }
                finally { _autoCmdRunning = false; }
            });
        }

        // Runs on the engine thread: every P/Invoke below needs it.
        void RunAutoBatchOnEngine(List<string> batch)
        {
            byte[] frame = NewFrameBuffer();
            foreach (string cmd in batch)
            {
                string[] parts = cmd.Split(' ');
                string verb = parts[0].ToLowerInvariant();
                var sw = Stopwatch.StartNew();
                try
                {
                    if (verb == "goto")
                    {
                        string url = cmd.Substring(4).Trim();
                        AutoLog("goto " + url);
                        if (!_sessionActive) AutoLog("goto ignored: no session");
                        else NavigateTo(url, true);
                    }
                    else if (verb == "scroll")
                    {
                        AutoScrollOne(int.Parse(parts[1]), int.Parse(parts[2]), frame, sw, -1);
                    }
                    else if (verb == "scrolln")
                    {
                        int dx = int.Parse(parts[1]), dy = int.Parse(parts[2]), n = int.Parse(parts[3]);
                        AutoLog("scrolln dx=" + dx + " dy=" + dy + " n=" + n);
                        for (int i = 0; i < n; i++)
                        {
                            sw.Restart();
                            AutoScrollOne(dx, dy, frame, sw, i);
                        }
                    }
                    else if (verb == "swipe")
                    {
                        AutoSwipe(int.Parse(parts[1]), int.Parse(parts[2]), int.Parse(parts[3]));
                    }
                    else if (verb == "wait")
                    {
                        int ms = int.Parse(parts[1]);
                        // Thread.Sleep is not available in this UWP profile (only
                        // System.Threading.Tasks is referenced), so the sleep goes through Task.Delay.
                        System.Threading.Tasks.Task.Delay(ms).Wait();
                        AutoLog("wait " + ms + "ms -> " + sw.ElapsedMilliseconds + "ms");
                    }
                    else if (verb == "hash")
                    {
                        AutoLog("hash 0x" + WebCoreDriver.WebCoreGetFrameHash().ToString("x8"));
                    }
                    else if (verb == "diag")
                    {
                        byte[] dbuf = new byte[2048];
                        int n = WebCoreDriver.WebCoreGetDiag(dbuf, dbuf.Length);
                        AutoLog("diag " + System.Text.Encoding.UTF8.GetString(dbuf, 0, Math.Max(0, n)));
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

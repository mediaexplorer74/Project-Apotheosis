using System.Collections.Generic;

namespace Apotheosis
{
    public class Entry
    {
        public string Url { get; set; } = "";
        public string Title { get; set; } = "";
        public string Extra { get; set; } = "";
    }

    public class PageLink
    {
        public int X { get; set; }
        public int Y { get; set; }
        public int W { get; set; }
        public int H { get; set; }
        public string Url { get; set; } = "";
    }

    public enum DrawerTab { Favorites, History, Downloads }

    public class Tab
    {
        // Apotheosis 2026-10-03: this held a per-tab PAGE SCALE. There is no page scale any more --
        // the zoom is CSS page zoom, one user-level setting (see ViewportZoom in MainPage), so tabs
        // share it and a tab switch must not carry a stale copy of it. Page scale magnified the
        // finished layer tree without re-laying out, which is what let a tab's geometry and its
        // pixels disagree after a pinch.
        public List<string> NavStack { get; set; } = new List<string>();
        public int NavIndex { get; set; } = -1;
        public string CurrentUrl { get; set; } = "about:home";
        public string CurrentTitle { get; set; } = "";
    }
}

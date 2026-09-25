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
        public List<string> NavStack { get; set; } = new List<string>();
        public int NavIndex { get; set; } = -1;
        public string CurrentUrl { get; set; } = "about:home";
        public string CurrentTitle { get; set; } = "";
        public float PageScale { get; set; } = 1.0f;
    }
}

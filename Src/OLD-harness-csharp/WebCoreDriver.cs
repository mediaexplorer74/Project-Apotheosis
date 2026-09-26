using System;
using System.Runtime.InteropServices;

namespace Apotheosis
{
    internal static class WebCoreDriver
    {
        private const string Dll = "WebCoreDriver-gpu.dll";

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreSetCACertBlob(byte[] data, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreRenderHtml(string utf8Html, int width, int height, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreSessionLoad(string url, int width, int height, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreCloseSession();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreClickAt(int x, int y, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreScrollBy(int dx, int dy, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSyncLinks();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSessionPaint(IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSetPageScale(float scale, int focalX, int focalY, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetPageScale();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreFocusedEditable();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreTypeText(string utf8, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreKeyAction(int action, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreSetUserAgentMobile(int mobile);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern void WebCoreSetUserAgentString(string ua);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreEnableCompositing();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGpuInit(IntPtr nativeWindow, int w, int h);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreComposite();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreCompositeReadback(IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreGpuSetFlip(int flipH, int flipV);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreGetTitle(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreGetUrl(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetLinkCount();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreGetLink(int i, ref int x, ref int y, ref int w, ref int h, byte[] url, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint WebCoreGetFrameHash();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetPendingResourceCount();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreGetLastError(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreGetDiag(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreFindString(string utf8, int matchCase, int wrap, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreFindNext(int forward, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreFindClear(IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int WebCoreEvalJS(string script, IntPtr out, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreLiveTick(IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreDownload(string url, string outPath);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGpuLayerInfo(IntPtr outBuf, int len);
    }
}

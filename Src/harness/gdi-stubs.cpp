// ============================================================================
// gdi-stubs.cpp — Win32 GDI function stubs for UWP AppContainer
// Cairo's win32 backend references desktop GDI functions via __imp_ prefix.
// These stubs resolve them so the harness links against cairo-complete lib.
// ============================================================================

extern "C" {

// Core GDI object functions
int __stdcall DeleteObject(void*) { return 1; }
void* __stdcall SelectObject(void*, void* h) { return h; }
void* __stdcall CreateCompatibleDC(void*) { return 0; }
int __stdcall DeleteDC(void*) { return 1; }
int __stdcall SaveDC(void*) { return 1; }
int __stdcall RestoreDC(void*, int) { return 1; }

// Text functions
int __stdcall SetTextColor(void*, unsigned long) { return 0; }
int __stdcall SetBkMode(void*, int) { return 0; }
int __stdcall SetTextAlign(void*, unsigned int) { return 0; }
int __stdcall ExtTextOutW(void*, int, int, unsigned int, const void*, const wchar_t*, unsigned int, const int*) { return 0; }

// Font metrics
int __stdcall GetTextMetricsA(void*, void* tm) {
    if (tm) {
        int* p = (int*)tm;
        p[0] = 16;  // tmHeight
        p[1] = 12;  // tmAscent
        p[2] = 4;   // tmDescent
    }
    return 1;
}
int __stdcall GetTextMetricsW(void*, void* tm) {
    if (tm) {
        int* p = (int*)tm;
        p[0] = 16;  // tmHeight
        p[1] = 12;  // tmAscent
        p[2] = 4;   // tmDescent
    }
    return 1;
}
unsigned long __stdcall GetOutlineTextMetricsA(void*, unsigned long, void*) { return 0; }
unsigned long __stdcall GetOutlineTextMetricsW(void*, unsigned long, void*) { return 0; }
unsigned long __stdcall GetFontData(void*, unsigned long, unsigned long, void*, unsigned long) { return (unsigned long)-1; }
unsigned long __stdcall GetFontUnicodeRanges(void*, void*) { return 0; }

// Glyph functions
int __stdcall GetGlyphOutlineW(void*, unsigned int, unsigned int, void*, unsigned long, void*, const void*) { return -1; }
int __stdcall GetCharWidth32A(void*, unsigned int, unsigned int, int*) { return 0; }
int __stdcall GetCharWidth32W(void*, unsigned int, unsigned int, int*) { return 0; }
unsigned short __stdcall GetGlyphIndicesW(void*, const unsigned short*, unsigned long, unsigned short*, unsigned long) { return 0; }

// Bitmap
void* __stdcall CreateBitmap(int, int, unsigned int, unsigned int, const void*) { return 0; }
void* __stdcall CreateCompatibleBitmap(void*, int, int) { return 0; }

// Font
void* __stdcall CreateFontIndirectA(const void*) { return 0; }
void* __stdcall CreateFontIndirectW(const void*) { return 0; }
void* __stdcall CreateFontW(int, int, int, int, int, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, const wchar_t*) { return 0; }

// Transform
int __stdcall SetGraphicsMode(void*, int) { return 0; }
int __stdcall SetWorldTransform(void*, const void*) { return 0; }
int __stdcall SetMapMode(void*, int) { return 0; }
int __stdcall ModifyWorldTransform(void*, const void*, unsigned long) { return 0; }
int __stdcall GetGraphicsMode(void*) { return 0; }
int __stdcall GetWorldTransform(void*, void*) { return 0; }

// Region / clipping
void* __stdcall ExtCreateRegion(const void*, unsigned long, const void*) { return 0; }
int __stdcall ExtSelectClipRgn(void*, void*, int) { return 0; }
int __stdcall SelectClipRgn(void*, void*) { return 0; }
int __stdcall IntersectClipRect(void*, int, int, int, int) { return 0; }
int __stdcall GetClipBox(void*, void*) { return 0; }
void* __stdcall CreateRectRgn(int, int, int, int) { return 0; }
int __stdcall GetClipRgn(void*, void*) { return 0; }

// DIB
void* __stdcall CreateDIBSection(void*, const void*, unsigned int, void**, void*, unsigned long) { return 0; }

// GDI flush
int __stdcall GdiFlush(void) { return 1; }

// DC operations
void* __stdcall GetDC(void*) { return 0; }
void* __stdcall GetWindowDC(void*) { return 0; }
int __stdcall ReleaseDC(void*, void*) { return 1; }
int __stdcall GetDeviceCaps(void*, int) { return 0; }
int __stdcall GetSystemMetrics(int) { return 0; }
int __stdcall GetDIBits(void*, void*, unsigned int, unsigned int, void*, void*, unsigned int) { return 0; }
int __stdcall BitBlt(void*, int, int, int, int, void*, int, int, unsigned long) { return 0; }
int __stdcall StretchDIBits(void*, int, int, int, int, int, int, int, int, const void*, const void*, unsigned int, unsigned long) { return 0; }

// Brush
void* __stdcall CreateSolidBrush(unsigned long) { return 0; }
int __stdcall FillRect(void*, const void*, void*) { return 0; }
int __stdcall AlphaBlend(void*, int, int, int, int, void*, int, int, int, int, unsigned long) { return 0; }

// Object
int __stdcall GetObjectW(void*, int, void*) { return 0; }

// System
int __stdcall SystemParametersInfoA(unsigned int, unsigned int, void*, unsigned int) { return 0; }
int __stdcall SystemParametersInfoW(unsigned int, unsigned int, void*, unsigned int) { return 0; }

// Printer
int __stdcall StartPage(void*) { return 1; }
int __stdcall EndPage(void*) { return 1; }
int __stdcall StartDocW(void*, const void*) { return 1; }
int __stdcall EndDoc(void*) { return 1; }
int __stdcall Escape(void*, int, int, const void*, void*) { return 0; }

} // extern "C"

// ============================================================================
// NOTE (removed): MSVC STL intrinsic stubs (__std_min_element_f, __std_smf_hypot3f,
// __std_find_trivial_*, __std_reverse_trivially_swappable_*, __std_mismatch_1, etc.)
// were previously defined here for clang-compiled WebCore objs that reference them.
// Since the MSVC 14.44 STL, these symbols are provided by the SRTL runtime
// (msvcprt.lib / msvcp140.dll, implicit via /MD) AND the STL headers now declare them
// with different signatures (const void* + bool), so a local override caused C2733
// (cannot overload extern "C" function). They resolve at link from msvcp140.dll.
// The GDI stubs above (needed for Cairo's win32 backend) are unaffected.
// ============================================================================

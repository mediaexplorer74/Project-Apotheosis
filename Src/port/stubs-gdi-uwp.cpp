// ============================================================================
// stubs-gdi-uwp.cpp — Win32 GDI function stubs for UWP AppContainer
// Cairo's win32-font backend references these desktop GDI functions,
// but UWP AppContainer does not expose them. Providing stub implementations
// so the link succeeds.
// ============================================================================

#include "config.h"

// Provide stub implementations for GDI functions that Cairo references.
// These are desktop-only APIs not available in UWP AppContainer.

extern "C" {

// SystemParametersInfoA/W: stubbed on purpose. The engine link includes
// WindowsApp.lib, whose api-ms-win-ntuser-sysparams-l1-1-0.dll member would
// otherwise satisfy the references from the static cairo win32 objects and
// make WebCore.dll import that api-set at runtime. Whether it exists on
// Windows 10 Mobile 15254 is not guaranteed, and the device proved fatal on
// ANY unresolvable import, so the stubs win via /FORCE:MULTIPLE instead.

// SystemParametersInfo
int __stdcall SystemParametersInfoA(unsigned int, unsigned int, void*, unsigned int) { return 0; }
int __stdcall SystemParametersInfoW(unsigned int, unsigned int, void*, unsigned int) { return 0; }
// The static cairo win32 objects were compiled with dllimport, so they
// reference the __imp_ forms. Defining them here stops the WindowsApp.lib
// api-ms-win-ntuser-sysparams member from ever being pulled into the link:
// no member, no duplicate, no runtime dependency on that api-set.
int __stdcall __imp_SystemParametersInfoA(unsigned int, unsigned int, void*, unsigned int) { return 0; }
int __stdcall __imp_SystemParametersInfoW(unsigned int, unsigned int, void*, unsigned int) { return 0; }

// GDI object functions
int __stdcall DeleteObject(void*) { return 1; }
void* __stdcall SelectObject(void*, void* h) { return h; }
void* __stdcall CreateCompatibleDC(void*) { return 0; }
int __stdcall DeleteDC(void*) { return 1; }

// DC state
int __stdcall SaveDC(void*) { return 1; }
int __stdcall RestoreDC(void*, int) { return 1; }

// Text functions
int __stdcall SetTextColor(void*, unsigned long) { return 0; }
int __stdcall SetBkMode(void*, int) { return 0; }
int __stdcall SetTextAlign(void*, unsigned int) { return 0; }

// Font metrics
typedef struct { int tmHeight; int tmAscent; int tmDescent; int tmInternalLeading;
    int tmExternalLeading; int tmAveCharWidth; int tmMaxCharWidth; int tmWeight;
    int tmItalic; int tmUnderlined; int tmStruckOut; int tmPitchAndFamily;
    int tmCharSet; char tmFirstChar; char tmLastChar; } FakeTEXTMETRICA;

int __stdcall GetTextMetricsA(void*, FakeTEXTMETRICA* tm) {
    if (tm) {
        tm->tmHeight = 16; tm->tmAscent = 12; tm->tmDescent = 4;
        tm->tmInternalLeading = 2; tm->tmExternalLeading = 0;
        tm->tmAveCharWidth = 8; tm->tmMaxCharWidth = 16;
        tm->tmWeight = 400; tm->tmItalic = 0; tm->tmUnderlined = 0;
        tm->tmStruckOut = 0; tm->tmPitchAndFamily = 1; tm->tmCharSet = 0;
        tm->tmFirstChar = 32; tm->tmLastChar = 126;
    }
    return 1;
}

typedef struct { int tmHeight; int tmAscent; int tmDescent; int tmInternalLeading;
    int tmExternalLeading; int tmAveCharWidth; int tmMaxCharWidth; int tmWeight;
    int tmItalic; int tmUnderlined; int tmStruckOut; int tmPitchAndFamily;
    int tmCharSet; wchar_t tmFirstChar; wchar_t tmLastChar; } FakeTEXTMETRICW;

int __stdcall GetTextMetricsW(void*, FakeTEXTMETRICW* tm) {
    if (tm) {
        tm->tmHeight = 16; tm->tmAscent = 12; tm->tmDescent = 4;
        tm->tmInternalLeading = 2; tm->tmExternalLeading = 0;
        tm->tmAveCharWidth = 8; tm->tmMaxCharWidth = 16;
        tm->tmWeight = 400; tm->tmItalic = 0; tm->tmUnderlined = 0;
        tm->tmStruckOut = 0; tm->tmPitchAndFamily = 1; tm->tmCharSet = 0;
        tm->tmFirstChar = 32; tm->tmLastChar = 126;
    }
    return 1;
}

unsigned long __stdcall GetFontData(void*, unsigned long, unsigned long, void*, unsigned long) { return (unsigned long)-1; }
unsigned long __stdcall GetFontUnicodeRanges(void*, void*) { return 0; }

// Glyph functions
typedef struct { int gmBlackBoxX; int gmBlackBoxY; int gmptGlyphOriginX;
    int gmptGlyphOriginY; int gmCellIncX; int gmCellIncY; } FakeGLYPHMETRICS;
typedef struct { short eM11, eM11Fract, eM12, eM12Fract, eM21, eM21Fract, eM22, eM22Fract; } FakeMAT2;

int __stdcall GetGlyphOutlineW(void*, unsigned int, unsigned int, FakeGLYPHMETRICS*, unsigned long, void*, const FakeMAT2*) { return -1; }
int __stdcall GetCharWidth32A(void*, unsigned int, unsigned int, int*) { return 0; }
int __stdcall GetCharWidth32W(void*, unsigned int, unsigned int, int*) { return 0; }
unsigned short __stdcall GetGlyphIndicesW(void*, const unsigned short*, unsigned long, unsigned short*, unsigned long) { return 0; }

// Bitmap
void* __stdcall CreateBitmap(int, int, unsigned int, unsigned int, const void*) { return 0; }

// Font
void* __stdcall CreateFontIndirectA(const void*) { return 0; }
void* __stdcall CreateFontW(int, int, int, int, int, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, const wchar_t*) { return 0; }
void* __stdcall CreateFontIndirectW(const void*) { return 0; }

// Additional GDI functions referenced by Cairo win32 backend
int __stdcall GetObjectW(void*, int, void*) { return 0; }
int __stdcall SetGraphicsMode(void*, int) { return 0; }
int __stdcall SetWorldTransform(void*, const void*) { return 0; }
int __stdcall SetMapMode(void*, int) { return 0; }
int __stdcall ModifyWorldTransform(void*, const void*, unsigned long) { return 0; }
unsigned long __stdcall GetOutlineTextMetricsA(void*, unsigned long, void*) { return 0; }
unsigned long __stdcall GetOutlineTextMetricsW(void*, unsigned long, void*) { return 0; }
int __stdcall ExtTextOutW(void*, int, int, unsigned int, const void*, const wchar_t*, unsigned int, const int*) { return 0; }
void* __stdcall ExtCreateRegion(const void*, unsigned long, const void*) { return 0; }
int __stdcall ExtSelectClipRgn(void*, void*, int) { return 0; }
int __stdcall GetGraphicsMode(void*) { return 0; }
int __stdcall GetWorldTransform(void*, void*) { return 0; }
int __stdcall SelectClipRgn(void*, void*) { return 0; }
int __stdcall IntersectClipRect(void*, int, int, int, int) { return 0; }
int __stdcall GetClipBox(void*, void*) { return 0; }
void* __stdcall CreateRectRgn(int, int, int, int) { return 0; }
int __stdcall GetClipRgn(void*, void*) { return 0; }
void* __stdcall CreateDIBSection(void*, const void*, unsigned int, void**, void*, unsigned long) { return 0; }
int __stdcall GdiFlush(void) { return 1; }
void* __stdcall GetDC(void*) { return 0; }
void* __stdcall GetWindowDC(void*) { return 0; }
int __stdcall ReleaseDC(void*, void*) { return 1; }
int __stdcall GetDeviceCaps(void*, int) { return 0; }
int __stdcall GetSystemMetrics(int) { return 0; }
int __stdcall GetDIBits(void*, void*, unsigned int, unsigned int, void*, void*, unsigned int) { return 0; }
void* __stdcall CreateCompatibleBitmap(void*, int, int) { return 0; }
int __stdcall BitBlt(void*, int, int, int, int, void*, int, int, unsigned long) { return 0; }
int __stdcall StretchDIBits(void*, int, int, int, int, int, int, int, int, const void*, const void*, unsigned int, unsigned long) { return 0; }
int __stdcall StartPage(void*) { return 1; }
int __stdcall EndPage(void*) { return 1; }
int __stdcall StartDocW(void*, const void*) { return 1; }
int __stdcall EndDoc(void*) { return 1; }
int __stdcall Escape(void*, int, int, const void*, void*) { return 0; }
void* __stdcall CreateSolidBrush(unsigned long) { return 0; }
int __stdcall FillRect(void*, const void*, void*) { return 0; }
int __stdcall AlphaBlend(void*, int, int, int, int, void*, int, int, int, int, unsigned long) { return 0; }

} // extern "C"

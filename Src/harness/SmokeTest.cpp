// SmokeTest.cpp — drive the JavaScriptCore C API to run JS, verifying the JSC port runs on ARM32 UWP.
#include "pch.h"
#include "SmokeTest.h"

#include <JavaScriptCore/JavaScript.h>
#include <string>

static std::wstring toWide(JSStringRef s)
{
    size_t len = JSStringGetLength(s);
    std::wstring out(len, L'\0');
    // JSChar is UTF-16, matching Windows wchar_t.
    const JSChar* chars = JSStringGetCharactersPtr(s);
    for (size_t i = 0; i < len; ++i)
        out[i] = static_cast<wchar_t>(chars[i]);
    return out;
}

std::wstring RunJscSmokeTest()
{
    JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
    if (!ctx)
        return L"JSGlobalContextCreate FAILED";

    JSStringRef script = JSStringCreateWithUTF8CString("1 + 1");
    JSValueRef exception = nullptr;
    JSValueRef result = JSEvaluateScript(ctx, script, nullptr, nullptr, 0, &exception);
    JSStringRelease(script);

    std::wstring text;
    if (exception) {
        JSStringRef es = JSValueToStringCopy(ctx, exception, nullptr);
        text = L"JS exception: " + toWide(es);
        JSStringRelease(es);
    } else {
        double n = JSValueToNumber(ctx, result, nullptr);
        // Also run something slightly more complex to prove the interpreter really works.
        JSStringRef s2 = JSStringCreateWithUTF8CString("(function(){var s=0;for(var i=1;i<=100;i++)s+=i;return s;})()");
        JSValueRef r2 = JSEvaluateScript(ctx, s2, nullptr, nullptr, 0, nullptr);
        JSStringRelease(s2);
        double sum = JSValueToNumber(ctx, r2, nullptr);

        wchar_t buf[128];
        swprintf_s(buf, L"1 + 1 = %g\nΣ1..100 = %g\n\nJSC on ARM32 UWP ✓", n, sum);
        text = buf;
    }

    JSGlobalContextRelease(ctx);
    return text;
}

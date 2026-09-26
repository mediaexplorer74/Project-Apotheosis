#include "pch.h"
#include "MainPage.xaml.h"
#include "MainPage.g.hpp"
#include <windows.h>
#include <fstream>

using namespace JITTest;
using namespace Platform;
using namespace Windows::UI::Xaml;

// Thumb-2: movs r0, #42 ; bx lr   ->  should return 42
static const unsigned char kCode[] = { 0x2A, 0x20, 0x70, 0x47 };

// Separate function for the SEH __try/__except (no C++ objects, to avoid C2712). If execution is blocked
// by DEP/ACG it raises an access violation, which we catch here.
static bool tryCallThumb(void* mem, int* out)
{
    typedef int (*Fn)();
    Fn f = reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(mem) | 1u);   // ARM Thumb bit
    __try {
        *out = f();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct TR {
    bool allocOk = false; unsigned long allocErr = 0;
    bool protectOk = false; unsigned long protectErr = 0;
    bool called = false; bool crashed = false; int ret = 0;
};

static TR runScenario(unsigned long allocProt, bool doReprotect, unsigned long reprotectFlag)
{
    TR r;
    SIZE_T sz = 4096;
    void* mem = VirtualAllocFromApp(nullptr, sz, MEM_COMMIT | MEM_RESERVE, allocProt);
    if (!mem) { r.allocErr = GetLastError(); return r; }
    r.allocOk = true;
    std::memcpy(mem, kCode, sizeof(kCode));   // must be writable at this point (RW or RWX)
    if (doReprotect) {
        ULONG old = 0;
        if (!VirtualProtectFromApp(mem, sz, reprotectFlag, &old)) {
            r.protectErr = GetLastError();
            VirtualFree(mem, 0, MEM_RELEASE);
            return r;
        }
    }
    r.protectOk = true;
    FlushInstructionCache(GetCurrentProcess(), mem, sz);
    int out = 0;
    if (tryCallThumb(mem, &out)) { r.called = true; r.ret = out; }
    else { r.crashed = true; }
    VirtualFree(mem, 0, MEM_RELEASE);
    return r;
}

static std::wstring fmt(const wchar_t* name, const TR& r)
{
    wchar_t buf[600];
    if (!r.allocOk)
        swprintf_s(buf, L"%s\n  alloc FAILED err=%lu\n\n", name, r.allocErr);
    else if (!r.protectOk)
        swprintf_s(buf, L"%s\n  alloc OK, reprotect-to-exec DENIED err=%lu  <- JIT impossible\n\n", name, r.protectErr);
    else if (r.crashed)
        swprintf_s(buf, L"%s\n  alloc OK, protect OK, but execution CRASHED (DEP/ACG blocks)  <- JIT impossible\n\n", name);
    else if (r.called && r.ret == 42)
        swprintf_s(buf, L"%s\n  SUCCESS! alloc -> write -> exec -> call returned %d  <- JIT possible!\n\n", name, r.ret);
    else
        swprintf_s(buf, L"%s\n  call returned unexpected %d (expected 42)\n\n", name, r.ret);
    return buf;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

MainPage::MainPage()
{
    InitializeComponent();
    RunTests();
}

void MainPage::RunTests()
{
    std::wstring log;
    log += L"Goal: can the App Container allocate and execute runtime-generated code (the JIT prerequisite).\n";
    log += L"Test code: Thumb-2 movs r0,#42; bx lr (should return 42).\n\n";

    log += fmt(L"[A] alloc RW -> write -> reprotect RX (W^X, modern JSC usage)", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READ));
    log += fmt(L"[B] allocate RWX directly (PAGE_EXECUTE_READWRITE)", runScenario(PAGE_EXECUTE_READWRITE, false, 0));
    log += fmt(L"[C] alloc RW -> write -> reprotect RWX", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READWRITE));

    log += L"Verdict: any OK = JIT possible (A is the decisive one). All fail/crash = JIT impossible, fall back to asm LLInt.\n";

    ResultText->Text = ref new String(log.c_str());

    // Write LocalState\jitresult.txt (UTF-8) for WDP to pull.
    try {
        auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
        std::wstring path = std::wstring(local->Data()) + L"\\jitresult.txt";
        std::ofstream f(WideToUtf8(path), std::ios::binary | std::ios::trunc);
        if (f) { std::string u = WideToUtf8(log); f.write(u.data(), u.size()); }
    } catch (...) {}
}

// JitProbe.cpp — verify whether the App Container can allocate and execute runtime-generated code (the JIT prerequisite).
// Pure C++ + SEH; must be compiled separately with CompileAsWinRT=false (C++/CX /ZW disables __try/__except).
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

// Thumb-2: movs r0, #42 ; bx lr  ->  the call should return 42
static const unsigned char kCode[] = { 0x2A, 0x20, 0x70, 0x47 };

// SEH wraps the actual call: if execution is blocked by DEP/ACG it raises an access violation,
// which we catch here instead of crashing.
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
    std::memcpy(mem, kCode, sizeof(kCode));   // must be writable (RW or RWX)
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

static std::string line(const char* name, const TR& r)
{
    char buf[400];
    if (!r.allocOk)
        std::snprintf(buf, sizeof buf, "%s: alloc FAILED err=%lu\n", name, r.allocErr);
    else if (!r.protectOk)
        std::snprintf(buf, sizeof buf, "%s: reprotect-to-exec DENIED err=%lu  (JIT impossible)\n", name, r.protectErr);
    else if (r.crashed)
        std::snprintf(buf, sizeof buf, "%s: protect OK but execution CRASHED (DEP/ACG)  (JIT impossible)\n", name);
    else if (r.called && r.ret == 42)
        std::snprintf(buf, sizeof buf, "%s: OK returned %d  *JIT possible!*\n", name, r.ret);
    else
        std::snprintf(buf, sizeof buf, "%s: call returned unexpected %d (expected 42)\n", name, r.ret);
    return buf;
}

std::string RunJitProbe()
{
    std::string s = "=== JIT executable-memory test (Thumb-2 movs r0,#42; bx lr should return 42) ===\n";
    s += line("[A] RW->reprotect RX (W^X, modern JSC usage)", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READ));
    s += line("[B] allocate RWX directly", runScenario(PAGE_EXECUTE_READWRITE, false, 0));
    s += line("[C] RW->reprotect RWX", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READWRITE));
    s += "Verdict: any * = JIT possible (A is the decisive one); all fail/crash = JIT impossible, fall back to asm LLInt.\n";
    return s;
}

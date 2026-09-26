#include "pch.h"
#include "MainPage.xaml.h"
#include "MainPage.g.h"
#include "WebCoreDriver.h"
#include "JitProbe.h"
#include "GpuProbe.h"

#include <robuffer.h>
#include <wrl.h>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <algorithm>
#include <cwctype>
#include <cstdlib>
#include <cstdio>   // std::sscanf -- scripted tap commands (TapFromScript)
#include <csignal>
#include <cstdarg>
#include <cstring>   // std::strcmp -- per-job wedge threshold (2026-09-19)
#include <exception>
#include <ppltasks.h>
#include <collection.h>

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

using namespace Harness;
using namespace Platform;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::UI::Core;
using namespace Microsoft::WRL;

// Apotheosis: the linker-provided base address of this module, i.e. exactly what
// GetModuleHandleW(nullptr) returns, without calling it. SDK 10.0.19041.0 keeps GetModuleHandleW
// outside WINAPI_PARTITION_APP, and the ARM line has to build against 19041 because 26100 ships no
// ARM32 libraries -- so the API form does not compile for ARM at all. __ImageBase is resolved at
// link time and needs no loader call, which also makes it safe to read from a crash handler.
extern "C" IMAGE_DOS_HEADER __ImageBase;

// ===== Logger — writes to <LocalState>\log.txt =====
// Logs messages with timestamps to <LocalState>\log.txt for diagnostics.
// Buffer is flushed to disk on LogInit; max 512 lines held before init.

static std::mutex g_logMutex;
static bool g_logInitialized = false;
static std::wstring g_logPath;
static std::vector<std::string> g_logBuffer; // pre-init buffer, flushed to LocalState on LogInit()
static const size_t kLogMaxBuffer = 512;

static std::string LogTimestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

// Apotheosis 2026-09-24: keep the previous session's log instead of truncating it away.
// log.txt is opened with trunc on the first LogInit of a process, so the *first relaunch after a
// crash* is what destroys the only record of it -- including the VEH block ApoFirstChance had
// already written and flushed line by line. Measured cost: the nav-load access violation of
// 2026-09-19 07:57:43 (job=nav-load, stage=before-load, faultaddr holding ASCII) left nothing
// recoverable, because RunCrashVerdict's crash-report.txt is a *single slot* and a later launch had
// already overwritten it -- 41 crashes in the two days up to that point, one preserved file.
// Rotation is one rename per process and keeps the newest kLogRotations sessions, so a fatal crash
// can be read instead of theorised about. The name carries the source file's own last-write time,
// so it says when the session that produced it *ended*, not when it was rotated.
static const int kLogRotations = 12;

// Returns the rotated file's name (ASCII, for the log line) or an empty string if there was nothing
// to keep. Never throws and never fails loudly: it runs on the engine thread before any logging.
static std::string ApoRotatePreviousLog(const std::wstring& localStateDir)
{
    try {
        const std::wstring src = localStateDir + L"\\log.txt";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad)) return {};
        if (fad.nFileSizeHigh == 0 && fad.nFileSizeLow == 0) return {};

        SYSTEMTIME st{};
        FILETIME ft{};
        if (!FileTimeToLocalFileTime(&fad.ftLastWriteTime, &ft) || !FileTimeToSystemTime(&ft, &st))
            GetLocalTime(&st);

        char name[64];
        sprintf_s(name, "log-prev-%02u%02u-%02u%02u%02u.txt",
            (unsigned)st.wMonth, (unsigned)st.wDay, (unsigned)st.wHour,
            (unsigned)st.wMinute, (unsigned)st.wSecond);

        wchar_t nameW[64];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, nameW, 64) <= 0) return {};
        const std::wstring dst = localStateDir + L"\\" + nameW;
        if (!MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) return {};

        // Prune the oldest. The names are fixed width and sort chronologically within a year, which
        // is the whole lifetime of this diagnostic; two rotations inside one second are not expected,
        // since rotation runs once per process.
        const std::wstring pattern = localStateDir + L"\\log-prev-*.txt";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return std::string(name);
        std::vector<std::wstring> names;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if ((int)names.size() > kLogRotations) {
            std::sort(names.begin(), names.end());
            for (size_t i = 0; i + (size_t)kLogRotations < names.size(); ++i) {
                const std::wstring victim = localStateDir + L"\\" + names[i];
                DeleteFileW(victim.c_str());
            }
        }
        return std::string(name);
    } catch (...) { return {}; }
}

static void LogInit(const std::wstring& localStateDir)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logInitialized) return;
    g_logPath = localStateDir + L"\\log.txt";
    const std::string rotated = ApoRotatePreviousLog(localStateDir);
    {
        std::ofstream f(g_logPath.c_str(), std::ios::binary | std::ios::trunc);
        if (f) {
            f << "=== Harness Log Started ===\n";
            if (!rotated.empty())
                f << "previous session's log preserved as " << rotated << "\n";
        }
    }
    for (const auto& line : g_logBuffer) {
        std::ofstream f(g_logPath.c_str(), std::ios::binary | std::ios::app);
        if (f) f << line;
    }
    g_logBuffer.clear();
    g_logBuffer.shrink_to_fit();
    g_logInitialized = true;
}

static void LogWrite(const char* msg)
{
    std::string line = LogTimestamp() + "  " + msg + "\n";
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logInitialized && !g_logPath.empty()) {
        std::ofstream f(g_logPath.c_str(), std::ios::binary | std::ios::app);
        if (f) f.write(line.data(), line.size());
    } else {
        if (g_logBuffer.size() < kLogMaxBuffer)
            g_logBuffer.push_back(line);
    }
}

static void LogWriteF(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    // Apotheosis 2026-09-24: was 2048, and that was the binding cut on the `latediag` samples -- a
    // resource-heavy page's diag (29 resources on lenta.ru) runs past it, and the line was cut
    // mid-filename with nothing to say so. Raising it alone would not have been enough either: the
    // port's own builder truncated at 4096 and closed the bracket as if the list were complete. Both
    // halves are fixed, and the port's list now declares its own remainder (`+N more]`).
    char buf[8192];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LogWrite(buf);
}

// Forward declaration: the real definition (and the module list it needs) lives further down, and
// the death net is installed before it. Declaring the function keeps the net self-contained.
static void ApoFormatAddr(char* out, size_t cb, uintptr_t a);

// Apotheosis 2026-09-18: AddVectoredExceptionHandler is declared in <errhandlingapi.h> only under
// WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM), and an App-Container
// build sees neither -- which is the fact behind the old "VEH not available in UWP" comment this
// replaces. Measured: the error is `C3861: 'AddVectoredExceptionHandler': identifier not found`.
// The same function is exported by ntdll as RtlAddVectoredExceptionHandler -- kernel32's own
// implementation calls into it -- so the net reaches it one layer down. If this link ever fails on
// a target, the correct response is to drop the VEH and say so, not to invent another hook.
extern "C" PVOID NTAPI RtlAddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
#pragma comment(lib, "ntdll.lib")

// ===== The death net: first-chance exceptions, abort, pure call =====
// Why this exists (2026-09-18, dzen.ru). A session that scrolls a heavy page dies leaving *every*
// channel silent at once: log.txt simply stops, TERMINATE: is absent (std::set_terminate is
// installed and does not run), UEF: is absent (SetUnhandledExceptionFilter is installed and does not
// run), unhandled.txt is absent, wtfcash is absent, no Windows Application-log event is recorded and
// WER leaves no dump. Meanwhile the UI thread keeps beating to within 0.3-1.2 s of the end, so the
// process was healthy and then was not.
//
// At least four different mechanisms produce that signature, and they need different fixes:
//
//   * a SEH exception that something *handles* and then a second, unrelated mechanism kills us
//     -> only a FIRST-CHANCE handler can see it, and that is exactly what was missing;
//   * abort(), which on this UCRT becomes a Watson fast-fail (0xC0000409) that runs no user handler
//     at all -> needs both signal(SIGABRT) and _set_abort_behavior to be tamed;
//   * a pure-virtual call or a CRT parameter check -> _set_purecall_handler (the invalid-parameter
//     handler is already installed above);
//   * a platform kill, which nothing in-process can see (that is what cdb is for).
//
// The handler below runs FIRST, before any SEH frame, so it sees an exception even when the code
// that raises it goes on to handle it successfully. It is deliberately cheap for routine codes:
// WinRT and the CRT raise and catch exceptions as ordinary control flow, and a handler that logs
// every one of those would slow the process it is measuring and bury the interesting line.
static LONG CALLBACK ApoFirstChance(EXCEPTION_POINTERS* p)
{
    // Re-entrancy guard. A first-chance handler runs at the exception point, so if the fault is
    // raised from inside LogWrite itself the handler would take g_logMutex while the same thread
    // already holds it -- a deadlock in the one code path that exists to make a dead process talk.
    // Thread-local, so a fault on another thread is still reported.
    static thread_local bool s_inHandler = false;
    if (s_inHandler) return EXCEPTION_CONTINUE_SEARCH;
    struct Guard { bool& f; Guard(bool& b) : f(b) { f = true; } ~Guard() { f = false; } } guard(s_inHandler);

    const DWORD  code = p->ExceptionRecord->ExceptionCode;
    const void*  addr = p->ExceptionRecord->ExceptionAddress;
    const DWORD  tid  = GetCurrentThreadId();

    // Codes that can end the process. Everything else is counted, not logged in full.
    const bool interesting =
        code == 0xC0000005 ||   // access violation
        code == 0xC000001D ||   // illegal instruction (__builtin_trap -> ud2, i.e. WebKit CRASH())
        code == 0xC0000409 ||   // fast fail
        code == 0xC00000FD ||   // stack overflow
        code == 0xC0000374 ||   // heap corruption
        code == 0xC0000602 ||   // fail fast
        code == 0xC0000006 ||   // in-page error
        code == 0xC0000094 ||   // integer divide by zero
        code == 0xC0000096 ||   // privileged instruction
        code == 0x80000003;     // breakpoint (__debugbreak)

    // Per-code counters. Two arrays indexed by a small slot map keep this branch-free enough to run
    // on every exception in the process.
    static volatile LONG s_hits[16];
    static volatile LONG s_routine;
    int slot = -1;
    switch (code) {
    case 0xC0000005: slot = 0;  break;
    case 0xC000001D: slot = 1;  break;
    case 0xC0000409: slot = 2;  break;
    case 0xC00000FD: slot = 3;  break;
    case 0xC0000374: slot = 4;  break;
    case 0xC0000602: slot = 5;  break;
    case 0xC0000006: slot = 6;  break;
    case 0xC0000094: slot = 7;  break;
    case 0xC0000096: slot = 8;  break;
    case 0x80000003: slot = 9;  break;
    default:         slot = -1; break;
    }

    if (!interesting || slot < 0) {
        const LONG n = InterlockedIncrement(&s_routine);
        if (n <= 8 || (n % 512) == 0)
            LogWriteF("VEH-routine: code=0x%08X addr=%p tid=%lu n=%ld",
                (unsigned)code, addr, (unsigned long)tid, (long)n);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const LONG n = InterlockedIncrement(&s_hits[slot]);
    if (n > 8 && (n % 256) != 0)
        return EXCEPTION_CONTINUE_SEARCH;   // keep the channel cheap; the count is in the sparse line

    // For an access violation ExceptionInformation[0] says read(0)/write(1)/execute(8) and [1] is the
    // faulting *data* address -- the field that separates a wild pointer from a deliberate WebKit
    // CRASH(), which stores to a poison address.
    if (code == 0xC0000005 && p->ExceptionRecord->NumberParameters >= 2)
        LogWriteF("VEH: code=0x%08X addr=%p tid=%lu n=%ld op=%llu faultaddr=%p",
            (unsigned)code, addr, (unsigned long)tid, (long)n,
            (unsigned long long)p->ExceptionRecord->ExceptionInformation[0],
            (void*)p->ExceptionRecord->ExceptionInformation[1]);
    else
        LogWriteF("VEH: code=0x%08X addr=%p tid=%lu n=%ld params=%lu",
            (unsigned)code, addr, (unsigned long)tid, (long)n,
            (unsigned long)p->ExceptionRecord->NumberParameters);

    if (n <= 8) {
        void* bt[40];
        const USHORT frames = CaptureStackBackTrace(0, 40, bt, nullptr);
        LogWriteF("VEH: stack (%u frames):", frames);
        for (USHORT i = 0; i < frames; ++i) {
            char a[128];
            ApoFormatAddr(a, sizeof a, (uintptr_t)bt[i]);
            LogWriteF("  VEH #%u %s", (unsigned)i, a);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;   // never change behaviour: we are an observer
}

// Apotheosis: the installers, called once from WebEngine::loop().
// AddVectoredExceptionHandler with First=1 runs before every SEH frame; this was avoided for months
// on the belief that it is unavailable in UWP ("VEH not available in UWP", the comment this
// replaces). If the UWP link rejects it, the fallback is ntdll's RtlAddVectoredExceptionHandler,
// which is the same function one layer down.
static void ApoInstallDeathNet()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    PVOID veh = RtlAddVectoredExceptionHandler(1, ApoFirstChance);
    LogWriteF("DEATHNET: RtlAddVectoredExceptionHandler -> %p (err=%lu)",
        veh, (unsigned long)GetLastError());

    // abort(): UCRT raises SIGABRT, and unless the report-fault behaviour is switched off the abort
    // becomes a Watson fast-fail that runs NO user handler -- which is one of the ways this family
    // stays silent. Log first, then let the (now harmless) abort proceed: the exit code becomes 3,
    // which is itself the diagnosis.
    void (*prev)(int) = signal(SIGABRT, [](int) {
        LogWriteF("SIGABRT: tid=%lu -- abort() reached the CRT signal handler",
            (unsigned long)GetCurrentThreadId());
        void* bt[40];
        const USHORT frames = CaptureStackBackTrace(0, 40, bt, nullptr);
        LogWriteF("SIGABRT: stack (%u frames):", frames);
        for (USHORT i = 0; i < frames; ++i) {
            char a[128];
            ApoFormatAddr(a, sizeof a, (uintptr_t)bt[i]);
            LogWriteF("  ABRT #%u %s", (unsigned)i, a);
        }
    });
    LogWriteF("DEATHNET: signal(SIGABRT) prev=%p", (void*)prev);
    const unsigned oldBehavior = _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    LogWriteF("DEATHNET: _set_abort_behavior -> prev=0x%X", (unsigned)oldBehavior);

    _set_purecall_handler([] {
        LogWriteF("PURECALL: tid=%lu -- pure virtual call",
            (unsigned long)GetCurrentThreadId());
        void* bt[40];
        const USHORT frames = CaptureStackBackTrace(0, 40, bt, nullptr);
        LogWriteF("PURECALL: stack (%u frames):", frames);
        for (USHORT i = 0; i < frames; ++i) {
            char a[128];
            ApoFormatAddr(a, sizeof a, (uintptr_t)bt[i]);
            LogWriteF("  PURC #%u %s", (unsigned)i, a);
        }
    });
    LogWriteF("DEATHNET: _set_purecall_handler installed");
}

// ===== Crash verdict + heartbeat: making a dead phone talk =====
// On a Lumia the app can die with nothing but a frozen frame -- no debugger (VS 2022 dropped ARM32
// device debugging), and WER data never leaves the phone. Two cheap markers turn the next launch
// into an autopsy: exit-ok.txt is written only by App::OnSuspending (a crash never suspends), and
// heartbeat.txt is refreshed every 2 s by a UI timer. At startup:
//   exit-ok present              -> previous session ended cleanly
//   exit-ok missing + fresh beat -> died inside the last ~2 s window
//   exit-ok missing + stale beat -> died earlier, or never got far enough to beat
// RunCrashVerdict runs from App::OnLaunched, strictly before the engine thread's LogInit truncates
// log.txt, so the dying session's log can be preserved as crash-report.txt first.
static void WriteMarkerFile(const wchar_t* name, const char* text)
{
    try {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
        wcscat_s(path, L"\\");
        wcscat_s(path, name);
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"w") == 0 && f) {
            fputs(text, f);
            fclose(f);
        }
    } catch (...) {}
}

static std::string ReadMarkerFile(const wchar_t* name)
{
    try {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
        wcscat_s(path, L"\\");
        wcscat_s(path, name);
        std::ifstream f(path, std::ios::binary);
        std::string s;
        if (f) std::getline(f, s);
        return s;
    } catch (...) { return std::string(); }
}

// One-line app-memory snapshot for the log: used/limit KB + usage level. Deliberately cheap and
// non-allocating beyond a stack buffer; MemoryManager is a system property (not an engine call),
// so it is safe on both the UI and the engine thread.
static std::string MemoryLine()
{
    char buf[160];
    try {
        const unsigned long long use = Windows::System::MemoryManager::AppMemoryUsage;
        const unsigned long long lim = Windows::System::MemoryManager::AppMemoryUsageLimit;
        const wchar_t* lvl = L"?";
        switch (Windows::System::MemoryManager::AppMemoryUsageLevel) {
        case Windows::System::AppMemoryUsageLevel::Low: lvl = L"low"; break;
        case Windows::System::AppMemoryUsageLevel::Medium: lvl = L"medium"; break;
        case Windows::System::AppMemoryUsageLevel::High: lvl = L"high"; break;
        case Windows::System::AppMemoryUsageLevel::OverLimit: lvl = L"overlimit"; break;
        }
        sprintf_s(buf, "mem %llu/%llu KB level=%ls", use / 1024ULL, lim / 1024ULL, lvl);
    } catch (...) {
        sprintf_s(buf, "mem n/a");
    }
    return std::string(buf);
}

// Apotheosis 2026-09-24: is the current app-memory level actually pressure?
//
// AppMemoryUsageIncreased fires on an upward *crossing*, and the platform counts the first reading
// of a process as one: usage goes from nothing to whatever the app has already allocated, which on a
// fresh launch is `low` -- the LOWEST level. Measured on three launches, always within a millisecond
// of `MainPage: memory-level handlers registered`. Releasing then is wrong twice over: there is no
// pressure, so every byte dropped is a re-decode the Lumia cannot spare -- the same argument the
// LimitChanging handler below already makes for ignoring a ceiling *rise* -- and it posted a job
// ahead of the engine loop's first navigation, which is what killed the app at startup (see
// WebCoreReleaseMemory in Src/port/WebCoreDriver.cpp for the symbolized chain).
//
// `low` is the level below the ones this exists to catch, so only Medium and above may cost the
// caches anything. Medium rather than High on purpose: the failure being prevented is the platform
// reaping the app with no exception and no dump, so the first level that means "more than expected"
// is the one worth reacting to -- and every release logs a line, so if Medium turns out to fire on
// ordinary navigation on the Lumia, the measurement to move this gate to High is already in the log.
static bool MemoryLevelIsPressure()
{
    try {
        switch (Windows::System::MemoryManager::AppMemoryUsageLevel) {
        case Windows::System::AppMemoryUsageLevel::Medium:
        case Windows::System::AppMemoryUsageLevel::High:
        case Windows::System::AppMemoryUsageLevel::OverLimit:
            return true;
        default:
            return false;
        }
    } catch (...) { return false; }
}

void RunCrashVerdict()
{
    try {
        const std::string ok = ReadMarkerFile(L"exit-ok.txt");
        const std::string hb = ReadMarkerFile(L"heartbeat.txt");
        const bool crashed = ok.empty();
        std::string verdict;
        if (crashed) {
            // Preserve the dying session's log.txt before LogInit truncates it.
            try {
                wchar_t src[MAX_PATH], dst[MAX_PATH];
                wcscpy_s(src, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
                wcscat_s(src, L"\\log.txt");
                wcscpy_s(dst, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
                wcscat_s(dst, L"\\crash-report.txt");
                std::ifstream in(src, std::ios::binary);
                if (in) {
                    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
                    if (out) out << in.rdbuf();
                }
            } catch (...) {}
            verdict = "CRASHED last heartbeat=" + (hb.empty() ? std::string("(none)") : hb);
        } else {
            verdict = "clean-exit";
        }
        WriteMarkerFile(L"crashverdict.txt", (verdict + "\n").c_str());
        LogWriteF("CRASHVERDICT: %s", verdict.c_str());
        // Clear the clean-exit marker so a crash in THIS session is detected at the next launch.
        if (!crashed) {
            try {
                wchar_t path[MAX_PATH];
                wcscpy_s(path, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
                wcscat_s(path, L"\\exit-ok.txt");
                DeleteFileW(path);
            } catch (...) {}
        }
    } catch (...) {}
}

// ===== UTF-8 / Wide string conversion helpers =====
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string ToUtf8(Platform::String^ s)
{
    return s ? WideToUtf8(std::wstring(s->Data())) : std::string{};
}
static std::wstring ToWide(const char* s) { return s ? Utf8ToWide(std::string(s)) : std::wstring{}; }

#include "DiagEmpty.h"

// Apotheosis (2026-09-18): does this engine diag string describe a page that produced nothing?
// The parser lives in DiagEmpty.h so it can be unit-tested against real log samples; this wrapper
// only names the intent at the call site.
static bool DiagLooksEmpty(const char* diag, int& bodyKidsOut)
{
    return ApoDiagEmpty::LooksEmpty(diag, bodyKidsOut);
}

static std::wstring LocalStateDir()
{
    using namespace Windows::Storage;
    try { return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()); }
    catch (...) { return {}; }
}
static std::wstring InstallDir()
{
    using namespace Windows::ApplicationModel;
    try { return std::wstring(Package::Current->InstalledLocation->Path->Data()); }
    catch (...) { return {}; }
}

// ===== Runtime environment setup: fontconfig (SimHei CJK fallback) + CA cert blob =====
static void SetupRuntimeEnvOnce()
{
    try {
        // Direct UAPI file write — survives even if LogInit not yet called
        try {
            auto folder = Windows::Storage::ApplicationData::Current->LocalFolder;
            auto file = concurrency::create_task(
                folder->CreateFileAsync(L"setup-env-start.txt",
                    Windows::Storage::CreationCollisionOption::ReplaceExisting)).get();
            concurrency::create_task(
                Windows::Storage::FileIO::WriteTextAsync(file, L"SetupRuntimeEnv entered\n")).get();
        } catch (...) {}
        std::string installDir = WideToUtf8(InstallDir());
        std::string localDir = WideToUtf8(LocalStateDir());
        if (installDir.empty() || localDir.empty()) {
            LogWrite("ERROR: InstallDir or LocalStateDir empty");
            return;
        }
        LogInit(LocalStateDir());
        LogWriteF("SetupRuntimeEnv: installDir=%s localDir=%s", installDir.c_str(), localDir.c_str());
        // Apotheosis: arm the port's marker log here rather than in EnableGpu. It used to be armed
        // only when GPU compositing was switched on, so with GPU off (settings.ini gpudefault=0, and
        // the whole software-rendering path) every gpuLogMarker/portDiagLog line was written to a
        // file that had never been opened, i.e. silently dropped -- including the parser and
        // hit-test probes, which have nothing to do with the GPU. EnableGpu still sets the same
        // path; setting it twice is harmless.
        try { WebCoreSetGpuInitLogFile((localDir + "\\gpuinit-steps.txt").c_str()); } catch (...) {}

        std::string fontsDir = installDir + "\\Assets\\fonts";
        std::string cacheDir = localDir + "\\fontconfig-cache";
        std::string confPath = localDir + "\\fonts.conf";
        LogWriteF("SetupRuntimeEnv: fontsDir=%s cacheDir=%s confPath=%s", fontsDir.c_str(), cacheDir.c_str(), confPath.c_str());
        // Apotheosis: expose the bundled fonts dir to the engine (FontPlatformData.cpp
        // FT backend reads it via getenv) — bypasses fontconfig entirely.
        _putenv_s("APOTHEOSIS_FONTS_DIR", fontsDir.c_str());
        LogWriteF("APOTHEOSIS_FONTS_DIR=%s set", fontsDir.c_str());
        // Apotheosis: glyph-path diagnostics. stubs-other.cpp appends GlyphPage::fill /
        // Font::platformInit traces here; the engine no-ops when this is unset.
        std::string glyphLog = WideToUtf8(LocalStateDir()) + "\\glyph.log";
        _putenv_s("APOTHEOSIS_GLYPH_LOG", glyphLog.c_str());
        LogWriteF("APOTHEOSIS_GLYPH_LOG=%s set", glyphLog.c_str());
        // Apotheosis: point ICU at the package root as its data directory.
        //
        // 2026-08-21: this is now vestigial on both lines and is kept only until a run confirms it.
        // It existed because the hand-built x64 ICU 75.1 was packaged in LIBRARY mode with an
        // icudt75.dll whose data entry point was an ALL-ZERO stub, so udata_open() found nothing and
        // ubrk_open(UBRK_LINE, ...) returned NULL -- no line breaking at all. The fix was to ship the
        // real icudt75l.dat beside it and let ICU find it here (ICU appends the file name to the
        // ICU_DATA path, so it must be the DIRECTORY, not the file).
        // Both lines now take ICU 78 from vcpkg, whose icudt78.dll carries real data, and neither
        // package contains a .dat any more. Setting the variable to a directory with no ICU data in
        // it is harmless -- ICU falls back to the linked-in data -- but it is no longer load-bearing.
        // Remove it once an x64 run on ICU 78 shows line breaking working without it.
        std::string icuData = installDir;
        _putenv_s("ICU_DATA", icuData.c_str());
        LogWriteF("ICU_DATA=%s set", icuData.c_str());
        // Apotheosis: ICU break-iterator data fix. This ICU is built in LIBRARY packaging
        // mode: icuuc75.dll imports icudt75_dat from icudt75.dll, whose entry point is an
        // ALL-ZERO stub in our package. udata_open() then never falls back to the
        // icudt75l.dat file, so ubrk_open(UBRK_LINE,...) returns NULL with
        // U_MISSING_RESOURCE_ERROR and the 16-bit layout path RELEASE_ASSERTs at
        // TextBreakIteratorICU.h:68 (crash 0xc0000409). Fix: point the data directory at
        // the package root AND inject icudt75l.dat as the ICU common data, so the
        // break-iterator rules are actually found. Buffer is static -> lives for the
        // whole process (udata_setCommonData keeps the pointer, no copy).
        {
            typedef const char* (__cdecl* UErrorNameFn)(int32_t);
            typedef void (__cdecl* USetDataDirFn)(const char*);
            typedef void (__cdecl* USetCommonDataFn)(const void*, int32_t*);
            typedef void* (__cdecl* UBrkOpenFn)(int32_t type, const char* locale, const char16_t* text, int32_t len, int32_t* status);
            typedef void (__cdecl* UBrkCloseFn)(void*);
            // Apotheosis: LoadPackagedLibrary is the App-Container loader, and the only one available
            // here. SDK 19041 keeps GetModuleHandleA and LoadLibraryExA outside WINAPI_PARTITION_APP,
            // and the ARM line must build against 19041 (26100 ships no ARM32 libraries), so the pair
            // this replaces did not compile for ARM at all. It resolves by leaf name inside the
            // package, which is exactly where icuuc75.dll sits, so installDir is no longer needed
            // for it. Calling it on an already-loaded module returns the same handle with the
            // refcount bumped, which covers the GetModuleHandle half of the old code; nothing frees
            // the handle, by design -- ICU stays for the life of the process.
            // Apotheosis: the ICU version is discovered, not hardcoded.
            //
            // This used to be a literal 75 in five places -- icuuc75.dll plus u_errorName_75,
            // u_setDataDirectory_75, udata_setCommonData_75, ubrk_open_75 and ubrk_close_75. That is
            // correct for the x64 line, whose ICU comes from C:\icu-x64-uwp and is version 75, but the
            // ARM line takes ICU from vcpkg's arm-uwp triplet, where it is version 78. On the Lumia
            // LoadPackagedLibrary(L"icuuc75.dll") therefore returned null and this whole block did
            // nothing -- silently, because every log line sat inside `if (hIcu)`. The device log showed
            // only "ICU_DATA=... set", with neither the data injection nor the break-iterator
            // verification, so there was no way to tell whether text segmentation worked at all. That
            // blind spot is exactly what makes a hard-to-read crash unreadable.
            //
            // As of 2026-08-21 both lines take ICU 78 from vcpkg, so the sweep finds 78 on either.
            // It stays anyway, and deliberately: it costs one failed LoadPackagedLibrary per absent
            // version at startup and it is what turns "text segmentation silently does nothing" into
            // a log line naming the version actually present. The previous comment here said keeping
            // the lines on different ICU versions was deliberate; that is no longer true, but the
            // robustness it bought is worth keeping regardless of whether the versions agree.
            // The descending sweep prefers the newest present.
            HMODULE hIcu = nullptr;
            int icuVer = 0;
            for (int v = 80; v >= 70 && !hIcu; --v) {
                wchar_t dllName[32];
                swprintf_s(dllName, L"icuuc%d.dll", v);
                hIcu = LoadPackagedLibrary(dllName, 0);
                if (hIcu)
                    icuVer = v;
            }
            if (!hIcu)
                hIcu = LoadPackagedLibrary(L"icuuc.dll", 0);   // unversioned export layout
            if (!hIcu)
                LogWrite("ICU: no icuuc DLL in the package - data injection and break-iterator verification SKIPPED");
            if (hIcu) {
                if (icuVer)
                    LogWriteF("ICU: loaded icuuc%d.dll", icuVer);
                else
                    LogWrite("ICU: loaded icuuc.dll (unversioned exports)");
                // ICU decorates its exports with the major version. Try the decorated name first, then
                // the plain one, so both packaging styles work.
                auto sym = [hIcu, icuVer](const char* base) -> FARPROC {
                    if (icuVer) {
                        char decorated[96];
                        sprintf_s(decorated, "%s_%d", base, icuVer);
                        if (FARPROC p = GetProcAddress(hIcu, decorated))
                            return p;
                    }
                    return GetProcAddress(hIcu, base);
                };
                auto pErr = (UErrorNameFn)sym("u_errorName");
                auto pSetDataDir = (USetDataDirFn)sym("u_setDataDirectory");
                auto pSetCommonData = (USetCommonDataFn)sym("udata_setCommonData");
                auto pOpen = (UBrkOpenFn)sym("ubrk_open");
                auto pClose = (UBrkCloseFn)sym("ubrk_close");
                LogWriteF("ICU: symbols err=%p setDataDir=%p setCommonData=%p brkOpen=%p brkClose=%p",
                    (void*)pErr, (void*)pSetDataDir, (void*)pSetCommonData, (void*)pOpen, (void*)pClose);
                if (pSetDataDir)
                    pSetDataDir(icuData.c_str());
                static std::vector<unsigned char> s_icuDataBuf;
                if (pSetCommonData && s_icuDataBuf.empty()) {
                    // The .dat name carries the same version, and its presence depends on how ICU was
                    // packaged: the x64 line ships icudt75l.dat as a file, while vcpkg's arm-uwp ICU
                    // ships its data as icudt78.dll instead. When the file is absent there is nothing
                    // to inject and ICU finds its data through the data DLL, which is fine -- but say
                    // so, because "no log line" used to be indistinguishable from "never ran".
                    char datName[32];
                    sprintf_s(datName, "icudt%dl.dat", icuVer ? icuVer : 75);
                    std::string datPath = icuData + "\\" + datName;
                    std::ifstream df(datPath, std::ios::binary | std::ios::ate);
                    if (df) {
                        auto n = df.tellg();
                        s_icuDataBuf.resize((size_t)n);
                        df.seekg(0);
                        df.read((char*)s_icuDataBuf.data(), n);
                        int32_t st = 0;
                        pSetCommonData(s_icuDataBuf.data(), &st);
                        LogWriteF("ICU data: udata_setCommonData(%s %lld bytes) status=%d%s",
                            datName, (long long)n, st, pErr ? pErr(st) : "");
                    } else
                        LogWriteF("ICU data: %s not packaged - relying on the data DLL instead", datName);
                }
                // Verify the break iterators now work (was the crash path).
                if (pOpen && pClose && pErr) {
                    const char16_t euro[4] = { 'A', 0x20AC, 'B', 0 };
                    int32_t st = 0;
                    void* it = pOpen(2 /*UBRK_LINE*/, "", euro, 3, &st);
                    LogWriteF("ICU data: verify ubrk_open(UBRK_LINE) it=%p status=%d(%s)", it, st, pErr(st));
                    if (it) pClose(it);
                } else
                    LogWrite("ICU data: cannot verify break iterators - ubrk_open/ubrk_close/u_errorName did not resolve");
            }
        }
        // Check if font files exist
        std::vector<std::string> fontFiles = { "segoeui.ttf", "arial.ttf", "times.ttf", "cour.ttf", "simhei.ttf" };
        for (const auto& fn : fontFiles) {
            std::string fp = fontsDir + "\\" + fn;
            std::ifstream ft(fp, std::ios::binary | std::ios::ate);
            if (ft) LogWriteF("  font OK: %s (%lld bytes)", fn.c_str(), (long long)ft.tellg());
            else LogWriteF("  font MISSING: %s", fn.c_str());
        }
        std::ofstream conf(confPath, std::ios::binary | std::ios::trunc);
        if (conf) {
            LogWrite("  fontconfig.conf written");
            conf << "<?xml version=\"1.0\"?>\n<fontconfig>\n";
            conf << "  <dir>" << fontsDir << "</dir>\n";
            conf << "  <cachedir>" << cacheDir << "</cachedir>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>sans-serif</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Segoe UI</string><string>Arial</string><string>SimHei</string></edit></match>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>serif</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Times New Roman</string><string>SimHei</string></edit></match>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>monospace</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Courier New</string><string>SimHei</string></edit></match>\n";
                        // Fallback: append Segoe UI and SimHei (CJK) as weak fallbacks
            conf << "  <match target=\"pattern\"><edit name=\"family\" mode=\"append\" binding=\"weak\"><string>Segoe UI</string><string>SimHei</string></edit></match>\n";
            conf << "</fontconfig>\n";
            conf.close();
            _putenv_s("FONTCONFIG_FILE", confPath.c_str());
            LogWrite("  FONTCONFIG_FILE env set");
        }

        // CA cert: inject via blob to bypass App Container file I/O restrictions
        std::string srcCa = installDir + "\\cacert.pem";
        std::vector<uint8_t> caBytes;
        std::ifstream in(srcCa, std::ios::binary | std::ios::ate);
        if (in) {
            std::streamsize n = in.tellg();
            if (n > 0) {
                caBytes.resize((size_t)n);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(caBytes.data()), n);
                LogWriteF("  CACert loaded: %lld bytes", (long long)n);
            } else {
                LogWrite("  CACert file empty");
            }
        } else {
            LogWrite("  CACert file MISSING");
        }
        if (!caBytes.empty()) {
            WebCoreSetCACertBlob(caBytes.data(), (int)caBytes.size());
            LogWrite("  WebCoreSetCACertBlob called");
        }
    } catch (...) { LogWrite("ERROR in SetupRuntimeEnv (exception)"); }
}

// Apotheosis: run the setup exactly once, however many threads ask for it.
// This is called from two places -- the engine thread at the top of WebEngine::loop, and the UI
// thread in the MainPage constructor -- and until now both ran it, concurrently: every line it
// logs appeared twice with timestamps equal to the millisecond. Two hazards followed.
//   1. WebCoreSetCACertBlob() hands the PEM to curl via CURL_BLOB_NOCOPY, i.e. curl keeps a *raw
//      pointer* into a Vector owned by CurlContext's SSL handle. Calling it twice replaces that
//      Vector, which frees the buffer the first call had already published to curl. OpenSSL then
//      parses freed memory and the request dies with "unable to get local issuer certificate (20)"
//      -- observed on https://news.ycombinator.com, which had loaded fine hours earlier. Whether
//      it breaks is pure timing, which is why HTTPS looked like it worked intermittently.
//   2. _putenv_s() is not thread-safe; two threads writing the environment concurrently is how a
//      launch once died silently right after "font OK: cour.ttf" with no dump at all.
// std::call_once is the right primitive rather than an atomic flag: the loser *blocks* until the
// winner has finished, so both threads still see a fully initialised environment before they
// continue. It cannot deadlock -- SetupRuntimeEnvOnce never waits on the UI thread, and the engine
// thread has always run this same work at this same point.
static void SetupRuntimeEnv()
{
    static std::once_flag s_setupOnce;
    std::call_once(s_setupOnce, SetupRuntimeEnvOnce);
}

// Apotheosis: the last stage the engine thread passed, kept in memory so the UI thread can name it.
//
// stage.txt already holds this, but only the engine thread writes it, and the whole point of the
// heartbeat below is to describe the engine from a thread that is provably still alive. 64 bytes,
// always null-terminated at the last index, so a reader that catches a mid-copy can produce a short
// string but never run off the end. A torn diagnostic string is acceptable; a fault inside the probe
// is not.
static char g_lastStage[64] = "(none)";

// Apotheosis: wedge-snapshot state. The engine thread publishes its tid once it runs its first
// job (see "WE-job: engine tid=" below); the module bases are captured where the DLLs are
// preloaded; OnHeartbeat uses all three to suspend the engine thread and dump its context the
// first time a beat shows busy with no progress. Rationale: the platform kills a wedged app 4-7 s
// after the engine goes quiet, the trace only says WHICH batch member hung (wtimer: enter without
// exit), and a raw PC + stack words from inside the wedge resolve offline to the exact frame --
// measured, not guessed, and no engine rebuild needed since this lives entirely in the harness.
static volatile unsigned long g_engineTid = 0;
static uintptr_t g_baseExe = 0;
static uintptr_t g_baseJsc = 0;
static uintptr_t g_baseWebCore = 0;

// Apotheosis: thread-snapshot entry points for the sibling-thread sweep in WriteWedgeDump.
// tlhelp32.h is DESKTOP-partition-only in the 19041 SDK, so its declarations are reproduced here
// verbatim (same ABI); whether kernel32 on this mobile build still exports them is answered at
// link time, and the sweep degrades gracefully ("snapshot failed" line) if it does not.
extern "C" {
__declspec(dllimport) void* __stdcall CreateToolhelp32Snapshot(unsigned long flags, unsigned int pid);
__declspec(dllimport) int __stdcall Thread32First(void* snapshot, void* entry);
__declspec(dllimport) int __stdcall Thread32Next(void* snapshot, void* entry);
}
#pragma pack(push, 8)
struct ApoThreadEntry {
    unsigned long dwSize;
    unsigned long cntUsage;
    unsigned long th32ThreadID;
    unsigned long th32OwnerProcessID;
    long tpBasePri;
    long tpDeltaPri;
    unsigned long dwFlags;
};
#pragma pack(pop)
static const unsigned long APO_TH32CS_SNAPTHREAD = 0x4;

// Apotheosis: stack copy under an SEH guard, in its own function with no C++ objects -- the
// compiler forbids mixing __try with unwinding scopes (C2712), and a bad SP must cost a zero
// return, not a UI-thread crash. Same-process memcpy is fine once the writer thread is suspended.
//
// 2026-09-03: the element type is now pointer-sized, not uint32_t. On x64 the stack holds 8-byte
// return addresses and slots, so reading it as 32-bit words split every address into two halves and
// made the annotated scan report nothing at all -- while looking like it had scanned successfully.
static unsigned CopyStackWords(uintptr_t* dst, uintptr_t sp, size_t bytes)
{
    __try {
        memcpy(dst, (const void*)sp, bytes);
        return (unsigned)(bytes / sizeof(uintptr_t));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Apotheosis: read a module's name straight out of its mapped PE header. The first wedgedump
// proved the point of doing this: the engine was parked inside a SYSTEM module (PC/LR both in
// one 0x77C00000-range image) and none of our three hardcoded bases appeared anywhere in the
// scanned stack window, so the interesting frames belong to images this code knows nothing about.
// Walking the address space with VirtualQuery catches every loaded image, and the export
// directory's Name field turns each base back into a filename without any PSAPI (which is partly
// outside the UWP APP partition on this SDK).
#include "MappedPeName.h"

static bool ApoReadModuleName(uintptr_t base, size_t mappedSize, char* out, size_t cb)
{
    // Apotheosis: PE32+ uses directory offset 112, not PE32's 96. Keep SEH for
    // inaccessible image pages; malformed RVAs are rejected before dereferencing.
    __try {
        return ApoMappedPe::ReadName(reinterpret_cast<const void*>(base), mappedSize, out, cb);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (out && cb) out[0] = 0;
        return false;
    }
}

struct ApoImage { uintptr_t base; uintptr_t end; char name[48]; };
static ApoImage g_apoImages[96];
static unsigned g_apoImageCount = 0;

// One address-space walk per dump. MEM_IMAGE allocation bases are module bases; the total image
// span is the accumulation of every committed sub-region sharing that AllocationBase.
static void ApoBuildImageList()
{
    g_apoImageCount = 0;
    uintptr_t a = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    int cur = -1;
    // Apotheosis 2026-09-03: the ceiling must cover the WHOLE user address space, not 2 GB.
    //
    // This loop stopped at 0x80000000, which is the top of a 32-bit user space and is correct for
    // ARM32. On x64 the DLLs live near 0x00007FFD_xxxxxxxx, so the walk ended before reaching a single
    // module: g_apoImageCount stayed 0, ApoFindImage always returned nullptr, and every frame fell
    // through to the base-plus-window heuristic below. That heuristic then misattributed
    // JavaScriptCore frames to WebCore, and a whole diagnosis was built on the result before the
    // arithmetic gave it away (an offset larger than WebCore's own image size).
    //
    // VirtualQuery walks free regions in single hops, so raising the ceiling costs almost nothing:
    // one call per region, and the empty span between the heap and the DLLs is a handful of regions.
    const uintptr_t kUserSpaceEnd = (sizeof(uintptr_t) > 4) ? (uintptr_t)0x7FFFFFFF0000ull
                                                            : (uintptr_t)0x80000000ul;
    while (a < kUserSpaceEnd && g_apoImageCount < 95) {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE) {
            if (cur < 0 || g_apoImages[cur].base != (uintptr_t)mbi.AllocationBase) {
                cur = g_apoImageCount++;
                g_apoImages[cur].base = (uintptr_t)mbi.AllocationBase;
                g_apoImages[cur].end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                g_apoImages[cur].name[0] = 0;
            } else {
                g_apoImages[cur].end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            }
        } else {
            cur = -1;
        }
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    for (unsigned i = 0; i < g_apoImageCount; ++i)
        ApoReadModuleName(g_apoImages[i].base, g_apoImages[i].end - g_apoImages[i].base,
            g_apoImages[i].name, sizeof g_apoImages[i].name);
}

static const ApoImage* ApoFindImage(uintptr_t w)
{
    for (unsigned i = 0; i < g_apoImageCount; ++i)
        if (w >= g_apoImages[i].base && w < g_apoImages[i].end)
            return &g_apoImages[i];
    return nullptr;
}

// Apotheosis 2026-09-03: one address formatter, used by every diagnostic below.
//
// Two defects it exists to prevent, both of which cost a wrong diagnosis on 2026-09-03:
//
//  * TRUNCATION. Every site used to print `(unsigned)a`, which on x64 drops the high word: WebCore at
//    0x7FFDE85F0000 and JavaScriptCore at 0x7FFDEB2B0000 both render as 0xE8xxxxxx / 0xEBxxxxxx and
//    become indistinguishable from each other and from a 32-bit ARM address. A frame in JSC was read
//    as "WebCore+3900163" and symbolised against the wrong DLL.
//  * THE FALLBACK WINDOW. When ApoFindImage failed, the old code guessed a module by testing
//    `a - base < 0x4000000` (64 MB). WebCore's x64 image is 0x2CB2000 (44.7 MB), so 19 MB of whatever
//    is mapped after it was reported as WebCore, at offsets beyond the end of the module. The
//    arithmetic is what eventually exposed it: an offset larger than the image cannot be in the image.
//
// The fallback is now bounded by the REAL image size, read from the PE header of the module that is
// already loaded in this process. An address that matches nothing is printed bare -- unhelpful but
// honest, and far better than a confident attribution to the wrong module.
static uintptr_t ApoImageSize(uintptr_t base)
{
    if (!base)
        return 0;
    __try {
        LONG eLfanew = *(LONG*)(base + 0x3C);
        uintptr_t nt = base + (uintptr_t)eLfanew;
        // IMAGE_OPTIONAL_HEADER::SizeOfImage sits at +56 in both PE32 and PE32+.
        return (uintptr_t)*(DWORD*)(nt + 24 + 56);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void ApoFormatAddr(char* out, size_t cb, uintptr_t a)
{
    const ApoImage* img = ApoFindImage(a);
    if (img && img->name[0]) {
        sprintf_s(out, cb, "%p [%s+%llx]", (void*)a, img->name, (unsigned long long)(a - img->base));
        return;
    }
    struct { uintptr_t base; const char* name; } known[] = {
        { g_baseWebCore, "WebCore" }, { g_baseJsc, "JSC" }, { g_baseExe, "Harness" },
    };
    for (const auto& k : known) {
        if (!k.base || a < k.base)
            continue;
        const uintptr_t size = ApoImageSize(k.base);
        if (size && a - k.base < size) {
            sprintf_s(out, cb, "%p [%s+%llx]", (void*)a, k.name, (unsigned long long)(a - k.base));
            return;
        }
    }
    sprintf_s(out, cb, "%p", (void*)a);
}

static void AppendSym(std::string& out, const char* what, uintptr_t a)
{
    char addr[96];
    ApoFormatAddr(addr, sizeof addr, a);
    char l[128];
    sprintf_s(l, sizeof l, "%s=%s\n", what, addr);
    out += l;
}

// Apotheosis: REAL stack unwind for the wedge snapshot, via the NT runtime-function tables
// (.pdata) that every module on ARM32 carries. Raw stack-word scans repeatedly produced
// plausible-looking but wrong chains -- live frames interleaved with stale words from earlier
// calls on the same stack (libcurl leftovers from nav-load sat under font frames under JS
// frames), and each reading sent the investigation down a different garden path.
//
// RtlVirtualUnwind walks the ACTUAL frame chain: start from the suspended thread's context,
// resolve each PC through RtlLookupFunctionEntry (declared in winnt.h for ARM32), unwind one
// frame, repeat. Frames without unwind info fall back to a single LR step.
// Apotheosis 2026-08-25: architecture accessors, added because this walker was written for ARM32 only
// and broke the x64 line outright. `Pc` and `Lr` are ARM CONTEXT members; x64 has `Rip` and no link
// register at all, so the x64 build failed with
//     error C2039: 'Pc': is not a member of '_CONTEXT'
//     error C2039: 'Lr': is not a member of '_CONTEXT'
//     error C2664: RtlLookupFunctionEntry: cannot convert argument 1
// The last one is a width problem, not a signature one: x64 takes the same three arguments but
// DWORD64 addresses, so a ULONG_PTR-typed pc satisfies both targets.
//
// The walker is kept working on x64 rather than compiled out. It costs a few macros, both targets then
// exercise the same code, and a diagnostic that only builds on one architecture is how this divergence
// appeared in the first place. Note x64 has no LR fallback: a frame with no unwind info simply ends the
// walk there, which is honest rather than a guessed extra frame.
#if defined(_M_ARM)
#define APO_CTX_PC(c) ((c)->Pc)
#define APO_CTX_SP(c) ((c)->Sp)
#define APO_HAS_LR 1
#define APO_CTX_LR(c) ((c)->Lr)
#elif defined(_M_AMD64) || defined(_M_X64)
#define APO_CTX_PC(c) ((c)->Rip)
#define APO_CTX_SP(c) ((c)->Rsp)
#define APO_HAS_LR 0
#else
#error "ApoUnwindStack: unsupported architecture -- add CONTEXT accessors before building here"
#endif

static void ApoUnwindStack(std::string& out, void* pctx, unsigned maxFrames)
{
    auto* ctx = static_cast<CONTEXT*>(pctx);
    for (unsigned i = 0; i < maxFrames; ++i) {
        ULONG_PTR pc = (ULONG_PTR)APO_CTX_PC(ctx);
        // Apotheosis 2026-09-03: through the shared formatter. This block used to carry its own copy
        // of the truncating, 64-MB-window attribution logic -- see ApoFormatAddr for what that cost.
        char addr[96];
        ApoFormatAddr(addr, sizeof addr, (uintptr_t)pc);
        char l[128];
        sprintf_s(l, sizeof l, "  #%u %s\n", i, addr);
        out += l;

        ULONG_PTR imageBase = 0;
        PVOID handlerData = nullptr;
        ULONG_PTR establisherFrame = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        if (!entry) {
            // No unwind info: leaf or assembly stub. Step by LR once where the architecture has one --
            // guessing further would only manufacture evidence.
#if APO_HAS_LR
            ULONG_PTR lr = (ULONG_PTR)APO_CTX_LR(ctx);
            if (!lr || lr == pc) break;
            APO_CTX_PC(ctx) = (decltype(APO_CTX_PC(ctx)))lr;
            APO_CTX_SP(ctx) += 4;
            continue;
#else
            break;
#endif
        }
        RtlVirtualUnwind(0 /*UNW_FLAG_NHANDLER*/, imageBase, pc, entry,
            ctx, &handlerData, &establisherFrame, nullptr);
        if (!APO_CTX_SP(ctx)) break;
    }
}

static void WriteWedgeDump(unsigned long tid)
{
    std::string out = "wedgedump begin\n";
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) {
        char l[96];
        sprintf_s(l, sizeof l, "OpenThread(%lu) failed err=%lu\n", tid, GetLastError());
        out += l;
    } else {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(h) == (DWORD)-1) {
            out += "SuspendThread failed\n";
        } else if (!GetThreadContext(h, &ctx)) {
            out += "GetThreadContext failed\n";
            ResumeThread(h);
        } else {
            ApoBuildImageList();   // walk once per dump; the thread is suspended meanwhile
            AppendSym(out, "PC ", (uintptr_t)APO_CTX_PC(&ctx));
#if APO_HAS_LR
            AppendSym(out, "LR ", (uintptr_t)APO_CTX_LR(&ctx));
#endif
            AppendSym(out, "SP ", (uintptr_t)APO_CTX_SP(&ctx));
            // Apotheosis: low registers too. When the thread is parked in WTF's parking lot,
            // R0/R1 hold the park ADDRESS -- i.e. the identity of the very lock byte being
            // waited on -- plus the condition lambdas. That address is what turns "parked
            // somewhere" into "parked on THAT mutex" once matched against module data ranges.
            char rl[128];
#if defined(_M_ARM)
            sprintf_s(rl, sizeof rl, "R0=%08x R1=%08x R2=%08x R3=%08x\n",
                (unsigned)ctx.R0, (unsigned)ctx.R1, (unsigned)ctx.R2, (unsigned)ctx.R3);
            out += rl;
            sprintf_s(rl, sizeof rl, "R4=%08x R5=%08x R6=%08x R7=%08x\n",
                (unsigned)ctx.R4, (unsigned)ctx.R5, (unsigned)ctx.R6, (unsigned)ctx.R7);
            out += rl;
            sprintf_s(rl, sizeof rl, "R8=%08x R9=%08x R10=%08x R11=%08x R12=%08x\n",
                (unsigned)ctx.R8, (unsigned)ctx.R9, (unsigned)ctx.R10, (unsigned)ctx.R11, (unsigned)ctx.R12);
            out += rl;
#else
            // Apotheosis: the x64 equivalent. The argument registers are the ones that carry a park
            // address here (RCX/RDX first two under the Windows x64 convention), so they lead.
            sprintf_s(rl, sizeof rl, "RCX=%016llx RDX=%016llx R8=%016llx R9=%016llx\n",
                (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx,
                (unsigned long long)ctx.R8, (unsigned long long)ctx.R9);
            out += rl;
            sprintf_s(rl, sizeof rl, "RAX=%016llx RBX=%016llx RBP=%016llx RSI=%016llx RDI=%016llx\n",
                (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx, (unsigned long long)ctx.Rbp,
                (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi);
            out += rl;
#endif
            // Apotheosis: the REAL frame chain via .pdata unwind -- raw word scans kept mixing
            // live frames with stale words and sent the hunt down three different garden paths.
            out += "unwind:\n";
            ApoUnwindStack(out, &ctx, 32);
            // Stack words: copied under an SEH guard (see CopyStackWords) so a bad SP costs a
            // zero return, not a UI-thread crash. Every word is printed raw -- the first dump
            // classified only three modules and lost everything else -- with an annotated line
            // for each word that lands inside any loaded image. The engine thread is suspended.
            // 2026-09-03: pointer-sized slots and full-width printing, so an x64 return address is
            // one entry rather than two halves that match nothing.
            uintptr_t words[2048] = {};
            unsigned n = CopyStackWords(words, (uintptr_t)APO_CTX_SP(&ctx), sizeof words);
            if (n) {
                const unsigned ws = (unsigned)sizeof(uintptr_t);
                for (unsigned i = 0; i < n; ++i) {
                    uintptr_t w = words[i];
                    const ApoImage* img = ApoFindImage(w);
                    if (img) {
                        char l[128];
                        sprintf_s(l, sizeof l, "sp+%05x %p %s+%llx\n", i * ws, (void*)w,
                            img->name[0] ? img->name : "(image)", (unsigned long long)(w - img->base));
                        out += l;
                    } else if (i % 4 == 0) {
                        // raw rows for the unclassified filler, four slots per line
                        char hexline[128];
                        unsigned used = 0, cnt = 0;
                        for (unsigned j = i; j < i + 4 && j < n && !ApoFindImage(words[j]); ++j, ++cnt)
                            used += (unsigned)sprintf_s(hexline + used, sizeof hexline - used, "%p ", (void*)words[j]);
                        if (used) {
                            hexline[used - 1] = '\n';
                            out.append(hexline, used);
                        }
                        i += (cnt ? cnt - 1 : 0);
                    }
                }
                char l[64];
                sprintf_s(l, sizeof l, "stack words=%u images=%u\n", n, g_apoImageCount);
                out += l;
            } else {
                out += "stack copy failed\n";
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    out += "wedgedump end\n";
    // Apotheosis: per-process name. port-trace.txt APPENDS across launches, but a shared
    // wedgedump.txt was overwritten by whichever launch died last -- with two reproductions per
    // session the pulled dump repeatedly belonged to a different death than the trace tail being
    // read against it, and the mismatch cost a day. PID in the name makes every death's snapshot
    // survive its siblings; wedgedump.txt stays as the latest-death convenience pointer.
    wchar_t wname[64];
    swprintf_s(wname, L"wedgedump-%lu.txt", (unsigned long)GetCurrentProcessId());
    WriteMarkerFile(wname, out.c_str());
    WriteMarkerFile(L"wedgedump.txt", out.c_str());

    // Apotheosis: sibling-thread sweep. The 0.1.9.54 dump put the engine thread inside
    // libcrypto's own locking (not curl's cancellable timeout zone, not WTF's parking lot),
    // which means someone else holds a global crypto/curl lock. Naming that party needs the
    // OTHER threads' states at the same instant, so walk every thread in the process except
    // this one and ours-already-dumped, suspend each briefly, capture PC/LR plus a short stack
    // word scan, resume, and append. A worker parked in the same libcrypto range convicts a
    // mutual deadlock; a worker sitting in transfer code names the lock holder.
    {
        void* snap = CreateToolhelp32Snapshot(APO_TH32CS_SNAPTHREAD, 0);
        if (!snap || snap == (void*)-1) {
            WriteMarkerFile(wname, (out + "threads snapshot failed\n").c_str());
            return;
        }
        std::string all = "threads begin\n";
        ApoThreadEntry te;
        te.dwSize = sizeof te;
        unsigned myPid = GetCurrentProcessId();
        unsigned long myTid = GetCurrentThreadId();
        if (Thread32First(snap, &te)) {
            int dumped = 0;
            do {
                if (te.th32OwnerProcessID != myPid) continue;
                if ((unsigned long)te.th32ThreadID == myTid || (unsigned long)te.th32ThreadID == tid) continue;
                if (dumped >= 12) { all += "thread cap reached\n"; break; }
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!th) continue;
                ++dumped;
                char l[96];
                sprintf_s(l, sizeof l, "-- tid=%lu pri=%ld\n", te.th32ThreadID, te.tpBasePri);
                all += l;
                CONTEXT c2;
                memset(&c2, 0, sizeof c2);
                c2.ContextFlags = CONTEXT_FULL;
                if (SuspendThread(th) == (DWORD)-1) {
                    all += "suspend failed\n";
                } else if (!GetThreadContext(th, &c2)) {
                    all += "context failed\n";
                    ResumeThread(th);
                } else {
                    AppendSym(all, "PC ", (uintptr_t)APO_CTX_PC(&c2));
#if APO_HAS_LR
                    AppendSym(all, "LR ", (uintptr_t)APO_CTX_LR(&c2));
#endif
                    uintptr_t w2[128] = {};
                    unsigned n2 = CopyStackWords(w2, (uintptr_t)APO_CTX_SP(&c2), sizeof w2);
                    int shown = 0;
                    for (unsigned i = 0; i < n2 && shown < 24; ++i) {
                        const ApoImage* img = ApoFindImage(w2[i]);
                        if (!img || !img->name[0]) continue;
                        char l2[128];
                        sprintf_s(l2, sizeof l2, "sp+%04x %s+%llx\n", i * (unsigned)sizeof(uintptr_t),
                            img->name, (unsigned long long)(w2[i] - img->base));
                        all += l2;
                        ++shown;
                    }
                    ResumeThread(th);
                }
                CloseHandle(th);
            } while (Thread32Next(snap, &te));
        } else {
            all += "Thread32First failed\n";
        }
        CloseHandle(snap);
        all += "threads end\n";
        WriteMarkerFile(wname, (out + all).c_str());
        WriteMarkerFile(L"wedgedump.txt", (out + all).c_str());   // latest-death pointer
    }
}

static void WriteStage(const char* stage)
{
    LogWriteF("[STAGE] %s", stage);
    if (stage) {
        strncpy_s(g_lastStage, sizeof g_lastStage, stage, _TRUNCATE);
        g_lastStage[sizeof g_lastStage - 1] = '\0';
    }
    try {
        std::wstring d = LocalStateDir();
        if (d.empty()) return;
        std::ofstream f(WideToUtf8(d) + "\\stage.txt", std::ios::binary | std::ios::trunc);
        if (f) f << stage << "\n";
    } catch (...) {}
}

// ???????????????(??????),WebCoreRenderHtml ?????????CJK ?????????(SimHei)???
static const char* kHomeHtml =
    "<html><head><meta charset='utf-8'></head>"
    "<body style='margin:0;background:#f5f6f8;font-family:sans-serif;color:#202124'>"
    "<div style='background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:40px 24px'>"
    "<h1 style='margin:0;font-size:48px'>EdgeHTML Reborn</h1>"
    // Apotheosis 2026-09-18: the Russian text here and in MakeErrorHtml below was destroyed by an
    // encoding accident -- the bytes are literal '?' (0x3F) in the file, not misdecoded Cyrillic, so
    // there is nothing left to recover and the strings are rewritten rather than restored. The rest
    // of this file is genuine UTF-8 with working Cyrillic (the ru row of the string table), which is
    // why this is a handful of literals and not a file-wide problem. Doc/DESTROYED-STRINGS.md.
    "<p style='margin:8px 0 0;font-size:22px;opacity:.9'>Modern WebKit &middot; Windows 10 Mobile &middot; ARM32</p></div>"
    "<div style='padding:28px 24px'>"
    "<p style='font-size:26px;margin:0 0 18px'>Real pages, on a phone Microsoft abandoned.</p>"
    "<div style='background:#fff;border-radius:14px;padding:20px 24px;box-shadow:0 2px 8px rgba(0,0,0,.08)'>"
    "<p style='margin:0 0 10px;font-size:20px;color:#5f6368'>Capabilities</p>"
    "<p style='margin:6px 0;font-size:22px'>HTTPS &middot; TLS 1.3 &middot; JavaScript &middot; Web Storage &middot; GPU compositing</p>"
    "<p style='margin:6px 0;font-size:22px'>WebKit (WebCore) 2.52.4</p></div>"
    "<p style='margin:22px 0 0;font-size:20px;color:#80868b'>Try &nbsp;example.com &nbsp;&middot;&nbsp; github.com &nbsp;&middot;&nbsp; bing.com</p>"
    "</div></body></html>";

// Defined below BuildHomeHtml's first use of it; MakeErrorHtml needs it too.
static std::string HtmlEscape(const std::string& s);

// Apotheosis 2026-09-24: three defects in one function, all found by asking what the user can
// actually DO with this page.
//
// (1) There was no way out of it. On a failed navigation the harness sets `m_sessionActive = false`
//     and `MakeErrorHtml` contained no `<a href>` at all, so `extractLinks` (which keeps only
//     http(s) anchors) published an EMPTY link table -- `m_pageLinks` empty, `HandleTapAt` taking
//     its link-table branch with nothing in it, and every scroll/pinch handler early-returning.
//     The user got a full-screen "Could not load the page" that answered no tap and no swipe, with
//     only the address bar as an exit. On a phone that reads as "the browser died".
//     The retry anchor is deliberately the FAILED url, not the current document's: the engine is
//     still on the previous page (the error page is drawn through `WebCoreRenderHtml`, which builds
//     a throwaway Page and returns pixels -- see the driver), so retrying "where we are" would
//     reload the page that was already on screen.
// (2) `url` and `err` were interpolated raw. `err` embeds the requested url (see the port's
//     `lasterr= type=... desc=... url=...`), and a URL carrying `&` -- every query string with two
//     parameters -- was parsed as an entity reference, so the page misrendered its own explanation.
//     `HtmlEscape` already existed three functions down and is used by `BuildHomeHtml`.
// (3) The render result was never logged, so "was an error page produced, and was it put on screen"
//     could not be answered from a log. `WE-job:post-try rc=0` was the only clue, and it is written
//     for the success path too.
//
// English-only and unstyled by the string table on purpose, same as the empty-page notice: this is a
// diagnostic surface, and the three-language table is not worth the churn for it.
static std::string MakeErrorHtml(const std::string& url, const char* err)
{
    std::string e = err ? err : "";
    const std::string u = HtmlEscape(url);
    return "<html><head><meta charset='utf-8'></head>"
        "<body style='margin:0;background:#fff;font-family:sans-serif'>"
        "<div style='background:#d93025;color:#fff;padding:32px 24px'><h1 style='margin:0;font-size:38px'>Could not load the page</h1></div>"
        "<div style='padding:24px;color:#333;font-size:24px'><p style='word-break:break-all;color:#1a73e8'>" + u + "</p>"
        "<p style='color:#d93025;font-size:22px;word-break:break-all'>" + HtmlEscape(e) + "</p>"
        // Block-shaped with real padding so `boundingClientRect()` has a non-zero height -- an inline
        // anchor with no box is skipped by `extractLinks` and would be invisible to a tap.
        "<p style='margin:28px 0 0'><a href='" + u + "' style='display:inline-block;padding:16px 30px;"
        "background:#1a73e8;color:#fff;text-decoration:none;border-radius:8px;font-size:24px'>Try again</a></p>"
        "</div></body></html>";
}

// ??????:?????????????????? + ??????(?????????;LoadSettings ??? settings.ini ??????)???free ?????? NormalizeUrl/?????????,???????????????
static std::wstring g_searchPrefix = L"https://cn.bing.com/search?q=";
static std::wstring g_homeUrl = L"about:home";
static std::wstring SearchPrefixFor(int idx)
{
    switch (idx) {
        case 1: return L"https://www.google.com/search?q=";
        case 2: return L"https://duckduckgo.com/?q=";
        case 3: return L"https://www.baidu.com/s?wd=";
        default: return L"https://cn.bing.com/search?q=";
    }
}

static std::string HtmlEscape(const std::string& s)
{
    std::string out; out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}
static std::string HostOfU8(const std::string& u)
{
    size_t p = u.find("://");
    size_t s = (p == std::string::npos) ? 0 : p + 3;
    size_t e = u.find('/', s);
    return u.substr(s, (e == std::string::npos) ? std::string::npos : e - s);
}

// Build home page HTML: combine bookmarks/history as quick tiles (max 8), rendered as <a> links
static std::string BuildHomeHtml(const std::vector<Harness::Entry>& bookmarks, const std::vector<Harness::Entry>& history)
{
    std::vector<Harness::Entry> tiles;
    std::vector<std::wstring> seen;
    auto add = [&](const std::vector<Harness::Entry>& src) {
        for (const auto& e : src) {
            if (tiles.size() >= 8) break;
            if (e.url.empty() || e.url == L"about:home") continue;
            if (std::find(seen.begin(), seen.end(), e.url) != seen.end()) continue;
            seen.push_back(e.url);
            tiles.push_back(e);
        }
    };
    add(bookmarks);
    add(history);

    std::string h;
    h += "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
    h += "*{box-sizing:border-box}body{margin:0;background:#f5f6f8;font-family:sans-serif;color:#202124}";
    h += ".hero{background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:46px 26px 38px}";
    h += ".hero h1{margin:0;font-size:46px;letter-spacing:-1px}.hero p{margin:10px 0 0;font-size:20px;opacity:.92}";
    h += ".wrap{padding:24px}.sec{font-size:17px;color:#5f6368;margin:0 0 14px}";
    h += ".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}";
    h += "a.tile{display:block;text-decoration:none;background:#fff;border-radius:16px;padding:18px 18px 20px;box-shadow:0 2px 10px rgba(0,0,0,.08);color:#202124}";
    h += ".tile .t{font-size:20px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".tile .u{font-size:15px;color:#80868b;margin-top:7px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += "</style></head><body>";
    h += "<div class='hero'><h1>EdgeHTML Reborn</h1><p>\xE7\x8E\xB0\xE4\xBB\xA3\xE6\xB5\x8F\xE8\xA7\x88\xE5\x99\xA8\xE5\xBC\x95\xE6\x93\x8E &middot; Windows 10 Mobile &middot; ARM32</p></div>";
    h += "<div class='wrap'>";
    if (tiles.empty()) {
        h += "<p class='sec'>\xE5\x9C\xA8\xE4\xB8\x8A\xE6\x96\xB9\xE5\x9C\xB0\xE5\x9D\x80\xE6\xA0\x8F\xE8\xBE\x93\xE5\x85\xA5\xE7\xBD\x91\xE5\x9D\x80\xE8\xAE\xBF\xE9\x97\xAE\xE7\xBD\x91\xE9\xA1\xB5\xE3\x80\x82</p><div class='grid'>";
        const char* defs[][2] = { {"https://example.com","example.com"}, {"https://github.com","github.com"}, {"https://cn.bing.com","bing.com"}, {"https://en.wikipedia.org","wikipedia.org"} };
        for (auto& d : defs) { h += "<a class='tile' href='"; h += d[0]; h += "'><div class='t'>"; h += d[1]; h += "</div><div class='u'>"; h += d[0]; h += "</div></a>"; }
        h += "</div>";
    } else {
        h += "<p class='sec'>\xE5\xB8\xB8\xE7\x94\xA8\xE7\xAB\x99\xE7\x82\xB9</p><div class='grid'>";
        for (const auto& e : tiles) {
            std::string href = HtmlEscape(WideToUtf8(e.url));
            std::string title = HtmlEscape(WideToUtf8(e.title.empty() ? e.url : e.title));
            std::string host = HtmlEscape(HostOfU8(WideToUtf8(e.url)));
            h += "<a class='tile' href='" + href + "'><div class='t'>" + title + "</div><div class='u'>" + host + "</div></a>";
        }
        h += "</div>";
    }
    h += "</div></body></html>";
    return h;
}

static Platform::String^ NormalizeUrl(Platform::String^ raw)
{
    std::wstring s = raw ? std::wstring(raw->Data()) : L"";
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    if (s.empty())
        return ref new String(L"about:home");
    // Determine if input looks like a URL (has dot) vs search term => Bing fallback
    bool looksUrl = (s.find(L"://") != std::wstring::npos) || (s.find(L'.') != std::wstring::npos && s.find(L' ') == std::wstring::npos);
    if (s.rfind(L"about:", 0) == 0)
        return ref new String(s.c_str());
    // Apotheosis: "contains ://" is not the same as "has a scheme". A single stray character in
    // front of the address -- trivially easy on a touch keyboard, and observed for real as
    // "\x0430https://hh.ru" -- used to sail through here and reach the loader, which rejected it
    // with rc=-9 and left the user staring at a failed page with no hint why. A scheme is
    // ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) and is always ASCII (RFC 3986 3.1), so an invalid
    // one is detectable without guessing. When a real http(s) URL is hiding behind the junk, keep
    // the URL; otherwise treat the whole string as a search term, which is what desktop browsers do.
    size_t sep = s.find(L"://");
    if (sep != std::wstring::npos) {
        bool validScheme = sep > 0;
        for (size_t i = 0; i < sep && validScheme; ++i) {
            wchar_t c = s[i];
            bool alpha = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
            bool digit = (c >= L'0' && c <= L'9');
            if (i == 0) validScheme = alpha;
            else validScheme = alpha || digit || c == L'+' || c == L'-' || c == L'.';
        }
        if (!validScheme) {
            size_t https = s.find(L"https://"), http = s.find(L"http://");
            size_t keep = (https != std::wstring::npos) ? https : http;
            if (keep != std::wstring::npos && keep > 0)
                s = s.substr(keep);          // stray prefix in front of a real address
            else
                looksUrl = false;            // nothing salvageable -> search for it
        }
    }
    if (!looksUrl) {
        std::wstring q;
        for (wchar_t c : s) { if (c == L' ') q += L"%20"; else q += c; }
        return ref new String((g_searchPrefix + q).c_str());
    }
    if (s.find(L"://") == std::wstring::npos)
        s = L"https://" + s;
    return ref new String(s.c_str());
}

// ===== WebEngine: dedicated thread for WebCore/JSC, serialized job queue =====
extern "C" __declspec(dllimport) HMODULE __stdcall GetModuleHandleW(const wchar_t*);
extern "C" __declspec(dllimport) BOOL __stdcall GetModuleHandleExW(DWORD, LPCWSTR, HMODULE*);
extern "C" __declspec(dllimport) DWORD __stdcall GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
class WebEngine {
public:
    static WebEngine& instance() { static WebEngine e; return e; }
    // Apotheosis: every job carries a short label, and the loop publishes the label of the one it is
    // currently running. Added 2026-08-22.
    //
    // The heartbeat could already say `busy=1` -- the engine is inside a job -- and could name the
    // last *stage* the engine passed, but most jobs write no stage marker at all (GPU init, the diag
    // snapshot, taps, scrolls, the live tick), so a hang in any of those reported the stage of some
    // earlier job and pointed at the wrong place. It pointed at the resize path, and the log then
    // showed ApplyViewportSize had returned early and posted nothing. A label per job removes the
    // guessing: the label is a string literal, so storing the pointer is safe for the process's life.
    void post(std::function<void()> job, const char* label = "unlabelled")
    {
        { std::lock_guard<std::mutex> lk(m_mtx); m_q.emplace_back(label, std::move(job)); }
        m_cv.notify_one();
    }
    // Label-first overloads. Deliberately the form used at the call sites: the job lambdas here run
    // to dozens of lines, and putting the label after them would mean editing nineteen closing
    // `});` -- a mechanical edit with a real chance of attaching a label to the wrong brace. Label
    // first is one insertion per site and cannot slide.
    void post(const char* label, std::function<void()> job) { post(std::move(job), label); }
    void postFront(const char* label, std::function<void()> job) { postFront(std::move(job), label); }
    // Apotheosis: queue-jumping post, for navigation only. The queue is a strict FIFO drained by
    // a single thread, so ordinary posting makes a navigation wait for whatever is already queued
    // -- including a resize, which on a heavy page has been measured at 5.9 s and observed never
    // to return at all. Navigation is the one operation the user is actually waiting on, so it
    // goes to the head of the line.
    //
    // What this does NOT do, and cannot: preempt a job that has already STARTED. There is no
    // cancellation point inside WebCoreGpuResize, and the iron rule that every C ABI call runs on
    // the one engine thread rules out a second thread doing the navigation. A resize already in
    // flight is therefore waited out -- which is why queued resizes additionally self-cancel when
    // a navigation is pending (see g_navGen) and why the resize path itself is instrumented.
    void postFront(std::function<void()> job, const char* label = "unlabelled-front")
    {
        { std::lock_guard<std::mutex> lk(m_mtx); m_q.emplace_front(label, std::move(job)); }
        m_cv.notify_one();
    }
    // Apotheosis: the engine thread's own view of whether it is inside a job, as opposed to the
    // UI thread's m_loading / m_interacting beliefs -- which the load watchdog used to reset
    // without asking anyone, leaving the UI convinced the engine was idle while it was in fact
    // stuck in a resize. Read from the UI thread, hence atomic.
    bool busy() const { return m_busy.load(std::memory_order_acquire); }
    // Number of jobs that have run to completion. Two samples 40 s apart tell a genuinely wedged
    // engine (unchanged) from a merely slow one (advancing).
    unsigned long long finished() const { return m_finished.load(std::memory_order_acquire); }
    // The label of the job currently running, or the last one that ran. Read from the UI thread.
    const char* currentJob() const
    {
        const char* s = m_currentJob.load(std::memory_order_acquire);
        return s ? s : "(none)";
    }
    size_t pending() { std::lock_guard<std::mutex> lk(m_mtx); return m_q.size(); }
private:
    WebEngine() {
        LogWrite("WebEngine: thread starting");
        HANDLE h = CreateThread(nullptr, 16 * 1024 * 1024, [](LPVOID p) -> DWORD {
            static_cast<WebEngine*>(p)->loop(); return 0;
        }, this, 0, nullptr);
        if (h) CloseHandle(h);
    }
    void loop()
    {
        // Apotheosis: install unhandled exception filter + VEH to catch GS failures
        // (0xc0000409 STATUS_STACK_BUFFER_OVERRUN) and log stack trace before fast-fail.
        // The CRT invalid parameter handler is also installed to catch CRT bugs.
        _set_invalid_parameter_handler([](
            const wchar_t* expr, const wchar_t* func, const wchar_t* file,
            unsigned int line, uintptr_t) {
            LogWriteF("CRT-INVALID-PARAM: expr=%ls func=%ls file=%ls line=%u",
                expr ? expr : L"null", func ? func : L"null",
                file ? file : L"null", line);
            void* bt[32];
            USHORT frames = CaptureStackBackTrace(0, 32, bt, nullptr);
            LogWriteF("CRT-INVALID-PARAM: stack (%u frames):", frames);
            for (USHORT i = 0; i < frames; ++i)
                LogWriteF("  #%u: %p", i, bt[i]);
        });
        // Apotheosis 2026-09-18: the death net that was missing. It is NOT redundant with the UEF
        // below: the UEF only runs when nothing handled the exception, so an exception that *is*
        // caught and then followed by a separate kill is invisible to it. A first-chance handler
        // sees it. The old comment here read "VEH not available in UWP"; the installer in
        // ApoInstallDeathNet logs the loader's own error code, which is the arbiter of that claim.
        ApoInstallDeathNet();
        // Apotheosis: SetUnhandledExceptionFilter as catch-all
        SetUnhandledExceptionFilter([](PEXCEPTION_POINTERS p) -> LONG {
            LogWriteF("UEF: unhandled exception code=0x%08X at addr=%p tid=%lu",
                p->ExceptionRecord->ExceptionCode,
                p->ExceptionRecord->ExceptionAddress,
                GetCurrentThreadId());
            // Apotheosis: for an access violation the faulting *data* address is the
            // diagnostic that matters — it separates a wild/dangling pointer from a
            // deliberate WebKit CRASH() (RELEASE_ASSERT stores to a poison address),
            // which also surfaces as 0xC0000005. Without this the two are identical
            // in the log and cost a full debug cycle to tell apart.
            if (p->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                && p->ExceptionRecord->NumberParameters >= 2) {
                ULONG_PTR op = p->ExceptionRecord->ExceptionInformation[0];
                LogWriteF("UEF: AV %s address=0x%llX",
                    op == 0 ? "reading" : (op == 1 ? "writing" : "DEP-executing"),
                    (unsigned long long)p->ExceptionRecord->ExceptionInformation[1]);
            }
            // Apotheosis: module bases for the crash PC + stack. GetModuleHandleW is the one
            // UWP-safe way to map addresses to DLLs (no PEB walk, no tlhelp32 on Mobile).
            {
                static const wchar_t* const kMods[] = {
                    L"Harness.exe", L"WebCore.dll", L"JavaScriptCore.dll",
                    L"libEGL.dll", L"libGLESv2.dll", L"libcurl.dll",
                    L"libcrypto-3-arm.dll", L"libssl-3-arm.dll",
                    L"pixman-1-0.dll", L"freetype.dll", L"fontconfig-1.dll",
                    L"harfbuzz.dll", L"harfbuzz-icu.dll", L"icuuc78.dll",
                    L"icuin78.dll", L"icudt78.dll", L"jpeg62.dll",
                    L"libpng16.dll", L"libwebp.dll", L"libwebpdemux.dll",
                    L"libxml2.dll", L"z.dll", L"bz2.dll", L"brotlidec.dll",
                    L"brotlicommon.dll", L"iconv-2.dll", L"libexpat.dll",
                    L"libsharpyuv.dll", L"MSVCP140_APP.dll",
                    L"MSVCP140_2_APP.dll", L"VCRUNTIME140_APP.dll",
                    L"vccorlib140_app.dll",
                };
                LogWrite("UEF: modules:");
                for (const wchar_t* n : kMods) {
                    HMODULE m = GetModuleHandleW(n);
                    LogWriteF("UEF:   %ls = %p", n, (void*)m);
                }
            }
            void* bt[32];
            USHORT frames = CaptureStackBackTrace(0, 32, bt, nullptr);
            // Apotheosis: identify the module containing each address (PC, LR, every frame).
            // The fixed-name module table is not exhaustive, so resolve from the address itself.
            auto modOf = [](const void* addr) -> const wchar_t* {
                HMODULE h = nullptr;
                if (!GetModuleHandleExW(0x00000004 /*GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS*/,
                        (LPCWSTR)addr, &h) || !h)
                    return L"???";
                static thread_local wchar_t name[128];
                name[0] = 0;
                GetModuleFileNameW(h, name, 128);
                int last = 0;
                for (int i = 0; name[i] && i < 127; ++i)
                    if (name[i] == L'\\' || name[i] == L'/')
                        last = i + 1;
                return name + last;
            };
            LogWriteF("UEF: stack (%u frames):", frames);
            for (USHORT i = 0; i < frames; ++i)
                LogWriteF("  #%u: %p [%ls]", i, bt[i], modOf(bt[i]));
            // Apotheosis: CaptureStackBackTrace from inside the filter only walks the exception
            // dispatch chain (filter -> KERNELBASE -> ntdll), not the faulting call sites -- every
            // crash here came out as the same useless seven frames. What DOES survive is the raw
            // faulting stack below ContextRecord->Sp: dump annotated words exactly like the
            // wedge snapshot does, and the real caller chain reconstructs offline against the
            // logged module bases. (No WK_WINUWP guard here -- that macro exists only in the
            // WebKit build; the first version of this block silently compiled itself away.)
            if (p->ContextRecord && APO_CTX_SP(p->ContextRecord)) {
                // 2026-09-03: pointer-sized slots, full-width printing, and the plausibility window
                // taken from the address size rather than hardcoded to 2 GB -- on x64 every module
                // sits above 0x80000000, so the old bound rejected exactly the addresses that matter.
                uintptr_t uefWords[256] = {};
                unsigned uefN = CopyStackWords(uefWords, (uintptr_t)APO_CTX_SP(p->ContextRecord), sizeof uefWords);
                LogWriteF("UEF: raw stack words=%u:", uefN);
                const unsigned uws = (unsigned)sizeof(uintptr_t);
                const uintptr_t uefTop = (sizeof(uintptr_t) > 4) ? (uintptr_t)0x7FFFFFFF0000ull
                                                                 : (uintptr_t)0x80000000ul;
                char uefLine[128];
                for (unsigned i = 0; i < uefN; ++i) {
                    const ApoImage* img = ApoFindImage(uefWords[i]);
                    if (img && img->name[0]) {
                        sprintf_s(uefLine, sizeof uefLine, "  sp+%04x %p %s+%llx",
                            i * uws, (void*)uefWords[i], img->name,
                            (unsigned long long)(uefWords[i] - img->base));
                        LogWriteF("%s", uefLine);
                    } else if (uefWords[i] >= 0x00010000 && uefWords[i] < uefTop && i % 8 == 0) {
                        sprintf_s(uefLine, sizeof uefLine, "  sp+%04x %p", i * uws, (void*)uefWords[i]);
                        LogWriteF("%s", uefLine);
                    }
                }
            }
            if (p->ContextRecord) {
#if defined(_M_AMD64) || defined(_M_X64)
                LogWriteF("UEF: RIP=%p RSP=%p RBP=%p",
                    (void*)p->ContextRecord->Rip,
                    (void*)p->ContextRecord->Rsp,
                    (void*)p->ContextRecord->Rbp);
#elif defined(_M_ARM64)
                LogWriteF("UEF: PC=%p SP=%p LR=%p",
                    (void*)p->ContextRecord->Pc,
                    (void*)p->ContextRecord->Sp,
                    (void*)p->ContextRecord->Lr);
#elif defined(_M_ARM)
                LogWriteF("UEF: PC=%p [%ls] SP=%p LR=%p [%ls]",
                    (void*)p->ContextRecord->Pc, modOf((void*)p->ContextRecord->Pc),
                    (void*)p->ContextRecord->Sp,
                    (void*)p->ContextRecord->Lr, modOf((void*)p->ContextRecord->Lr));
                LogWriteF("UEF: R0=%08lX R1=%08lX R2=%08lX R3=%08lX",
                    p->ContextRecord->R0, p->ContextRecord->R1,
                    p->ContextRecord->R2, p->ContextRecord->R3);
                LogWriteF("UEF: R4=%08lX R5=%08lX R6=%08lX R7=%08lX",
                    p->ContextRecord->R4, p->ContextRecord->R5,
                    p->ContextRecord->R6, p->ContextRecord->R7);
                LogWriteF("UEF: R8=%08lX R9=%08lX R10=%08lX R11=%08lX R12=%08lX",
                    p->ContextRecord->R8, p->ContextRecord->R9,
                    p->ContextRecord->R10, p->ContextRecord->R11,
                    p->ContextRecord->R12);
#endif
            }
            return EXCEPTION_CONTINUE_SEARCH;
        });
        // Apotheosis 2026-09-18: std::set_terminate -- the handler this crash family was missing.
        // The signature that motivated it (dzen.ru, scroll down, build 0.1.9.103): log.txt simply
        // stops, UEF writes nothing, unhandled.txt is absent, WER leaves no dump, and the window
        // vanishes. An exception the CRT runtime cannot unwind reaches std::terminate, whose default
        // action is abort() -- and on this UCRT abort() raises a fast-fail, which bypasses
        // SetUnhandledExceptionFilter (above) and the LocalDumps registry key by design. That is why
        // all three existing channels were silent at once. The terminate handler runs BEFORE the
        // fast-fail, on the dying thread, so it is the last place that can still speak.
        // CaptureStackBackTrace from inside it walks handler -> terminate -> the throw site, which is
        // the chain worth having; the raw stack below it is not, because the frame that threw is
        // still intact at this point.
        std::set_terminate([] {
            const unsigned long tid = GetCurrentThreadId();
            LogWriteF("TERMINATE: tid=%lu engine=%d", tid,
                (tid == (unsigned long)g_engineTid) ? 1 : 0);
            ApoBuildImageList();
            void* bt[64];
            const USHORT n = CaptureStackBackTrace(0, 64, bt, nullptr);
            LogWriteF("TERMINATE: stack (%u frames):", n);
            for (USHORT i = 0; i < n; ++i) {
                char a[128];
                ApoFormatAddr(a, sizeof a, (uintptr_t)bt[i]);
                LogWriteF("  TERM #%u %s", i, a);
            }
            abort();
        });
        // Apotheosis: early diagnostic — write a marker file before anything else
        try {
            auto folder = Windows::Storage::ApplicationData::Current->LocalFolder;
            auto file = concurrency::create_task(
                folder->CreateFileAsync(L"loop-started.txt",
                    Windows::Storage::CreationCollisionOption::ReplaceExisting)).get();
            concurrency::create_task(
                Windows::Storage::FileIO::WriteTextAsync(file, L"loop() entered\n")).get();
        } catch (...) {}
        LogWrite("WebEngine: loop started - calling SetupRuntimeEnv");
        SetupRuntimeEnv();   // SetupRuntimeEnv early: fontconfig + CA cert before WebCore call
        LogWrite("WebEngine: loop ready, waiting for jobs");
        for (;;) {
            std::pair<const char*, std::function<void()>> item;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] { return !m_q.empty(); });
                item = std::move(m_q.front());
                m_q.pop_front();
            }
            m_currentJob.store(item.first, std::memory_order_release);
            m_busy.store(true, std::memory_order_release);
            try { item.second(); } catch (...) {}
            m_busy.store(false, std::memory_order_release);
            m_finished.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::pair<const char*, std::function<void()>>> m_q;
    std::atomic<bool> m_busy { false };
    std::atomic<const char*> m_currentJob { nullptr };
    std::atomic<unsigned long long> m_finished { 0 };
};

// GPU direct present: swapBuffers renders to GpuPanel, skip blit to WriteableBitmap
//   ????????? BlitToBitmap ????????????,???????????? 3MB ??? RGBA???BGRA ??????(??? 60fps)?????????????????? UI ??????????????????,??? atomic???
static std::atomic<bool> g_directPresent { false };

// Apotheosis: generation counters for the "navigation outranks resizing" rule. Written on the UI
// thread, read on the engine thread, hence atomic.
//
// g_navGen is bumped by NavigateTo the moment a navigation is requested. A resize job captures it
// at post time and re-checks on entry: a mismatch means a navigation arrived after this resize was
// queued, so the resize skips its engine call altogether and gets out of the way. Nothing is lost
// by skipping -- the load lays the page out at the pending size anyway. That a resize is
// idempotent and discardable while a navigation is neither is precisely what makes the resize the
// safe one to drop.
//
// g_resizeGen does the same job for a superseded resize: when several sizes were queued, only the
// newest is worth paying a relayout plus a full backing-store regeneration for.
static std::atomic<unsigned> g_navGen { 0 };
static std::atomic<unsigned> g_resizeGen { 0 };

// ===== BlitToBitmap: copy RGBA to WriteableBitmap (RGBA->BGRA) =====
static void BlitToBitmap(WriteableBitmap^ wb, const std::vector<uint8_t>& rgba, int W, int H)
{
    if (g_directPresent.load()) return;   // ?????????:???????????? blit(GpuPanel ??????????????????)
    // Apotheosis: kW/kH can change under a resize, so a frame produced at the previous size may
    // arrive here after the size has already moved on. Every caller allocates exactly W*H*4, so
    // an inexact size means a stale frame: drop it rather than blit it at the wrong stride (too
    // small would read past the end, too large would come out skewed). The next paint is correct.
    if (W <= 0 || H <= 0 || rgba.size() != (size_t)W * H * 4) return;
    ComPtr<Windows::Storage::Streams::IBufferByteAccess> bba;
    reinterpret_cast<IInspectable*>(wb->PixelBuffer)->QueryInterface(IID_PPV_ARGS(&bba));
    byte* dst = nullptr;
    bba->Buffer(&dst);
    const size_t n = (size_t)W * H;
    const uint8_t* src = rgba.data();
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
}

// WriteBmp32: saves RGBA buffer as 32-bit BMP (BGRA) for GPU readback dump,
// retrieved via WDP file browser (no UI; PNG would be slower)
//
// Apotheosis 2026-09-19: unlike BlitToBitmap this takes a RAW POINTER, so it cannot check that the
// buffer is w*h*4 -- it starts at `rgba + (h-1)*w*4` and a caller that re-reads kW/kH after the
// buffer was allocated at the old size walks roughly (h*w - alloc/4) pixels past the end. That is not
// hypothetical: it cost the first autodiag run on x64 (VEH 0xC0000005 reading 0x82183FF4E2,
// WriteBmp32:1635 <- autodiag lambda:2407 <- WebEngine::loop). Callers must pass the size the buffer
// was ALLOCATED at -- snapshot it, do not re-read the globals.
static void WriteBmp32(const std::string& path, const uint8_t* rgba, int w, int h)
{
    if (rgba == nullptr || w <= 0 || h <= 0) return;
    const uint32_t dataSize = (uint32_t)w * h * 4;
    uint8_t fh[54] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    auto put32 = [&](int off, uint32_t v) { memcpy(fh + off, &v, 4); };
    auto put16 = [&](int off, uint16_t v) { memcpy(fh + off, &v, 2); };
    put32(2, 54 + dataSize); put32(10, 54);
    put32(14, 40); put32(18, (uint32_t)w); put32(22, (uint32_t)h);   // ??????=????????????
    put16(26, 1); put16(28, 32); put32(30, 0); put32(34, dataSize);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write((char*)fh, 54);
    std::vector<uint8_t> row((size_t)w * 4);
    for (int fy = 0; fy < h; ++fy) {
        const uint8_t* src = rgba + (size_t)(h - 1 - fy) * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2]; // B
            row[x * 4 + 1] = src[x * 4 + 1]; // G
            row[x * 4 + 2] = src[x * 4 + 0]; // R
            row[x * 4 + 3] = src[x * 4 + 3]; // A
        }
        f.write((char*)row.data(), (std::streamsize)w * 4);
    }
}

// Apotheosis: the engine render surface size, in engine pixels.
//
// This used to be a hardcoded 720x1080 phone viewport, so on a desktop-sized window the page
// laid out in a 720px column and the rest of ContentArea stayed empty grey. It now tracks
// ContentArea's actual size (see MainPage::ApplyViewportSize); 720x1080 is only the value used
// before the first layout pass, and stays the value on a real Lumia 950 screen.
//
// Written on the UI thread, read on both threads. A stale read can only mismatch a buffer that
// was allocated a frame earlier, and BlitToBitmap rejects a buffer that is too small for the
// size it is asked to blit, so the worst case is one dropped frame while the window is resizing.
static int kW = 720, kH = 1080;

// ===== UI string constants (i18n tables) =====
// 0=Chinese (zh-CN), 1=English, 2=Russian (ru-RU)
enum UIString {
    S_BACK, S_FORWARD, S_REFRESH, S_BOOKMARK, S_BOOKMARKED,
    S_NEW_TAB, S_HOME, S_DESKTOP_SITE, S_MOBILE_SITE, S_FIND_IN_PAGE,
    S_SHARE, S_COPY_LINK, S_DOWNLOAD_PAGE, S_BOOKMARKS, S_HISTORY,
    S_DOWNLOADS, S_SETTINGS, S_MENU, S_MENU_TITLE,
    S_SEARCH_PLACEHOLDER, S_LOCK_SECURE, S_LOCK_INSECURE,
    S_LOADING, S_PROCESSING, S_LOAD_FAILED, S_LOAD_TIMEOUT,
    S_EMPTY_FAV, S_EMPTY_HIST, S_EMPTY_DL, S_NO_SHARE,
    S_SETTINGS_TITLE, S_SET_LANG, S_SET_LANG_ZH, S_SET_LANG_EN, S_SET_LANG_RU,
    S_SET_SEARCH, S_SET_HOME, S_SET_HOME_PLACEHOLDER,
    S_SET_UA, S_SET_UA_CUSTOM, S_SET_UA_CUSTOM_PLACEHOLDER,
    S_SET_ZOOM, S_SET_GPU, S_SET_GPU_NOW,
    S_SET_CLEAR_DATA, S_SET_CLEAR_HIST, S_SET_CLEAR_FAV, S_SET_CLEAR_DL,
    S_SET_DIAG, S_SET_EXPORT, S_SET_ABOUT, S_SET_CHECK_UPDATE,
    S_TAB_SWITCHER_TITLE, S_TAB_SWITCHER_DONE, S_NEW_TAB_BTN,
    S_FAV_TAB, S_HIST_TAB, S_DL_TAB, S_ACTION_ADD_FAV, S_ACTION_DEL_FAV,
    S_ACTION_CLEAR, S_ACTION_DL_PAGE, S_DL_STARTED, S_DL_DONE, S_DL_FAILED,
    S_COPIED, S_CANCELLED, S_FOUND_N, S_FOUND_NONE, S_FOUND_NO_MORE,
    // Apotheosis: S_CANCEL_FAV and S_DEL_FAV are two slots for the same drawer button -- all three
    // .resw files give both the identical text. kStr held only one string for the pair, so from here
    // on every row was shifted by one: S_HOME_TITLE rendered "GPU on", S_GPU_ON rendered "GPU
    // failed", and the last slot read past the end of the row into a nullptr. Both slots are filled
    // below. S_CANCEL_FAV is itself unreferenced; only S_DEL_FAV is used (RebuildDrawerList).
    S_FIND_PLACEHOLDER, S_CANCEL_FAV, S_DEL_FAV, S_AUTODIAG_DONE,
    S_HOME_TITLE, S_GPU_ON, S_GPU_FAIL, S_UPDATE_CHECKING,
    S_UPDATE_FAIL, S_UPDATE_LATEST, S_UPDATE_FOUND, S_UPDATE_DLG_TITLE,
    S_UPDATE_DLG_GO, S_UPDATE_DLG_LATER, S_EXPORT_CHOOSE, S_EXPORT_FAIL,
    S_TOAST_BOOKMARKED, S_TOAST_UNBOOKMARKED, S_TOAST_HIST_CLEARED,
    S_TOAST_FAV_CLEARED, S_TOAST_DL_CLEARED, S_TOAST_CANNOT_FIND,
    // Apotheosis: appended at the end on purpose -- kStr below is a positional table, so inserting
    // anywhere else would silently shift every string after it.
    S_ENGINE_BUSY,
    S_COUNT
};

static const wchar_t* kStr[3][S_COUNT] = {
// 0: Chinese (zh-CN)
    {
        L"后退", L"前进", L"刷新", L"书签", L"已收藏",
        L"新标签页", L"主页", L"桌面版网站", L"手机版网站", L"在页面中查找",
        L"分享", L"复制链接", L"下载页面", L"书签", L"历史记录",
        L"下载", L"设置", L"菜单", L"菜单",
        L"搜索或输入网址", L"", L"",
        L"加载中", L"处理中...", L"加载失败", L"加载超时",
        L"暂无书签", L"暂无历史", L"暂无下载", L"无内容可分享",
        L"设置", L"语言", L"中文", L"英文", L"俄文",
        L"默认搜索引擎", L"主页（留空使用默认）", L"about:home",
        L"启动时请求桌面版网站", L"自定义 User-Agent（留空使用开关；重新加载生效）", L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) ...",
        L"默认缩放", L"启动时启用 GPU（首个页面加载后自动开启）", L"立即启用 GPU（重启恢复）",
        L"清除数据", L"清除历史", L"清除所有书签", L"清除下载记录",
        L"诊断", L"导出调试日志 / 崩溃转储", L"关于 / 更新", L"检查更新（GitHub Releases）",
        L"标签页", L"完成", L"新建标签页",
        L"收藏夹", L"历史", L"下载", L"收藏此页", L"取消收藏",
        L"清除", L"下载此页面", L"下载中", L"完成", L"失败：",
        L"链接已复制", L"已取消", L" 个匹配", L"无结果", L"没有更多",
        L"在页面中查找", L"删除书签", L"删除书签", L"自动诊断完成",
        L"主页", L"GPU 已开启", L"GPU 失败", L"正在检查更新...",
        L"检查更新失败", L"已是最新版本 ", L"发现新版本 ",
        L"发现更新", L"前往下载", L"稍后",
        L"选择导出位置...", L"导出失败",
        L"已添加书签", L"已移除书签", L"历史已清除", L"书签已清除", L"下载已清除", L"页面中未找到内容",
        L"引擎正忙",
    },

// 1: English (en-US)
//
// Apotheosis: this row went missing at some point and nobody noticed, because losing a row in a
// brace-elided 2D array is silent: the Russian block below simply slid up into index 1 while
// keeping its `// 2:` label, and index 2 became all-nullptr. Symptom on a machine with lang=1 in
// settings.ini: an English UI showing "Обработка..." in the status line -- and picking Russian in
// Settings would have handed std::wstring a nullptr. Keep all three rows, in enum order, and see
// the null guard in GetStr below.
    {
        L"Back", L"Forward", L"Reload", L"Bookmark", L"Bookmarked",
        L"New tab", L"Home", L"Desktop site", L"Mobile site", L"Find in page",
        L"Share", L"Copy link", L"Download page", L"Bookmarks", L"History",
        L"Downloads", L"Settings", L"Menu", L"Menu",
        L"Search or enter address", L"", L"",
        L"Loading", L"Working...", L"Load failed", L"Load timed out",
        L"No bookmarks", L"No history", L"No downloads", L"Nothing to share",
        L"Settings", L"Language", L"Chinese", L"English", L"Russian",
        L"Search engine", L"Home page (leave empty for about:home)", L"about:home",
        L"Request desktop site at startup", L"Custom User-Agent (empty = use the switch; reload to apply)", L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) ...",
        L"Default zoom", L"Enable GPU at startup (automatic after the first page)", L"Enable GPU now (restart to revert)",
        L"Clear data", L"Clear history", L"Clear all bookmarks", L"Clear downloads",
        L"Diagnostics", L"Export debug logs / crash dumps", L"About / Update", L"Check for updates (GitHub Releases)",
        L"Tabs", L"Done", L"New tab",
        L"Favorites", L"History", L"Downloads", L"Bookmark this page", L"Remove bookmark",
        L"Clear", L"Download this page", L"Downloading", L"Done", L"Failed: ",
        L"Link copied", L"Cancelled", L" matches", L"No results", L"No more results",
        L"Find in page", L"Delete bookmark", L"Delete bookmark", L"AUTODIAG DONE",
        L"Home", L"GPU on", L"GPU failed", L"Checking for updates...",
        L"Update check failed", L"Already up to date ", L"New version ",
        L"Update available", L"Go to download", L"Later",
        L"Choose an export folder...", L"Export failed",
        L"Bookmark added", L"Bookmark removed", L"History cleared", L"Bookmarks cleared", L"Downloads cleared", L"Not found on this page",
        L"Engine busy",
    },

// 2: Russian (ru-RU)
    {
        L"Назад", L"Вперёд", L"Обновить", L"Закладка", L"В закладках",
        L"Новая вкладка", L"Домой", L"Версия для ПК", L"Версия для телефона", L"Найти на странице",
        L"Поделиться", L"Копировать ссылку", L"Скачать страницу", L"Закладки", L"История",
        L"Загрузки", L"Настройки", L"Меню", L"Меню",
        L"Поиск или введите URL", L"", L"",
        L"Загрузка", L"Обработка...", L"Ошибка загрузки", L"Таймаут загрузки",
        L"Нет закладок", L"Нет истории", L"Нет загрузок", L"Нечем поделиться",
        L"Настройки", L"Язык", L"Китайский", L"Английский", L"Русский",
        L"Поисковая система", L"Домашняя страница (оставьте пустым для about:home)", L"about:home",
        L"Запрашивать ПК-версию при запуске", L"Свой User-Agent (пусто = использовать переключатель; перезагрузите для применения)", L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) ...",
        L"Масштаб по умолчанию", L"Включить GPU при запуске (авто после первой страницы)", L"Включить GPU сейчас (перезапуск для отката)",
        L"Очистить данные", L"Очистить историю", L"Очистить закладки", L"Очистить загрузки",
        L"Диагностика", L"Экспорт логов / дампов", L"О программе / Обновление", L"Проверить обновления (GitHub Releases)",
        L"Вкладки", L"Готово", L"Новая вкладка",
        L"Избранное", L"История", L"Загрузки", L"Добавить в закладки", L"Удалить из закладок",
        L"Очистить", L"Скачать страницу", L"Скачивание", L"Готово", L"Ошибка: ",
        L"Ссылка скопирована", L"Отменено", L" совпадений", L"Нет результатов", L"Больше нет",
        L"Найти на странице", L"Удалить закладку", L"Удалить закладку", L"АВТОДИАГНОСТИКА ГОТОВА",
        L"Домой", L"GPU Вкл", L"GPU Ошибка", L"Проверка обновлений...",
        L"Ошибка проверки", L"Уже актуально ", L"Новая версия ",
        L"Доступно обновление", L"Перейти к загрузке", L"Позже",
        L"Выберите папку для экспорта...", L"Ошибка экспорта",
        L"Добавлено в закладки", L"Удалено из закладок", L"История очищена", L"Закладки очищены", L"Загрузки очищены", L"Ничего не найдено на странице",
        L"Движок занят",
    },
};

static const wchar_t* kResNames[] = {
    L"S_BACK", L"S_FORWARD", L"S_REFRESH", L"S_BOOKMARK", L"S_BOOKMARKED",
    L"S_NEW_TAB", L"S_HOME", L"S_DESKTOP_SITE", L"S_MOBILE_SITE", L"S_FIND_IN_PAGE",
    L"S_SHARE", L"S_COPY_LINK", L"S_DOWNLOAD_PAGE", L"S_BOOKMARKS", L"S_HISTORY",
    L"S_DOWNLOADS", L"S_SETTINGS", L"S_MENU", L"S_MENU_TITLE",
    L"S_SEARCH_PLACEHOLDER", L"S_LOCK_SECURE", L"S_LOCK_INSECURE",
    L"S_LOADING", L"S_PROCESSING", L"S_LOAD_FAILED", L"S_LOAD_TIMEOUT",
    L"S_EMPTY_FAV", L"S_EMPTY_HIST", L"S_EMPTY_DL", L"S_NO_SHARE",
    L"S_SETTINGS_TITLE", L"S_SET_LANG", L"S_SET_LANG_ZH", L"S_SET_LANG_EN", L"S_SET_LANG_RU",
    L"S_SET_SEARCH", L"S_SET_HOME", L"S_SET_HOME_PLACEHOLDER",
    L"S_SET_UA", L"S_SET_UA_CUSTOM", L"S_SET_UA_CUSTOM_PLACEHOLDER",
    L"S_SET_ZOOM", L"S_SET_GPU", L"S_SET_GPU_NOW",
    L"S_SET_CLEAR_DATA", L"S_SET_CLEAR_HIST", L"S_SET_CLEAR_FAV", L"S_SET_CLEAR_DL",
    L"S_SET_DIAG", L"S_SET_EXPORT", L"S_SET_ABOUT", L"S_SET_CHECK_UPDATE",
    L"S_TAB_SWITCHER_TITLE", L"S_TAB_SWITCHER_DONE", L"S_NEW_TAB_BTN",
    L"S_FAV_TAB", L"S_HIST_TAB", L"S_DL_TAB", L"S_ACTION_ADD_FAV", L"S_ACTION_DEL_FAV",
    L"S_ACTION_CLEAR", L"S_ACTION_DL_PAGE", L"S_DL_STARTED", L"S_DL_DONE", L"S_DL_FAILED",
    L"S_COPIED", L"S_CANCELLED", L"S_FOUND_N", L"S_FOUND_NONE", L"S_FOUND_NO_MORE",
    L"S_FIND_PLACEHOLDER", L"S_CANCEL_FAV", L"S_DEL_FAV", L"S_AUTODIAG_DONE",
    L"S_HOME_TITLE", L"S_GPU_ON", L"S_GPU_FAIL", L"S_UPDATE_CHECKING",
    L"S_UPDATE_FAIL", L"S_UPDATE_LATEST", L"S_UPDATE_FOUND", L"S_UPDATE_DLG_TITLE",
    L"S_UPDATE_DLG_GO", L"S_UPDATE_DLG_LATER", L"S_EXPORT_CHOOSE", L"S_EXPORT_FAIL",
    L"S_TOAST_BOOKMARKED", L"S_TOAST_UNBOOKMARKED", L"S_TOAST_HIST_CLEARED",
    L"S_TOAST_FAV_CLEARED", L"S_TOAST_DL_CLEARED", L"S_TOAST_CANNOT_FIND",
    L"S_ENGINE_BUSY",
};

static std::wstring GetStr(int lang, int id)
{
    if (id < 0 || id >= S_COUNT) return L"";
    // Apotheosis: the table is authoritative, not Resources/<lang>/Resources.resw.
    //
    // This used to try ResourceLoader::GetForCurrentView()->GetString() first and only fall back to
    // the table. But a ResourceLoader resolves against the *OS* language, with no way to ask it for
    // a specific one, so on a Russian Windows it returned Russian for every string no matter what
    // the user had picked in Settings -- the in-app language switch did nothing, which is how an
    // English UI (lang=1) ended up with "Обработка..." in its status line. An explicit user choice
    // has to win over the system locale.
    //
    // Doing this the other way round -- keeping MRT and honouring the choice -- means resolving
    // through ResourceManager::Current->MainResourceMap with a ResourceContext whose Languages is
    // set, and regenerating the three .resw from this table so they cannot drift. That is the
    // proper localization pass; the .resw files are left in place for it.
    if (lang < 0 || lang > 2) lang = 1;                       // English, not Chinese, is the fallback
    const wchar_t* s = kStr[lang][id];
    if (!s) s = kStr[1][id];                                  // never hand std::wstring a nullptr
    return s ? std::wstring(s) : std::wstring();
}

void MainPage::ApplyLanguage()
{
    int L = m_uiLang;

    // Quick action buttons in the action sheet (by Tag)
    struct MenuTag { const wchar_t* tag; int strId; };
    static const MenuTag kMenuTags[] = {
        { L"newtab", S_NEW_TAB }, { L"home", S_HOME }, { L"ua", S_DESKTOP_SITE },
        { L"find", S_FIND_IN_PAGE }, { L"share", S_SHARE }, { L"copylink", S_COPY_LINK },
        { L"download", S_DOWNLOAD_PAGE }, { L"bookmarks", S_BOOKMARKS },
        { L"history", S_HISTORY }, { L"downloads", S_DOWNLOADS }, { L"settings", S_SETTINGS },
    };

    if (ActionMenu) {
        try {
            auto outerBorder = dynamic_cast<Border^>(ActionMenu->Children->GetAt(0));
            if (outerBorder) {
                auto sv = dynamic_cast<ScrollViewer^>(outerBorder->Child);
                if (sv) {
                    auto spAll = dynamic_cast<StackPanel^>(sv->Content);
                    if (spAll) {
                        // First child of the stack panel is the quick actions grid
                        if (spAll->Children->Size > 0) {
                            auto quickGrid = dynamic_cast<Grid^>(spAll->Children->GetAt(0));
                            if (quickGrid) {
                                // Grid columns: Back(0), Forward(1), Reload(2), Bookmark(3)
                                auto updateQuickBtn = [&](int col, int strId) {
                                    if (col < (int)quickGrid->Children->Size) {
                                        auto btn = dynamic_cast<Button^>(quickGrid->Children->GetAt(col));
                                        if (btn) {
                                            auto sp = dynamic_cast<StackPanel^>(btn->Content);
                                            if (sp && sp->Children->Size > 1) {
                                                auto tb = dynamic_cast<TextBlock^>(sp->Children->GetAt(1));
                                                if (tb) tb->Text = ref new Platform::String(GetStr(L, strId).c_str());
                                            }
                                        }
                                    }
                                };
                                updateQuickBtn(0, S_BACK);
                                updateQuickBtn(1, S_FORWARD);
                                updateQuickBtn(2, S_REFRESH);
                                // Bookmark button (column 3) ??? ?????????????????? ???????????? ???? ActFavLabel
                                if (3 < (int)quickGrid->Children->Size) {
                                    auto btn = dynamic_cast<Button^>(quickGrid->Children->GetAt(3));
                                    if (btn) {
                                        auto sp = dynamic_cast<StackPanel^>(btn->Content);
                                        if (sp && sp->Children->Size > 1) {
                                            auto tb = dynamic_cast<TextBlock^>(sp->Children->GetAt(1));
                                            if (tb) { ActFavLabel = tb; tb->Text = ref new Platform::String(GetStr(L, S_BOOKMARK).c_str()); }
                                        }
                                    }
                                }
                            }
                        }
                        // Menu items (buttons with Tag) in the stack panel
                        for (unsigned i = 0; i < spAll->Children->Size; ++i) {
                            auto btn = dynamic_cast<Button^>(spAll->Children->GetAt(i));
                            if (!btn) continue;
                            auto tagObj = dynamic_cast<Platform::String^>(btn->Tag);
                            if (!tagObj) continue;
                            std::wstring tval(tagObj->Data());
                            for (const auto& mt : kMenuTags) {
                                if (tval == mt.tag) {
                                    auto sp = dynamic_cast<StackPanel^>(btn->Content);
                                    if (sp && sp->Children->Size > 1) {
                                        auto tb = dynamic_cast<TextBlock^>(sp->Children->GetAt(1));
                                        if (tb) {
                                            tb->Text = ref new Platform::String(GetStr(L, mt.strId).c_str());
                                            if (tval == L"ua") ActUaLabel = tb;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }

    // ActUaLabel fallback
    if (ActUaLabel)
        ActUaLabel->Text = ref new Platform::String(GetStr(L, m_uaMobile ? S_MOBILE_SITE : S_DESKTOP_SITE).c_str());

    // Placeholder texts
    if (FindBox) FindBox->PlaceholderText = ref new Platform::String(GetStr(L, S_FIND_IN_PAGE).c_str());
    if (UrlBox) UrlBox->PlaceholderText = ref new Platform::String(GetStr(L, S_SEARCH_PLACEHOLDER).c_str());

    // Tab switcher title
    if (TabSwitcherTitle)
        TabSwitcherTitle->Text = ref new Platform::String((GetStr(L, S_TAB_SWITCHER_TITLE) + L" (" + std::to_wstring(m_tabs.size()) + L")").c_str());

    // Drawer buttons
    if (UaBtn) UaBtn->Content = ref new Platform::String((m_uaMobile ? GetStr(L, S_MOBILE_SITE) : GetStr(L, S_DESKTOP_SITE)).c_str());
    if (GpuBtn) GpuBtn->Content = ref new Platform::String((m_gpuOn ? GetStr(L, S_GPU_ON) : GetStr(L, S_GPU_FAIL)).c_str());
    if (TabFav) TabFav->Content = ref new Platform::String(GetStr(L, S_FAV_TAB).c_str());
    if (TabHist) TabHist->Content = ref new Platform::String(GetStr(L, S_HIST_TAB).c_str());
    if (TabDl) TabDl->Content = ref new Platform::String(GetStr(L, S_DL_TAB).c_str());

    // Title on home page
    if (m_currentUrl == L"about:home" && TitleText)
        TitleText->Text = ref new Platform::String(GetStr(L, S_HOME_TITLE).c_str());
}

// Apotheosis: everything the constructor does before LogInit() -- which runs deep inside
// SetupRuntimeEnvOnce(), some 280 lines down -- is invisible to log.txt, because LogWrite() only
// buffers in memory until then. Anything that throws in that window (an XBF parse failure, a
// stale-layout mismatch, a DLL that will not load) kills the process with an empty LocalState,
// which is indistinguishable from "the app does not start" and cannot be attached to on the phone,
// VS 2022 having dropped ARM32 device debugging. EarlyMark appends one line per checkpoint and
// closes the file immediately, so the last line present is where we died.
static void EarlyMark(const char* tag)
{
    OutputDebugStringA(tag);
    OutputDebugStringA("\n");
    try {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
        wcscat_s(path, L"\\ctor-trace.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"a") == 0 && f) {
            fprintf(f, "%llu %s\n", (unsigned long long)GetTickCount64(), tag);
            fclose(f);
        }
    } catch (...) {
        // Intentionally empty: a diagnostic must never be the thing that kills the launch.
    }
}

// ============================================================================
MainPage::MainPage()
{
    // EARLY diagnostic
    OutputDebugStringW(L"MainPage::MainPage() - constructor entry\n");
    LogWrite("MainPage::MainPage() - constructor entry");
    // Apotheosis: first heartbeat now, then every 700 ms (was 2 s). On a crash this file goes
    // stale and the next launch's CRASHVERDICT line reports how fresh (or absent) the last beat
    // was. The cadence drop is for the wedge snapshot: the silent killer acts within ~2 s of the
    // engine wedge, and at 2 s the first stuck beat -- the only one that gets to snapshot --
    // regularly lost that race, leaving an empty wedgedump.txt behind.
    WriteMarkerFile(L"heartbeat.txt", (LogTimestamp() + " beat\n").c_str());
    m_heartbeatTimer = ref new Windows::UI::Xaml::DispatcherTimer();
    {
        Windows::Foundation::TimeSpan hts; hts.Duration = 700LL * 10000LL;   // 700 ms
        m_heartbeatTimer->Interval = hts;
    }
    m_heartbeatTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnHeartbeat);
    m_heartbeatTimer->Start();
    // Direct file write (synchronous, no WinRT async that could deadlock UI thread)
    try {
        wchar_t entryPath[MAX_PATH];
        wcscpy_s(entryPath, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
        wcscat_s(entryPath, L"\\constructor-entry.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, entryPath, L"w") == 0 && f) {
            fputs("MainPage::MainPage() reached\n", f);
            fclose(f);
        }
    } catch (...) {
        // Intentionally empty — crash before here means constructor never started
    }
    // Apotheosis: start a fresh checkpoint trail for this launch, then record every step of the
    // pre-LogInit window. Truncating here rather than appending forever keeps the file readable
    // in the in-app viewer, where only the current launch matters.
    try {
        wchar_t tracePath[MAX_PATH];
        wcscpy_s(tracePath, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
        wcscat_s(tracePath, L"\\ctor-trace.txt");
        DeleteFileW(tracePath);
    } catch (...) {}
    EarlyMark("ctor: entry");
    // Apotheosis: a markup error throws right here, and App::OnLaunched cannot usefully recover
    // from it, so name the failure before doing anything else. We deliberately do not rethrow:
    // the code-only fallback UI below is what the fork has always fallen back to, and this way the
    // cause ends up in ctor-trace.txt instead of vanishing with the process.
    try {
        InitializeComponent();
        EarlyMark("ctor: InitializeComponent ok");
    } catch (Platform::Exception^ ex) {
        char buf[512];
        sprintf_s(buf, "ctor: InitializeComponent THREW hr=0x%08X msg=%ls",
                  (unsigned)ex->HResult, ex->Message ? ex->Message->Data() : L"(none)");
        EarlyMark(buf);
    } catch (...) {
        EarlyMark("ctor: InitializeComponent THREW (non-WinRT exception)");
    }
    LogWrite("MainPage: InitializeComponent done");

    // If the markup did not come up, build minimal fallback UI in code.
    // This bypasses the 0xc000027b crash in twinapi.appcore.dll on Win11 29617.
    // Apotheosis: testing RootGrid alone was too weak. A desynchronised Connect() left RootGrid
    // assigned and ContentArea null, so this check passed and the very next statement dereferenced
    // a null handle. Every field the constructor uses unconditionally has to be there before the
    // tree can be trusted, and the values go into the trace because guessing cost a day.
    const bool markupOk = (RootGrid != nullptr && ContentArea != nullptr && RootShift != nullptr);
    {
        char buf[256];
        sprintf_s(buf, "ctor: markup %s (RootGrid=%p ContentArea=%p RootShift=%p)",
                  markupOk ? "ok -> full UI" : "INCOMPLETE -> code-only fallback UI",
                  (void*)RootGrid, (void*)ContentArea, (void*)RootShift);
        EarlyMark(buf);
    }

    // Apotheosis (MVP, 2026-09-25): FindBar and SuggestPanel are the only two overlay blocks whose
    // XAML default is Visible (they carry no Visibility="Collapsed"), so they sit over the content and
    // the URL area until some fragile code path collapses them -- which is why the address bar fought
    // the user. Force them collapsed at startup. This is a runtime-only change: the XAML and the
    // generated MainPage.g.hpp are untouched, so the Connect() id contract stays valid.
    if (FindBar) FindBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (SuggestPanel) SuggestPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;

    if (!markupOk) {
        LogWrite("MainPage: XBF not loaded — building code-only fallback UI");
        auto root = ref new Grid();
        root->Background = ref new SolidColorBrush(Windows::UI::Colors::Black);
        auto img = ref new Windows::UI::Xaml::Controls::Image();
        img->Width = 720;
        img->Height = 1080;
        img->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        img->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        root->Children->Append(img);
        auto tb = ref new TextBlock();
        tb->Text = L"Apotheosis v0.1.8.25 — Gradient test";
        tb->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
        tb->FontSize = 28;
        tb->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        tb->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
        tb->Margin = Thickness(0, 0, 0, 30);
        root->Children->Append(tb);
        this->Content = root;
        // 1. Show "Loading..." text initially
        auto loadText = ref new TextBlock();
        loadText->Text = L"Apotheosis v0.1.8.25 — Loading\u2026";
        loadText->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
        loadText->FontSize = 24;
        loadText->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        loadText->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        root->Children->Append(loadText);
        this->Content = root;
        // 2. Create WriteableBitmap and render about:home on engine thread
        auto bmp = ref new WriteableBitmap(kW, kH);
        img->Source = bmp;
        auto dispatcher = this->Dispatcher;
        // Ultra-simple about:home — solid background, big centered text, no CSS complexity.
        // Cairo font/cradient rendering may be unreliable, so keep it minimal.
        std::string homeHtml = "<html><head><meta charset='utf-8'>"
            "<body style='margin:0;background:#1a1a2e;"
            "display:flex;align-items:center;justify-content:center;height:100vh'>"
            "<div style='text-align:center;color:white;font-family:sans-serif'>"
            "<h1 style='font-size:64px;margin:0'>EdgeHTML Reborn</h1>"
            "<p style='font-size:28px;margin:20px 0;color:#a0c4ff'>"
            "WebKit 2.52.4 &middot; Windows 10 Mobile</p>"
            "<p style='font-size:20px;color:#888'>Loading\u2026</p></div></body></html>";
        LogWrite("MainPage: posting WebCoreRenderHtml for about:home");
        EarlyMark("ctor(fallback): posting WebCoreRenderHtml");
        WebEngine::instance().post("render-home-fallback", [dispatcher, bmp, homeHtml, loadText]() {
            EarlyMark("engine(fallback): WebCoreRenderHtml enter");
            // Apotheosis 2026-09-19: same snapshot rule as the autodiag job below. `bmp` was created at
            // a fixed size and `rgba` must match the size it was ALLOCATED at, not whatever ApplyViewportSize
            // has since written into kW/kH on the UI thread -- this path runs during startup, which is
            // exactly when that resize lands, and the UI-thread memcpy below would then read past the
            // buffer AND write past the bitmap.
            const int hw = kW;
            const int hh = kH;
            auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)hw * hh * 4, 0);
            int rc = -999;
            try { rc = WebCoreRenderHtml(homeHtml.c_str(), hw, hh, rgba->data()); } catch (...) { rc = -1000; }
            EarlyMark("engine(fallback): WebCoreRenderHtml returned");
            LogWriteF("WebCoreRenderHtml: rc=%d size=%dx%d", rc, hw, hh);
            // Debug: dump the engine frame to LocalState (bypasses XAML display).
            try { WriteBmp32(WideToUtf8(LocalStateDir()) + "\\\\shot_ui.bmp", rgba->data(), hw, hh); LogWrite("frame dumped to shot_ui.bmp"); } catch (...) {}
            OutputDebugStringA("[UI] RunAsync posting\n");
            dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([bmp, rgba, rc, loadText, hw, hh]() {
                OutputDebugStringA("[UI] RunAsync entered\n");
                if (rc != 0) { OutputDebugStringA("[UI] rc!=0, returning\n"); return; }
                OutputDebugStringA("[UI] getting PixelBuffer\n");
                auto pixelBuffer = bmp->PixelBuffer;
                OutputDebugStringA("[UI] QueryInterface\n");
                Windows::Storage::Streams::IBufferByteAccess* ba = nullptr;
                if (SUCCEEDED(reinterpret_cast<IUnknown*>(pixelBuffer)->QueryInterface(
                        __uuidof(Windows::Storage::Streams::IBufferByteAccess),
                        reinterpret_cast<void**>(&ba)))) {
                    BYTE* pixels = nullptr;
                    if (SUCCEEDED(ba->Buffer(&pixels)))
                        memcpy(pixels, rgba->data(), (size_t)hw * hh * 4);
                    ba->Release();
                }
                OutputDebugStringA("[UI] Invalidating\n");
                bmp->Invalidate();
                OutputDebugStringA("[UI] Done\n");
                // Hide the "Loading..." placeholder — it is stacked above the Image
                // (added later) and would otherwise cover the rendered page.
                loadText->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                LogWrite("WebCoreRenderHtml: about:home displayed");
            }));
        });
        LogWrite("MainPage: fallback UI set — constructor done");
        EarlyMark("ctor(fallback): constructor done");
        return;
    }

    // Add startup overlay (name + version) — hidden when first page loads
    try {
        auto overlay = ref new TextBlock();
        // Apotheosis: read the version from the package, do not spell it out here.
        //
        // This line said L"Apotheosis v0.1.8.25" until 2026-08-22 -- a literal frozen roughly a
        // hundred builds earlier, so the splash showed .25 while .41 was installed. The maintainer
        // spotted it mid-test, and the danger is exactly that: during a deploy-and-reproduce loop the
        // version on screen is what you use to confirm the package you are looking at, and a stale
        // literal turns that check into a source of false conclusions. Package::Current->Id->Version
        // is already how BuildDiagReport and the update check read it.
        auto pv = Windows::ApplicationModel::Package::Current->Id->Version;
        wchar_t vtext[64];
        swprintf_s(vtext, L"Apotheosis v%u.%u.%u.%u",
            (unsigned)pv.Major, (unsigned)pv.Minor, (unsigned)pv.Build, (unsigned)pv.Revision);
        overlay->Text = ref new String(vtext);
        overlay->FontSize = 24;
        overlay->Foreground = ref new SolidColorBrush(Windows::UI::Colors::Gray);
        overlay->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        overlay->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        m_startupOverlay = overlay;
        ContentArea->Children->Append(overlay);
    } catch (...) {}
    EarlyMark("ctor: startup overlay ok");

#if defined(MINIMAL_TEST)
    OutputDebugStringW(L"MainPage: MINIMAL TEST MODE - skipping full initialization\n");
    LogWrite("MainPage: MINIMAL TEST MODE");

    // Test: write a file to LocalState
    try {
        auto folder = Windows::Storage::ApplicationData::Current->LocalFolder;
        auto file = concurrency::create_task(
            folder->CreateFileAsync(L"minimal-test.txt",
                Windows::Storage::CreationCollisionOption::ReplaceExisting)).get();
        concurrency::create_task(
            Windows::Storage::FileIO::WriteTextAsync(file,
                L"Minimal test SUCCESS\n")).get();
        LogWrite("MainPage: minimal test file written OK");
    } catch (Platform::Exception^ ex) {
        LogWriteF("MainPage: exception writing test file: %ls", ex->Message->Data());
    }
    OutputDebugStringW(L"MainPage: MINIMAL TEST MODE - constructor exit\n");
    LogWrite("MainPage::MainPage() - MINIMAL TEST MODE exit");
    return;
}
#else

    // Apotheosis: Register DataTransferManager for share
    try {
        auto dtm = Windows::ApplicationModel::DataTransfer::DataTransferManager::GetForCurrentView();
        dtm->DataRequested += ref new Windows::Foundation::TypedEventHandler<
            Windows::ApplicationModel::DataTransfer::DataTransferManager^,
            Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^>(
            [this](Windows::ApplicationModel::DataTransfer::DataTransferManager^,
                   Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^ e) {
                // Apotheosis: the diagnostics page shares a log report rather than a link. It parks
                // the text in m_pendingShareText and calls ShowShareUI; we consume it here so the
                // ordinary "share this page" path below stays exactly as it was.
                if (!m_pendingShareText.empty()) {
                    auto req0 = e->Request;
                    req0->Data->Properties->Title = ref new Platform::String(L"Apotheosis diagnostics");
                    req0->Data->Properties->Description = ref new Platform::String(L"Engine logs from LocalState");
                    req0->Data->SetText(ref new Platform::String(m_pendingShareText.c_str()));
                    m_pendingShareText.clear();
                    return;
                }
                if (m_currentUrl.empty() || m_currentUrl == L"about:home") {
                    e->Request->FailWithDisplayText(ref new Platform::String(GetStr(m_uiLang, S_NO_SHARE).c_str()));
                    return;
                }
                auto req = e->Request;
                req->Data->Properties->Title = ref new Platform::String(
                    m_currentTitle.empty() ? m_currentUrl.c_str() : m_currentTitle.c_str());
                req->Data->Properties->Description = ref new Platform::String(m_currentUrl.c_str());
                try {
                    req->Data->SetWebLink(ref new Windows::Foundation::Uri(ref new Platform::String(m_currentUrl.c_str())));
                } catch (...) {
                    req->Data->SetText(ref new Platform::String(m_currentUrl.c_str()));
                }
            });
    } catch (...) {}

    LogWrite("MainPage: constructor - setup phase start");
    EarlyMark("ctor: DataTransferManager ok");

    // Pre-load engine DLLs via LoadPackagedLibrary so delay-load hooks succeed.
    // We load them explicitly here (after process init is complete) to ensure
    // DllMain runs in a safe context with full CRT/COM/XAML initialization.
    // Order matters: JavaScriptCore.dll first (WebCore depends on it).
    LogWrite("MainPage: pre-loading engine DLLs via LoadPackagedLibrary");
    // Apotheosis: the UEF stack dump prints raw addresses, and ASLR moves every module
    // on each run. Without the exe's own base the EXE frames -- i.e. our driver, the
    // half of the stack we actually control -- cannot be symbolized at all. Log it
    // alongside the DLL bases so a crash log is self-contained for llvm-symbolizer.
    LogWriteF("  ModuleBase(Harness.exe) = %p", (void*)&__ImageBase);
    g_baseExe = (uintptr_t)&__ImageBase;   // Apotheosis: for AppendSym in WriteWedgeDump
    {
        HMODULE hJSC = LoadPackagedLibrary(L"JavaScriptCore.dll", 0);
        LogWriteF("  LoadPackagedLibrary(JavaScriptCore.dll) = %p (err=%lu)", (void*)hJSC, hJSC ? 0UL : GetLastError());
        HMODULE hWebCore = hJSC ? LoadPackagedLibrary(L"WebCore.dll", 0) : nullptr;
        LogWriteF("  LoadPackagedLibrary(WebCore.dll) = %p (err=%lu)", (void*)hWebCore, hWebCore ? 0UL : GetLastError());
        if (!hWebCore) {
            // Retry after a brief pause - some packages need settle time
            Sleep(100);
            DWORD err1 = GetLastError();
            hWebCore = LoadPackagedLibrary(L"WebCore.dll", 0);
            DWORD err2 = GetLastError();
            LogWriteF("  LoadPackagedLibrary(WebCore.dll) retry = %p (err1=%lu err2=%lu)", (void*)hWebCore, err1, err2);
        }
        // Apotheosis: wedge-snapshot module bases (see AppendSym above). The logged %p values and
        // these statics are written from the same call sites so they can never disagree.
        g_baseJsc = (uintptr_t)hJSC;
        g_baseWebCore = (uintptr_t)hWebCore;
    }
    EarlyMark("ctor: engine DLLs preloaded");

    // ??????????????????????????????(JIT ?????????),????????? LocalState\jitresult.txt ??? WDP ?????????
    try {
        LogWrite("MainPage: running JIT probe");
        std::string jit = RunJitProbe();
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ofstream jf(WideToUtf8(d) + "\\jitresult.txt", std::ios::binary | std::ios::trunc);
            if (jf) jf.write(jit.data(), jit.size());
        }
        LogWriteF("MainPage: JIT probe done (%lld bytes)", (long long)jit.size());
    } catch (...) { LogWrite("MainPage: JIT probe exception"); }
    EarlyMark("ctor: JIT probe done");
    LogWrite("MainPage: LoadData+LoadSettings");
    LoadData();
    LoadSettings();   // ????????????/??????/??????UA/??????/????????????(????????????????????????)
    EarlyMark("ctor: LoadData+LoadSettings ok");
    UpdateKeepAwake(true);   // Apotheosis: honour keepawake=1 from the moment the app starts
    StartNavWatch();         // Apotheosis: make the harness navigation path scriptable via nav.txt
    StartNavSeq();           // Apotheosis: and playable from a packaged sequence, for the device
    // ?????????????????????:????????????????????????????????????????????????,???????????? 1 ???(?????????????????????????????????)???
    { Tab t0; t0.currentUrl = g_homeUrl; m_tabs.push_back(t0); m_activeTab = 0; }
    UpdateTabCount();
    // GPU ??????????????????:??????????????? GPU ???????????????(????????????)??? ?????????????????????,??????????????????(????????????????????????)???
    LogWrite("MainPage: checking GPU crash flag");
    {
        std::wstring d = LocalStateDir();
        if (!d.empty() && GetFileAttributesW((d + L"\\gpu-crash.flag").c_str()) != INVALID_FILE_ATTRIBUTES) {
            m_gpuDefault = false;
            LogWrite("MainPage: GPU crash flag found - disabling GPU default");
            try { DeleteFileW((d + L"\\gpu-crash.flag").c_str()); } catch (...) {}
            SaveSettings();
        } else {
            LogWrite("MainPage: no GPU crash flag");
        }
    }
    // Apotheosis: watch the app's memory level, because on Windows 10 Mobile the platform terminates an
    // app that exceeds its quota WITHOUT raising an exception -- no UEF line, no minidump, the log
    // simply stops mid-sentence. That is exactly the shape of the ya.ru failure on the Lumia
    // (2026-08-19): "nav: OK", then a second navigation starts, then nothing. A single sampled reading
    // cannot catch it, because the interesting moment is the transition, so subscribe to it.
    //
    // AppMemoryUsageIncreased fires when the level crosses upward (low -> medium -> high -> overlimit),
    // and OverLimit is the last warning before the process is taken away. Each line is flushed by
    // LogWrite, so whatever arrives last stays on disk.
    //
    // Note the diagnostic value of the negative result too: if a launch dies with no `mem-level` line
    // above `medium`, memory is honestly excluded rather than merely suspected -- and the same silent
    // death has been reported by the other W10M port (Revenant, YouTube stopping after about a minute),
    // so distinguishing "killed by quota" from "killed for something else" matters beyond this bug.
    // Apotheosis 2026-09-24: these handlers used to only log, and that made the engine's entire remedy
    // for memory pressure unreachable -- WebCoreReleaseMemory had no caller anywhere under Src/ while
    // the port's own comment claimed "harness calls WebCoreReleaseMemory() under memory pressure". The
    // SILENT LIE class of Doc/STUB-AUDIT.md. They now post a release job.
    //
    // A posted job, not a direct call: the iron rule is that every C ABI call runs on the engine thread
    // and the UI thread never blocks on the engine. WebEngine::post is mutex+cv and callable from any
    // thread, so the handler stays non-blocking, and the release lands between jobs where it cannot
    // interleave with a navigation's own session teardown. On the phone this is the half that actually
    // matters: the Lumia reports pressure through this API, and a reap with no UWP exception, no dump
    // and a log that just stops mid-sentence is what an unhandled OverLimit looks like.
    LogWrite("MainPage: registering memory-level handler");
    try {
        Windows::System::MemoryManager::AppMemoryUsageIncreased += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [](Platform::Object^, Platform::Object^) {
                LogWriteF("mem-level UP: %s", MemoryLine().c_str());
                // Apotheosis 2026-09-24: the level gate, and the reason it is not optional.
                // This event fires at handler registration on a fresh launch, carrying level=low --
                // the lowest level there is. Posting a release for it was what made the app die at
                // startup: the job reached the engine thread before the loop's first navigation and
                // before process init, and faulted. The gate is the correct semantics (no pressure,
                // nothing to release) and the driver now initialises regardless, so neither half
                // depends on the other -- but a skipped release is logged either way, because a
                // silent skip is how the previous version of this handler hid its own uselessness.
                if (!MemoryLevelIsPressure()) {
                    LogWrite("mem-release: skipped -- level below medium is the platform's own resting state");
                    return;
                }
                // Logged, not implied. "mem-level UP followed by no release line" used to be the only
                // way to tell a gated release from a posted one, which made a reader infer the branch
                // from an absence -- and inference from absence is what hid the original defect. The
                // two decisions now have two different lines, and the own word `posting` is what the
                // soak tool counts, so its counter cannot be satisfied by the arming probe's own line.
                LogWrite("mem-release: posting -- level at or above medium");
                WebEngine::instance().post("mem-release", []() {
                    try { WebCoreReleaseMemory(1); } catch (...) {}
                });
            });
        Windows::System::MemoryManager::AppMemoryUsageDecreased += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [](Platform::Object^, Platform::Object^) {
                LogWriteF("mem-level DOWN: %s", MemoryLine().c_str());
            });
        Windows::System::MemoryManager::AppMemoryUsageLimitChanging += ref new Windows::Foundation::EventHandler<Windows::System::AppMemoryUsageLimitChangingEventArgs^>(
            [](Platform::Object^, Windows::System::AppMemoryUsageLimitChangingEventArgs^ e) {
                LogWriteF("mem-limit changing: %llu -> %llu KB, %s",
                    e->OldLimit / 1024ULL, e->NewLimit / 1024ULL, MemoryLine().c_str());
                // Only when the ceiling drops. The event fires in both directions, and releasing on an
                // increase would throw away caches for no reason -- on the Lumia that is re-decoding
                // paid for by a phone with no CPU to spare.
                if (e->NewLimit < e->OldLimit) {
                    WebEngine::instance().post("mem-release", []() {
                        try { WebCoreReleaseMemory(1); } catch (...) {}
                    });
                }
            });
        LogWriteF("MainPage: memory-level handlers registered, %s", MemoryLine().c_str());
    } catch (Platform::Exception^ ex) {
        LogWriteF("MainPage: memory-level handlers FAILED hr=0x%08X", (unsigned)ex->HResult);
    } catch (...) {
        LogWrite("MainPage: memory-level handlers FAILED (non-WinRT exception)");
    }
    // Apotheosis 2026-09-24: a deterministic reproduction of the startup race, armed by a file.
    //
    // The defect it reproduces needs the platform to raise AppMemoryUsageIncreased on its own, and the
    // platform does that only SOMETIMES -- measured: 3 launches in ~10 produced the event and died, and
    // then 12 consecutive launches produced no event at all (startup-soak.ps1 reports that case
    // explicitly, because a green sweep that never armed the trigger is not evidence). A race that
    // cannot be provoked on demand cannot be verified fixed either, so this arms it by hand.
    //
    // This is the same mechanism the port already uses for jstack.txt and texttrace.txt, and for the
    // same reason: the bench launches the app through shell:AppsFolder, so the app is activated by the
    // shell and never sees the PowerShell session's environment. Anything a run must be told arrives as
    // a file in LocalState.
    //
    // The timing is the point, and it is not incidental: this runs HERE, at handler registration on the
    // UI thread, which is 54 ms BEFORE `WebEngine: loop ready` (measured 21:45:54.425 -> .479). The job
    // therefore sits in the queue and is the engine thread's FIRST WebCore call of the process, ahead
    // of any navigation and ahead of process init -- exactly where the dying launches put it. Posting it
    // after the loop starts would test nothing.
    // Read synchronously through ReadMarkerFile (std::ifstream over LocalFolder->Path, the same helper
    // the crash verdict uses) rather than through StorageFolder::TryGetItemAsync: the async form threw
    // here and was swallowed by the catch, which is precisely the silent failure this instrument exists
    // to avoid -- measured on the first attempt, where the log said only "arming check failed".
    //
    // The result is logged either way. A probe that can be skipped in silence is how the previous
    // version of these handlers hid its own uselessness, and a reader has to be able to tell "checked,
    // not armed" from "never checked". The file must be non-empty: ReadMarkerFile reads one line, and
    // an empty first line is indistinguishable from an absent file.
    {
        const std::string armed = ReadMarkerFile(L"mem-release-race.txt");
        LogWriteF("mem-release-race: checked, armed=%d", armed.empty() ? 0 : 1);
        if (!armed.empty()) {
            LogWrite("mem-release-race: posting mem-release BEFORE the engine loop is ready");
            WebEngine::instance().post("mem-release", []() {
                try { WebCoreReleaseMemory(1); } catch (...) {}
            });
        }
    }
    // ???????????????:????????????????????????(????????????????????????????????? PLM ???????????????)???
    LogWrite("MainPage: registering VisibilityChanged handler");
    Window::Current->VisibilityChanged += ref new Windows::UI::Xaml::WindowVisibilityChangedEventHandler(
        [this](Platform::Object^, Windows::UI::Core::VisibilityChangedEventArgs^ e) {
            LogWriteF("MainPage: VisibilityChanged foreground=%d", e->Visible ? 1 : 0);
            m_appForeground = e->Visible;
            UpdateKeepAwake(e->Visible);
            if (e->Visible) StartLiveMode(); else StopLiveMode();
        });
    // Apotheosis: the engine viewport follows the render host. Wired here rather than in the XAML
    // so that MainPage.g.hpp does not have to be regenerated. The first SizeChanged of the layout
    // pass is what replaces the 720x1080 startup default with the real window size.
    ContentArea->SizeChanged += ref new Windows::UI::Xaml::SizeChangedEventHandler(
        this, &MainPage::OnContentAreaSizeChanged);
    // ???????????????:?????????????????????,????????????????????????????????????????????????????????????????????????????????????
    //   (????????????+????????????????????????);??????????????????(ImeBox)??????????????????????????????????????????????????????
    LogWrite("MainPage: registering InputPane handlers");
    try {
        auto ip = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
        ip->Showing += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                if (m_urlFocused && RootShift) {
                    RootShift->Y = -e->OccludedRect.Height;
                    e->EnsuredFocusedElementInView = true;   // ???????????????,????????????????????????
                }
            });
        ip->Hiding += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                if (RootShift && RootShift->Y != 0) {   // ??????????????????????????????+??????(?????????????????????????????????????????????,?????????)
                    RootShift->Y = 0;
                    e->EnsuredFocusedElementInView = true;
                }
            });
    } catch (...) { LogWrite("MainPage: InputPane registration exception"); }
    EarlyMark("ctor: event handlers registered");
    // ????????????:??? LocalState\testurl.txt ??????,????????????????????????(??? WDP ?????????????????????,??? UI ??????)???
    std::wstring testUrl;
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ifstream f(WideToUtf8(d) + "\\testurl.txt", std::ios::binary);
            if (f) { std::string s; std::getline(f, s); testUrl = Utf8ToWide(s); }
            while (!testUrl.empty() && (testUrl.back() == L'\r' || testUrl.back() == L'\n' || testUrl.back() == L' ' || testUrl.back() == L'\t'))
                testUrl.pop_back();
        }
    } catch (...) {}
    // ??????????????????:??? LocalState\autodiag.txt ??????(???????????? URL,# ????????????),????????????????????????
    //   ?????? GpuInit(??????)+ ?????? GPU ???????????? + dump ?????? diag/????????? autodump.txt,??? WDP ???????????????
    //   (??? UI ?????? GPU ??????)???????????????"????????? GPU ????????????"?????????????????????????????????????????????????????????
    std::vector<std::string> diagUrls;
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ifstream f(WideToUtf8(d) + "\\autodiag.txt", std::ios::binary);
            std::string line;
            while (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                if (!line.empty() && line[0] != '#')
                    diagUrls.push_back(line);
            }
        }
    } catch (...) {}
    // Setup global runtime environment (fontconfig, CA blob) before any engine call
    LogWrite("MainPage: calling SetupRuntimeEnv early");
    SetupRuntimeEnv();
    EarlyMark("ctor: SetupRuntimeEnv done (log.txt live from here)");

    // (?????????)autodiag.txt ???????????????;??????????????????????????????????????????????????? autodiag.txt(???????????? URL,DESKTOP: ??????=?????? UA)???
    if (!diagUrls.empty()) {
        CoreDispatcher^ disp = this->Dispatcher;
        Platform::Agile<MainPage^> self(this);
        WebEngine::instance().post("diag-urls", [disp, self, diagUrls]() {
            std::string dump;
            int gi = -999;
            try { gi = WebCoreGpuInit(nullptr, kW, kH); } catch (...) { gi = -1000; }
            dump += "WebCoreGpuInit(offscreen) rc=" + std::to_string(gi) + "\n\n";
            // Apotheosis 2026-09-19: snapshot the frame size ONCE, here, and use the snapshot for the
            // allocation, every WebCoreSessionLoad and the BMP dump. `kW`/`kH` are mutable globals that
            // ApplyViewportSize rewrites on the UI thread (`kW = ew; kH = eh;`, m_sessionActive==0
            // branch), and this job runs on the engine thread: reading them again at dump time let the
            // buffer be allocated for 720x1080 and then walked as 1368x758 -- WriteBmp32 starts at
            // `rgba + (h-1)*w*4`, i.e. ~1 MB PAST the end of a 3.1 MB buffer, and the first pixel read
            // faulted (measured: VEH 0xC0000005 reading 0x82183FF4E2, symbolised to WriteBmp32:1635
            // <- this lambda:2407 <- WebEngine::loop, 1.3 s after ApplyViewportSize logged 1368x758).
            // Latent on the phone, where the engine size never leaves the Lumia's 720x1080.
            const int aw = kW;
            const int ah = kH;
            auto rgba = std::vector<uint8_t>((size_t)aw * ah * 4, 0);
            std::wstring dd = LocalStateDir();
            int idx = 0;
            for (const auto& rawUrl : diagUrls) {
                std::string url = rawUrl;
                bool desktop = false;
                if (url.rfind("DESKTOP:", 0) == 0) { desktop = true; url = url.substr(8); }
                try { WebCoreSetUserAgentMobile(desktop ? 0 : 1); } catch (...) {}
                dump += "########## URL: " + url + (desktop ? " [desktop UA]" : " [mobile UA]") + " ##########\n";
                int lrc = -999;
                try { lrc = WebCoreSessionLoad(url.c_str(), aw, ah, rgba.data()); } catch (...) { lrc = -1000; }
                dump += "SessionLoad rc=" + std::to_string(lrc) + "\n";
                std::vector<char> dg(4096, 0);
                try { WebCoreGetDiag(dg.data(), (int)dg.size()); } catch (...) {}
                dump += "diag: " + std::string(dg.data()) + "\n";
                std::vector<char> li(65536, 0);
                try { WebCoreGpuLayerInfo(li.data(), (int)li.size()); } catch (...) {}
                dump += std::string(li.data());
                dump += "\n\n";
                // ???????????? GPU readback ????????????(BMP),??? WDP ??????????????????
                // The size is in the file name so the artifact states the engine size it was taken at
                // instead of leaving it to be inferred from the pixels.
                try { if (!dd.empty()) WriteBmp32(WideToUtf8(dd) + "\\shot_" + std::to_string(idx) + "_" + std::to_string(aw) + "x" + std::to_string(ah) + ".bmp", rgba.data(), aw, ah); } catch (...) {}
                ++idx;
            }
            try {
                std::wstring d2 = LocalStateDir();
                if (!d2.empty()) {
                    std::ofstream f(WideToUtf8(d2) + "\\autodump.txt", std::ios::binary | std::ios::trunc);
                    if (f) f.write(dump.data(), dump.size());
                }
            } catch (...) {}
            try {
                disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->TitleText->Text = ref new Platform::String(L"AUTODIAG DONE");
                }));
            } catch (...) {}
            LogWriteF("MainPage: autodiag with %zu URLs", diagUrls.size());
        });
    } else if (!testUrl.empty()) {
        LogWriteF("MainPage: testurl.txt found, navigating to %s", WideToUtf8(testUrl).c_str());
        NavigateTo(ref new String(testUrl.c_str()), true);
    } else {
        LogWrite("MainPage: navigating to home");
        NavigateTo(ref new String(g_homeUrl.c_str()), true);   // ??????(????????????)
    }
    LogWrite("MainPage::MainPage() - constructor exit");
}
#endif // !MINIMAL_TEST

// ---- ????????? ----
static std::vector<Entry> ReadEntries(const std::wstring& path)
{
    std::vector<Entry> out;
    std::ifstream f(WideToUtf8(path), std::ios::binary);
    if (!f) return out;
    std::stringstream ss; ss << f.rdbuf();
    std::string all = ss.str();
    std::wstring w = Utf8ToWide(all);
    std::wstringstream ws(w);
    std::wstring line;
    while (std::getline(ws, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        Entry e;
        size_t t1 = line.find(L'\t');
        size_t t2 = (t1 == std::wstring::npos) ? std::wstring::npos : line.find(L'\t', t1 + 1);
        if (t1 == std::wstring::npos) { e.url = line; }
        else {
            e.url = line.substr(0, t1);
            if (t2 == std::wstring::npos) e.title = line.substr(t1 + 1);
            else { e.title = line.substr(t1 + 1, t2 - t1 - 1); e.extra = line.substr(t2 + 1); }
        }
        out.push_back(e);
    }
    return out;
}
static void WriteEntries(const std::wstring& path, const std::vector<Entry>& v)
{
    std::wstring w;
    for (auto& e : v) { w += e.url; w += L'\t'; w += e.title; w += L'\t'; w += e.extra; w += L'\n'; }
    std::ofstream f(WideToUtf8(path), std::ios::binary | std::ios::trunc);
    if (f) { std::string u = WideToUtf8(w); f.write(u.data(), u.size()); }
}

void MainPage::LoadData()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) return;
    m_bookmarks = ReadEntries(d + L"\\bookmarks.tsv");
    m_historyList = ReadEntries(d + L"\\history.tsv");
    m_downloads = ReadEntries(d + L"\\downloads.tsv");
}
void MainPage::SaveBookmarks() { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\bookmarks.tsv", m_bookmarks); }
void MainPage::SaveHistory()   { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\history.tsv", m_historyList); }
void MainPage::SaveDownloads() { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\downloads.tsv", m_downloads); }

void MainPage::AddHistory(const std::wstring& url, const std::wstring& title)
{
    if (url.empty() || url == L"about:home") return;
    m_historyList.erase(std::remove_if(m_historyList.begin(), m_historyList.end(),
        [&](const Entry& e) { return e.url == url; }), m_historyList.end());
    Entry e; e.url = url; e.title = title.empty() ? url : title;
    m_historyList.insert(m_historyList.begin(), e);
    if (m_historyList.size() > 300) m_historyList.resize(300);
    SaveHistory();
}
bool MainPage::IsBookmarked(const std::wstring& url)
{
    for (auto& b : m_bookmarks) if (b.url == url) return true;
    return false;
}

// ---- ?????? ----
void MainPage::UpdateNavButtons()
{
    BackBtn->IsEnabled = (m_navIndex > 0);
    FwdBtn->IsEnabled = (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1);
}
void MainPage::SetLoading(bool loading)
{
    m_loading = loading;
    Progress->IsIndeterminate = loading;
    Progress->Visibility = loading ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    UpdateUrlActionGlyph();   // ??????????????? ??? ?????? / ????????? ??? ??? ???
}

// Apotheosis: replay a navigation that arrived while the engine was busy.
//
// A single polling timer, rather than a hook in every place that clears m_loading / m_interacting:
// those exits are spread over OnNavDone, the resize completion and the load watchdog, and missing
// one would strand the parked request -- which is the exact failure this whole mechanism exists to
// fix. Checking two booleans every 150 ms costs nothing and covers all of them, including the case
// where the engine never comes back at all: the request then stays parked and visible in the log
// instead of disappearing without trace.
void MainPage::ScheduleNavRetry()
{
    if (!m_navRetry) {
        m_navRetry = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan ts; ts.Duration = 1500000LL;   // 150 ms in 100ns units
        m_navRetry->Interval = ts;
        Platform::Agile<MainPage^> self(this);
        m_navRetry->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [self](Platform::Object^, Platform::Object^) {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_pendNavUrl.empty()) {
                    if (s->m_navRetry) s->m_navRetry->Stop();
                    return;
                }
                if (s->m_loading || s->m_interacting) return;   // still owned -- try again next tick
                std::wstring wurl = s->m_pendNavUrl;
                const bool push = s->m_pendNavPush;
                s->m_pendNavUrl.clear();
                if (s->m_navRetry) s->m_navRetry->Stop();
                LogWriteF("NavRetry: replaying parked navigation %s", WideToUtf8(wurl).c_str());
                s->NavigateTo(ref new String(wurl.c_str()), push);
            });
    }
    m_navRetry->Start();
}

void MainPage::NavigateTo(Platform::String^ url, bool pushHistory)
{
    std::wstring wurl = url ? std::wstring(url->Data()) : L"about:home";
    const bool isHome = wurl.empty() || wurl == L"about:home";
    LogWriteF("NavigateTo: url=%s pushHistory=%d loading=%d", WideToUtf8(wurl).c_str(), pushHistory ? 1 : 0, m_loading ? 1 : 0);
    // Apotheosis: sample the app memory budget at navigation time. MemoryManager's AppMemoryUsage
    // is the figure the OS kills for; on a 3 GB Lumia the limit sits near 850 MB and the load of a
    // heavy page can eat 200+ MB. One line per navigation marks the starting point for the timeline
    // that the latediag and WATCHDOG lines continue.
    LogWriteF("NavigateTo %s", MemoryLine().c_str());
    // Apotheosis: close the suggestion panel here, at the top, and not further down where it used to
    // live. Below this point the function can park the request and return early (engine owned by a
    // load or an interaction), and every such early return left the panel on screen -- up to eight
    // rounded rows carrying titles and URLs from history, stacked over the page content and hiding
    // whatever the site had put there. It looked like the rows were accumulating; in fact the panel
    // was simply never closed, and the list grew as history did. Navigation intent is enough reason to
    // dismiss it, parked or not.
    HideSuggestions();
    // Apotheosis: announce the navigation before anything else, so a resize already sitting in the
    // queue sees it and steps aside instead of spending seconds on a viewport that is about to be
    // relaid out by the load anyway.
    g_navGen.fetch_add(1, std::memory_order_acq_rel);
    // A load or an interaction owns the engine at a size and buffer of its own and has to be left
    // to finish. This used to be a bare `return`, which threw the user's request away: an Enter or
    // a link tap during a slow load simply did nothing, and once a resize stopped returning at all
    // (which happens -- see WebCoreGpuResize), every subsequent attempt did nothing too, with
    // "Load Timeout" as the only feedback. Park the request and replay it when the engine frees up.
    if (m_loading || m_interacting) {
        // Apotheosis: a repeat of the load already running is not a navigation, it is the same user
        // action arriving twice, and it must be dropped rather than parked.
        //
        // Measured on the device 2026-08-21: one tap on the address bar's action button produced two
        // NavigateTo calls for https://ya.ru 28 ms apart, both with pushHistory=1. The first started
        // the load; the second saw m_loading and parked itself, and ScheduleNavRetry then replayed it
        // once the engine freed up -- so every navigation from the address bar fetched and rendered
        // the page twice. On the bench that is merely wasteful. On the phone the replayed load is the
        // one that kills the process, because by the time it runs WebCoreGpuInit has succeeded and it
        // becomes the first session ever built with accelerated compositing enabled. See
        // Doc/PUMPLOOP-SILENT-DEATH.md.
        //
        // The test is deliberately narrow: same URL *and* a load in progress. Parking exists for a
        // request aimed somewhere else during a slow load, and that still works. A user who really
        // wants the current page again has the reload button, which goes through Reload() and not
        // through here. Home is excluded because about:home is rendered, not loaded.
        if (m_loading && !isHome && wurl == m_currentUrl) {
            LogWriteF("NavigateTo: duplicate of the load in progress -> dropped %s",
                WideToUtf8(wurl).c_str());
            // Name the caller instead of guessing it. The harness logs its own module base at
            // startup, so these addresses resolve offline against Src\harness\ARM\Release\Harness\
            // Harness.pdb -- no debugger on the device required.
            void* bt[16];
            USHORT frames = CaptureStackBackTrace(0, 16, bt, nullptr);
            for (USHORT i = 0; i < frames; ++i)
                LogWriteF("NavigateTo: dup caller #%u: %p", i, bt[i]);
            return;
        }
        m_pendNavUrl = wurl;
        m_pendNavPush = pushHistory;
        LogWriteF("NavigateTo: engine owned (loading=%d interacting=%d busy=%d) -> parked %s",
            m_loading ? 1 : 0, m_interacting ? 1 : 0, WebEngine::instance().busy() ? 1 : 0,
            WideToUtf8(wurl).c_str());
        ScheduleNavRetry();
        return;
    }
    m_pendNavUrl.clear();
    m_currentUrl = isHome ? L"about:home" : wurl;

    // M4:??????=?????????,?????? pageScaleFactor ?????? 1.0 ??? harness ????????????/????????????????????????(???????????????????????????)???
    m_pinching = false; m_liveScale = 1.0f; m_pageScale = 1.0f;
    if (GpuPanel) GpuPanel->RenderTransform = nullptr;
    if (RenderImage) RenderImage->RenderTransform = nullptr;

    if (pushHistory) {
        if (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1)
            m_navStack.erase(m_navStack.begin() + m_navIndex + 1, m_navStack.end());
        m_navStack.push_back(wurl);
        m_navIndex = (int)m_navStack.size() - 1;
    }
    UpdateNavButtons();
    UpdateLockIcon();
    m_urlSyncing = true;
    UrlBox->Text = isHome ? ref new String(L"") : url;
    m_urlSyncing = false;
    TitleText->Text = isHome ? ref new String(GetStr(m_uiLang, S_HOME_TITLE).c_str()) : ref new String((GetStr(m_uiLang, S_LOADING) + L"  " + wurl).c_str());
    SetLoading(true);
    // Apotheosis (2026-09-18): a new navigation invalidates the "empty page" notice -- it belongs to
    // the previous result and must not stay on screen while the next page loads.
    HideEmptyPageNotice();

    // ???????????????:????????????????????? dispatcher ??????/??????????????????,40s ?????????????????? m_loading,
    // ????????????????????????(??????????????????????????? hang)???????????? 30s ??????????????? job ?????????,UI ??? 40s ?????????
    if (!m_loadWatchdog) {
        m_loadWatchdog = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan ts; ts.Duration = 40LL * 10000000LL;   // 40s(100ns ??????)
        m_loadWatchdog->Interval = ts;
        m_loadWatchdog->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnLoadWatchdog);
    }
    m_loadWatchdog->Start();

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string surl = ToUtf8(url);
    unsigned long long mySeq = ++m_opSeq;
    // ??????:???????????????/??????????????????????????????(????????????=<a>,??????????????????????????????????????????)???
    std::string homeHtml = isHome ? BuildHomeHtml(m_bookmarks, m_historyList) : std::string();

    WebEngine::instance().postFront("nav-load", [disp, self, surl, isHome, homeHtml, mySeq]() {
        WriteStage("WE-job:body-start");
        // Apotheosis: pairs with the tid in the UEF line -- tells at a glance whether a
        // crash landed on the engine thread or on a worker (e.g. a curl thread), which
        // changes the diagnosis completely.
        LogWriteF("WE-job: engine tid=%lu", GetCurrentThreadId());
        g_engineTid = GetCurrentThreadId();   // Apotheosis: published for WriteWedgeDump (see above)
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        bool loadOk = false;   // ???????????????????????????(??????????????????????????????),?????????????????????
        bool sessionActive = false;   // ?????????????????????????????????(??????????????????/????????????)
    // Apotheosis (2026-09-18): set when the load returned rc=0 but the engine produced no content
    // (see DiagLooksEmpty). Drives the in-window notice so an empty page never looks like a crash.
    bool emptyPage = false;
    int emptyBodyKids = 0;
    // Apotheosis (2026-09-18): the URL the engine actually ended up on, filled in by the load branch
    // below. Declared here because the notice is built outside that branch, and a redirect is exactly
    // what makes the two differ. Empty for the built-in home page.
    std::string finalUrl;
        std::wstring title;
        try {
            WriteStage("WE-job:try-enter");
            if (isHome) {
                WriteStage("WE-job:render-home");
                std::string homeHtml2 = homeHtml.empty()
                    ? "<html><body style='background:#eee;padding:40px'><h1>Hello</h1></body></html>"
                    : homeHtml;
                WriteStage(("WE-job:home-size=" + std::to_string(homeHtml2.size())).c_str());
                int rr = WebCoreRenderHtml(homeHtml2.c_str(), kW, kH, rgba->data());
                WriteStage(("WE-job:render-done rc=" + std::to_string(rr)).c_str());
                if (rr == 0) { rc = 0; loadOk = true; title = L"Home"; }
                else { rc = rr; loadOk = false; title = L"Home render error"; }
            } else {
                WriteStage(("before-load " + surl).c_str());
                int netRc = WebCoreSessionLoad(surl.c_str(), kW, kH, rgba->data());   // ??????????????????
                char t[512] = ""; WebCoreGetTitle(t, sizeof t);
                // Apotheosis 2026-09-24: 8192, to match the port's own `g_lastDiag` -- at 4096 this
                // was the second of the two silent cuts on the `res:` list.
                char diag[8192] = ""; WebCoreGetDiag(diag, sizeof diag);
                // Apotheosis (2026-09-18): rc=0 does not mean the user got something to look at.
                // Classify the result here, while the diag string is in hand; the UI thread turns
                // this into a visible notice instead of an empty frame. See DiagLooksEmpty.
                emptyPage = (netRc == 0) && DiagLooksEmpty(diag, emptyBodyKids);
                // Apotheosis (2026-09-18): the notice promises "the URL the engine actually ended up
                // on", and the requested URL is not that. dzen.ru's blank window is not a page that
                // failed to paint -- it is a *redirect* to `sso.dzen.ru/install?uuid=...`, so the URL
                // that explains the blank window is the document's, not the one in the address bar.
                // Read here, on the load's own thread: the engine serializes every C ABI call, and the
                // UI thread must never wait on the engine (CLAUDE.md, Threading).
                char finalUrlBuf[1024] = ""; WebCoreGetUrl(finalUrlBuf, sizeof finalUrlBuf);
                finalUrl = finalUrlBuf;
                char err[512] = ""; WebCoreGetLastError(err, sizeof err);   // curl ?????????+??????(?????????)
                int comp = 0; try { comp = WebCoreEnableCompositing(); } catch (...) {}   // M1 ??????:??????????????????(???????????????)
                // ????????????????????? stage.txt(?????????????????????,?????????)??? ????????????"????????????"?????????
                // Apotheosis: the ERR line is printed only when there is an error. It used to be
                // unconditional, so every clean load left a bare "ERR: " in log.txt -- a line that
                // reads like a failure at a glance and now travels in every mailed diagnostics
                // report, where the reader has no way to know it means nothing.
                std::string stageMsg = "after-load url=" + surl + " rc=" + std::to_string(netRc)
                                     + " compositing=" + std::to_string(comp);
                if (err[0])
                    stageMsg += std::string("\nERR: ") + err;
                stageMsg += std::string("\ndiag: ") + diag;
                WriteStage(stageMsg.c_str());
                // Apotheosis: one explicit, greppable verdict line. Until now the only visible
                // sign that a navigation had succeeded was the ScrollFab appearing -- a poor
                // signal that also covered the page. The log is the right place for it.
                WriteStage((std::string("nav: ") + (netRc == 0 ? "OK" : "FAIL")
                            + " url=" + surl + " rc=" + std::to_string(netRc)
                            + " title=" + t).c_str());
                if (netRc == 0) {
                    rc = 0;
                    loadOk = true;
                    sessionActive = true;
                    title = ToWide(t);
                    if (title.empty()) title = Utf8ToWide(surl);
                } else {
                    std::string eh = MakeErrorHtml(surl, err);
                    rc = WebCoreRenderHtml(eh.c_str(), kW, kH, rgba->data());   // ???????????????(????????????????????????)
                    loadOk = false;
                    title = L"Load failed";
                    // Apotheosis 2026-09-24: say out loud that an error page was produced, and that
                    // its pixels reach the screen only when `rc == 0` -- which is what makes `ok`
                    // below true and the lambda blit. Without this the only evidence was
                    // `WE-job:post-try rc=0`, a line the success path writes too, and the state of
                    // the link table (the page's only affordance) could not be read from a log.
                    LogWriteF("error page: rendered url=%s rc=%d blit=%s",
                              surl.c_str(), rc, (rc == 0 ? "yes" : "no -- old page stays on screen"));
                }
            }
        } catch (...) { WriteStage("WE-job:caught-exception"); rc = -1000; loadOk = false; title = L"Load failed"; }

        WriteStage(("WE-job:post-try rc=" + std::to_string(rc)).c_str());
        // ??????????????????(??????????????????????????? g_links,???????????????????????????)???
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        try {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx = 0, ly = 0, lw = 0, lh = 0; char lu[1200] = "";
                if (WebCoreGetLink(i, &lx, &ly, &lw, &lh, lu, sizeof lu)) {
                    Harness::PageLink pl; pl.x = lx; pl.y = ly; pl.w = lw; pl.h = lh; pl.url = Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        } catch (...) {}

        auto titleCopy = std::make_shared<std::wstring>(title);
        // Apotheosis (2026-09-18): the notice needs the URL that came back empty, and the lambda
        // below is created after `surl` goes out of scope, so carry a copy.
        auto navUrlCopy = std::make_shared<std::wstring>(Utf8ToWide(surl));
        // Apotheosis (2026-09-18): carried alongside the requested URL so the notice can say where
        // the engine actually landed when the two differ -- the redirect is the whole explanation of
        // a blank dzen.ru.
        auto finalUrlCopy = std::make_shared<std::wstring>(Utf8ToWide(finalUrl));
        bool ok = (rc == 0);   // ??????????????????(??????????????????)
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, titleCopy, ok, loadOk, sessionActive, links, mySeq, navUrlCopy, finalUrlCopy, emptyPage, emptyBodyKids]() {
                    MainPage^ s = self.Get();
                    if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // ??????????????????/???????????????,?????????????????????
                    if (ok) {
                        auto wb = ref new WriteableBitmap(kW, kH);
                        BlitToBitmap(wb, *rgba, kW, kH);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                        s->m_pageLinks = *links;   // ????????????????????????????????????
                    }
                    s->m_sessionActive = sessionActive;
                    // Apotheosis (2026-09-18): a load that returned rc=0 but painted nothing gets an
                    // explanatory notice instead of an empty window; any page that painted clears it.
                    if (ok && emptyPage)
                        s->ShowEmptyPageNotice(*finalUrlCopy, *navUrlCopy, emptyBodyKids);
                    else
                        s->HideEmptyPageNotice();
                    // Apotheosis: the ScrollFab (the two large round scroll arrows) stays hidden
                    // now. It was a phone-only affordance for scrolling without a scrollbar, but
                    // the engine already receives real pan/pinch gestures from any touch screen,
                    // so on a tablet or desktop the buttons only obscured the page -- and their
                    // appearance was doubling as the "navigation succeeded" indicator, which now
                    // lives in the log as an explicit `nav: OK|FAIL` line. Step one of removing
                    // the control: stop showing it. The XAML element and the Connect() binding
                    // stay for now, because deleting an x:Name'd element means re-syncing the
                    // hand-pasted Connect() from Generated Files/MainPage.g.hpp.
                    s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                    // ????????????:??????????????????(???????????????SPA ????????????);?????????(??????/?????????)??????
                    s->m_lastFrameHash = 0;
                    if (sessionActive) s->StartLiveMode(); else s->StopLiveMode();
                    s->OnNavDone(ref new String(titleCopy->c_str()), ok, loadOk,
                                 ref new String(finalUrlCopy->c_str()));
                    // Apotheosis: keep sampling the diag line for a while after the load returns.
                    // Every diag snapshot in the log until now was taken either right after
                    // WebCoreSessionLoad returned or right after the viewport resize -- and both of
                    // those happen while the loader is still working. On hh.ru that snapshot reads
                    // pending=31 rs=I, while port-trace.txt shows the loader going on to drain to
                    // inflight=0 pending=0 with nobody looking. So the long-standing claim that
                    // hh.ru "never reaches readyState Complete" had never actually been measured:
                    // the measuring stopped in the middle of the load. Five samples five seconds
                    // apart cover the tail of a heavy page; they read the diag on the engine thread,
                    // as every C ABI call must, and cost nothing on a page that finishes quickly.
                    if (loadOk) {
                        auto probe = ref new Windows::UI::Xaml::DispatcherTimer();
                        Windows::Foundation::TimeSpan pts; pts.Duration = 50000000LL;   // 5 s
                        probe->Interval = pts;
                        auto tries = std::make_shared<int>(0);
                        probe->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
                            [probe, self, tries](Platform::Object^, Platform::Object^) {
                                MainPage^ p = self.Get();
                                ++(*tries);
                                if (!p || !p->m_sessionActive || *tries > 5) { probe->Stop(); return; }
                                const int n = *tries;
                                WebEngine::instance().post("latediag", [n]() {
                                    // Apotheosis 2026-09-24: 8192, matching the port's `g_lastDiag`.
                                    // These five post-load samples are the only LIVE diag readings the
                                    // harness takes -- every other one is a snapshot from inside the
                                    // load, while the loader is still working -- so this is the line a
                                    // "is it actually stuck" question has to be answered from. It was
                                    // being cut at 4096 here and again at 2048 in LogWriteF.
                                    char d[8192] = "";
                                    try { WebCoreGetDiag(d, sizeof d); } catch (...) {}
                                    LogWriteF("latediag[%d/5] mem=%s diag=%s", n, MemoryLine().c_str(), d);
                                });
                            });
                        probe->Start();
                    }
                }));
        } catch (...) {
            // RunAsync ??????(dispatcher ??????/?????????):OnNavDone ?????????,m_loading ??? UI ??????????????????
            WriteStage("WE-job:runasync-failed");
        }
        WriteStage("WE-job:lambda-done");
    });
}

// Apotheosis (2026-09-18): the "empty page" notice.
//
// Why this exists: a load can return rc=0 and still paint nothing. dzen.ru is the case that produced
// the report -- the site redirects to `sso.dzen.ru/install?uuid=...`, whose body is empty, so the
// window stays white indefinitely. A user cannot tell that apart from a crash, and "the browser
// broke" is the only thing the log is not able to answer. This panel states what happened, shows the
// URL the engine actually ended up on, and offers a way out.
//
// Built in code rather than XAML on purpose: adding an x:Name'd element would mean re-syncing the
// generated Connect() and putting verify-xaml-connect.ps1 in the loop for a cosmetic panel.
//
// Text is deliberately English-only: it is a diagnostic surface, and adding new S_* ids to the
// three-language string table is churn this does not need yet.
void MainPage::ShowEmptyPageNotice(const std::wstring& url, const std::wstring& requested, int bodyKids)
{
    try {
        if (!m_emptyOverlay) {
            Platform::Agile<MainPage^> self(this);

            auto head = ref new TextBlock();
            head->Text = L"The server returned an empty page";
            head->FontSize = 20;
            head->TextWrapping = Windows::UI::Xaml::TextWrapping::Wrap;
            head->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);

            m_emptyText = ref new TextBlock();
            m_emptyText->FontSize = 13;
            m_emptyText->TextWrapping = Windows::UI::Xaml::TextWrapping::Wrap;
            m_emptyText->Margin = Thickness(0, 8, 0, 14);
            m_emptyText->Foreground = ref new SolidColorBrush(Windows::UI::Colors::LightGray);

            // One handler for both buttons: "desktop" only decides whether the live UA switch runs
            // first. Switching UA is the same code path the settings toggle uses -- settings.ini is
            // read at startup, so writing the file would do nothing to the running session.
            //
            // Both buttons retry the address the user ASKED for, not the one the engine landed on.
            // When a site has redirected into a dead end (dzen.ru -> sso.dzen.ru/install, which is
            // empty), reloading where you ended up re-fetches the dead end and can only produce the
            // same blank window -- so the only useful retry is the original address. The two are the
            // same string in the ordinary "this page really is empty" case, so nothing changes there.
            const std::wstring retryUrl = requested.empty() ? url : requested;
            auto makeButton = [self, retryUrl](const wchar_t* label, bool toDesktop) {
                auto button = ref new Button();
                button->Content = ref new String(label);
                button->Margin = Thickness(0, 0, 8, 0);
                button->Click += ref new RoutedEventHandler(
                    [self, toDesktop, retryUrl](Platform::Object^, RoutedEventArgs^) {
                        MainPage^ s = self.Get();
                        if (!s) return;
                        if (toDesktop) {
                            s->m_uaMobile = false;
                            WebEngine::instance().post("set-ua-mobile", []() {
                                try { WebCoreSetUserAgentMobile(0); } catch (...) {}
                            });
                            LogWriteF("empty page: retrying %s with desktop UA", WideToUtf8(retryUrl).c_str());
                        } else {
                            LogWriteF("empty page: reload requested for %s", WideToUtf8(retryUrl).c_str());
                        }
                        s->HideEmptyPageNotice();
                        s->NavigateTo(ref new String(retryUrl.c_str()), true);
                    });
                return button;
            };

            auto row = ref new StackPanel();
            row->Orientation = Orientation::Horizontal;
            row->Children->Append(makeButton(L"Reload", false));
            row->Children->Append(makeButton(L"Try desktop mode", true));

            auto stack = ref new StackPanel();
            stack->MaxWidth = 520;
            stack->Children->Append(head);
            stack->Children->Append(m_emptyText);
            stack->Children->Append(row);

            auto border = ref new Border();
            border->Background = ref new SolidColorBrush(ColorHelper::FromArgb(0xF2, 0x1E, 0x1E, 0x20));
            border->BorderBrush = ref new SolidColorBrush(ColorHelper::FromArgb(0x44, 0xFF, 0xFF, 0xFF));
            border->BorderThickness = Thickness(1);
            // Apotheosis: Metro design has no rounded chrome -- square corners are native to Lumia
            // (WinRT <= 5.0 has no Button.CornerRadius; Border keeps the default square shape).
            // The struct initialises to zeros, so this line only documents intent.
            border->Padding = Thickness(18);
            border->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
            border->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
            border->Child = stack;
            m_emptyOverlay = border;
            // Appended last, so it draws above the page image.
            if (ContentArea) ContentArea->Children->Append(border);
        }

        if (m_emptyText) {
            // Apotheosis (2026-09-18): `url` is the document the engine ended up on, `requested` is
            // what was asked for. dzen.ru is exactly the case where they differ and the difference is
            // the diagnosis -- the user typed one host and the blank window belongs to another -- so
            // the redirect is shown outright rather than left in the log. Both are clamped before
            // formatting: a query-string URL is unbounded and `detail` is a fixed buffer.
            auto clamp = [](const std::wstring& s) {
                return s.size() > 200 ? s.substr(0, 197) + L"..." : s;
            };
            const std::wstring shown = clamp(url);
            const std::wstring asked = clamp(requested);
            wchar_t detail[600];
            if (!asked.empty() && asked != shown) {
                swprintf_s(detail,
                    L"%s\r\n\r\nThat is not the address you asked for (%s) -- the site redirected.\r\n"
                    L"body elements: %d | painted pixels: 0 | UA: %s\r\n"
                    L"The site may be redirecting to a login page, or it may not support this browser.",
                    shown.c_str(), asked.c_str(), bodyKids, m_uaMobile ? L"mobile" : L"desktop");
            } else {
                swprintf_s(detail,
                    L"%s\r\nbody elements: %d | painted pixels: 0 | UA: %s\r\n"
                    L"The site may be redirecting to a login page, or it may not support this browser.",
                    shown.c_str(), bodyKids, m_uaMobile ? L"mobile" : L"desktop");
            }
            m_emptyText->Text = ref new String(detail);
        }
        m_emptyOverlay->Visibility = Windows::UI::Xaml::Visibility::Visible;
        if (!m_emptyNoticeShown) {
            m_emptyNoticeShown = true;
            LogWriteF("empty page: notice shown url=%s requested=%s bodyKids=%d ua=%s",
                      WideToUtf8(url).c_str(), WideToUtf8(requested).c_str(), bodyKids,
                      m_uaMobile ? "mobile" : "desktop");
        }
    } catch (...) {
        LogWrite("empty page: notice construction failed");
    }
}

void MainPage::HideEmptyPageNotice()
{
    if (m_emptyOverlay && m_emptyOverlay->Visibility != Windows::UI::Xaml::Visibility::Collapsed) {
        m_emptyOverlay->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        LogWrite("empty page: notice hidden");
    }
    m_emptyNoticeShown = false;
}

void MainPage::OnNavDone(Platform::String^ finalTitle, bool ok, bool loadOk, Platform::String^ documentUrl)
{
    // Hide startup overlay once any navigation completes
    try { if (m_startupOverlay) m_startupOverlay->Visibility = Windows::UI::Xaml::Visibility::Collapsed; } catch (...) {}
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    m_currentTitle = finalTitle ? std::wstring(finalTitle->Data()) : L"";
    TitleText->Text = (m_currentTitle.empty() ? ref new String(L"EdgeHTML Reborn") : finalTitle);
    if (loadOk && m_currentUrl != L"about:home")   // ?????????????????????????????????,???????????????
        AddHistory(m_currentUrl, m_currentTitle);
    if (m_currentUrl != L"about:home") {
        m_urlSyncing = true;
        UrlBox->Text = ref new String(m_currentUrl.c_str());
        m_urlSyncing = false;
    }
    UpdateLockIcon();
    SetLoading(false);
    UpdateNavButtons();
    // ????????????????????????:???????????????????????????????????????(?????? CA blob ?????????,WebCoreDownload ????????? TLS);
    //   ??????????????????,?????????/???????????????manual ???????????????????????????
    if (!m_updateAutoChecked && loadOk && m_currentUrl != L"about:home") {
        m_updateAutoChecked = true;
        // Apotheosis: TEMPORARY diagnostic. The auto update-check spawns a raw std::thread right
    // after the first successful navigation -- exactly when the "instant silent death" strikes
    // (~+2-3 s of content). Any uncaught exception on that thread means std::terminate -> abort,
    // which on this kernel leaves no UEF, no dump, nothing -- precisely the observed signature.
    // Disabled until the thread is proven innocent or made exception-proof.
    // CheckForUpdate(false);
    }
    // ?????? GPU:???????????????????????????????????? ??? ????????? GPU(EnableGpu ?????????????????????,?????????????????? OnNavDone)???
    // Apotheosis 2026-08-29: the auto-probe is retried on each NEW url, not once per process.
    //
    // m_gpuAutoTried used to be set once and never cleared, so the answer to "can this build present
    // through the GPU" was decided by whichever page happened to load first -- and that answer was
    // always no, because a page without compositing layers has no root layer to present. Measured on
    // x64: the probe reported EnableCompositing=0 Composite=-4 on the home page, latched
    // m_gpuPresent=false, and no later navigation ever asked again. So the GPU path was unreachable in
    // practice on both lines, for a reason that had nothing to do with the GPU.
    //
    // Whether a root layer exists is a property of the PAGE, not of the build, so the question has to
    // be re-asked when the page changes. Retrying per url keeps that cheap: WebCoreGpuInit itself is
    // idempotent (`if (g_gpuActive) return kOK;`), so a repeat probe costs one composite attempt, and
    // the retry only happens while the GPU is still off.
    //
    // Apotheosis 2026-09-19: the key used to be `m_currentUrl` -- the url the HARNESS ASKED FOR -- and
    // that made the retry unreachable exactly where it mattered. Measured on the bench the same day,
    // one session, two loads of `https://dzen.ru/` eight minutes apart that produced two completely
    // different documents:
    //
    //   05:39:32  after-load url=https://dzen.ru/ rc=0 compositing=0
    //             diag: url=https://sso.dzen.ru/install?uuid=... body=0 nonwhite=0/1036944
    //             EnableGpu: first frame after init: EnableCompositing=0 Composite=-4   <- honest: no root layer
    //   05:42:44  after-load url=https://dzen.ru/ rc=0 compositing=1
    //             diag: url=https://dzen.ru/ nonwhite=547064/1036944                     <- a real page
    //             (no EnableGpu line at all)
    //
    // The first load took the Yandex SSO path and ended on a document with no compositing tree, so the
    // probe correctly reported failure and latched `m_gpuTriedForUrl="https://dzen.ru/"`. The second
    // load was the real dzen.ru with a root layer to present -- and was never probed, because both
    // loads share their REQUESTED url. Since a retry only happens while the GPU is still off, that one
    // honest failure pinned every later dzen.ru load to software present, and no sequence of real pages
    // could reach the GPU path on the bench.
    //
    // So the key is the document's url, not the request's. It is read from the engine on the engine
    // thread (`finalUrl`, next to the same WebCoreGetUrl call that feeds the empty-page notice) and
    // arrives here already marshalled, so this costs no extra ABI call and no cross-thread read of a
    // live field -- the UI thread must never wait on the engine (CLAUDE.md, Threading).
    const std::wstring docUrl = documentUrl ? std::wstring(documentUrl->Data()) : std::wstring();
    const bool newDocument = !docUrl.empty() && m_gpuTriedForUrl != docUrl;
    if (m_gpuDefault && m_sessionActive && m_currentUrl != L"about:home" && newDocument) {
        m_gpuTriedForUrl = docUrl;
        if (!m_gpuOn) {
            LogWriteF("EnableGpu: probing for document %s (requested %s)",
                      WideToUtf8(docUrl).c_str(), WideToUtf8(m_currentUrl).c_str());
            EnableGpu();
            return;   // ????????????????????? OnNavDone ??????,??????????????????
        }
        if (m_gpuPresent) {
            // GPU already on: this document may still be one the GPU cannot present, so re-judge it.
            // Deliberately NOT an early return -- the zoom and resize work below must still run on a
            // navigation that changes neither.
            LogWriteF("EnableGpu: re-checking document %s (requested %s)",
                      WideToUtf8(docUrl).c_str(), WideToUtf8(m_currentUrl).c_str());
            ReevaluateGpuForDocument();
        }
    }
    // A skipped probe must not be silent: silence is what hid the defect above for a whole session.
    // Reachable when the same document is loaded again while the GPU is still off -- i.e. the probe
    // already ran for exactly this document and its answer still stands.
    if (m_gpuDefault && !m_gpuOn && m_sessionActive && !docUrl.empty() && m_currentUrl != L"about:home"
        && m_gpuTriedForUrl == docUrl) {
        LogWriteF("EnableGpu: probe skipped, already tried for document %s", WideToUtf8(docUrl).c_str());
    }
    // ????????????:????????????????????? 100% ???,?????????????????????(????????????????????????)???
    if (m_sessionActive && m_defaultZoom != 100)
        PinchCommit(m_defaultZoom / 100.0f, kW / 2, kH / 2);
    // Apotheosis: a window resize during the load was parked rather than applied (the load owned
    // the engine at the old size); the engine is idle again now, so let the final size win.
    if (m_pendW && (m_pendW != kW || m_pendH != kH))
        ApplyViewportSize();
    (void)ok;
}

void MainPage::OnHeartbeat(Platform::Object^, Platform::Object^)
{
    // Apotheosis: the beat now describes the ENGINE thread, from the UI thread, every 2 s.
    //
    // Why this exists. Three crashes on the device left a trace that simply stopped, and the natural
    // reading -- "the process died here" -- was wrong. Comparing the last trace line against this
    // file's own timestamp showed the process living another 4 to 7 seconds afterwards, with the UI
    // timer beating the whole time. So the engine thread stopped answering first and something killed
    // the app later; an unhandled exception cannot do that, because it would take the process down at
    // once. What this line adds is *which* state the engine was in when it went quiet:
    //
    //   busy=1, finished not advancing  -> stuck INSIDE a job, and stage= names which one
    //   busy=0, pending>0               -> the thread is gone while work piles up behind it
    //   busy=0, pending=0               -> idle and nothing to be stuck on; not a hang
    //
    // Deliberately no job is posted to the engine to prove it is alive, tempting as that was. The
    // live-mode tick skips itself while `pending() > 0` (see the queue-backlog comment there), so a
    // probe that queued one beat every 2 s would suppress a real code path -- a probe that alters what
    // it measures. All three counters are already published as atomics and cost nothing to read.
    //
    // The existing WATCHDOG line reports the same numbers but returns early unless m_loading or
    // m_interacting is set, and by the time the engine goes quiet the load is over and both are
    // false. That is precisely why the window we care about was blank.
    WebEngine& eng = WebEngine::instance();
    // Apotheosis: tickstep is meaningful only while the engine is inside the live-tick job; for any
    // other job it is whatever the previous tick left behind (10, "returned"). Read unconditionally
    // anyway -- it is a plain integer load with no lock and no engine involvement, so it cannot block
    // even when the engine thread is wedged, which is the one moment this line has to keep working.
    int tickStep = -1;
    try { tickStep = WebCoreGetLiveTickStep(); } catch (...) {}
    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "%s beat busy=%d pending=%u finished=%llu job=%s tickstep=%d stage=%s\n",
        LogTimestamp().c_str(),
        eng.busy() ? 1 : 0,
        (unsigned)eng.pending(),
        (unsigned long long)eng.finished(),
        eng.currentJob(),
        tickStep,
        g_lastStage);
    WriteMarkerFile(L"heartbeat.txt", line);

    // Apotheosis: wedge snapshot. busy=1 with an unchanged finished counter means the engine
    // thread made no progress across a whole beat; the platform kills the app seconds after that
    // starts, so the first such beat may be the only chance. Suspend the engine thread, capture
    // PC/LR/SP plus its stack words, resume, write wedgedump.txt -- all from the UI thread, which
    // is provably alive at this moment (it is executing this timer tick). One dump per WEDGE, not
    // per process: repeated snapshots while nothing changes would only rewrite the same frame, so
    // the flag is cleared as soon as progress resumes and the next wedge gets its own dump.
    static unsigned long long s_lastFinished = (unsigned long long)-1;
    static int s_stuckBeats = 0;
    static bool s_wedgeDumped = false;
    static long long s_lastFetchProg = -1;
    static long long s_lastEngineActivity = -1;
    static int s_gaugeSavedBeats = 0;
    unsigned long long nowFinished = eng.finished();
    // Apotheosis 2026-09-18: while a top-level document fetch is in flight the engine thread is
    // *supposed* to be blocked, so `finished` standing still says nothing. Ask how far the transfer has
    // got instead.
    // Apotheosis 2026-09-19: **the sign is the answer, and it is the only thing this export promises.**
    // -1 means "no top-level fetch is in flight" -- the engine thread is free, so a stalled `finished`
    // counter is a real stall. Any non-negative value means a fetch IS in flight: the engine is
    // legitimately parked in the driver's wait loop and how long it may stay parked is bounded by the
    // port (CONNECTTIMEOUT 3 s / TIMEOUT 8 s / LOW_SPEED, and a 9.5 s ceiling), NOT by our beat count.
    // During the pre-body phase (connect, TLS, TTFB, a redirect chain) the value is a legitimate 0.
    // The value is not a byte count: it is "body bytes with completed redirect hops folded in above
    // them", compared only against its own previous reading, printed as `fetchprog=`.
    long long nowFetchProg = WebCoreGetFetchProgress();
    const bool fetchInFlight = (nowFetchProg >= 0);
    const bool fetchMoving = fetchInFlight && (nowFetchProg != s_lastFetchProg);
    s_lastFetchProg = nowFetchProg;
    // Apotheosis 2026-09-19: and ask the engine whether the engine is working, instead of inferring it
    // from a job counter. `finished` counts COMPLETED jobs, so it cannot move while the one job the
    // watchdog is worried about is still running -- and one `nav-load` job legitimately runs for ~7 s on
    // a 3.6 MB page (parse + inline JS + first paint; measured on a cold dzen.ru). The port now publishes
    // a monotonic count of the work it has visibly done (load-job stage boundaries, each DocumentWriter
    // chunk, every settle tick of the load pump, the live tick's step), so the counter moves exactly when
    // the engine does. Compared against its own previous reading, like fetchprog; `engineact=` in the log.
    long long nowActivity = WebCoreGetEngineActivity();
    const bool engineMoving = (nowActivity != s_lastEngineActivity);
    s_lastEngineActivity = nowActivity;
    // Apotheosis: two stuck shapes now trigger the snapshot. busy=1 without progress is the
    // classic wedge; but the .57-era deaths showed a second shape -- busy=0 pending=0 with the
    // UI still waiting for a navigation to finish, i.e. the engine thread went silent BETWEEN
    // jobs (or died). While m_loading is true the engine is expected to be making progress on
    // our behalf, so an unchanged finished counter there is just as abnormal.
    bool noProgress = (nowFinished == s_lastFinished) && !fetchMoving && !engineMoving;
    // Apotheosis 2026-09-19: name the beats that ONLY the gauge saved. Suppressing a false WEDGE
    // silently is its own defect: the run that used to end in a stack dump now ends in nothing, and
    // "nothing" cannot be told from "the old defect is still there but the timing changed". One line per
    // six such beats records the mechanism -- and it is the line that will appear on the phone, where
    // the gauge is the only channel that can say "still working".
    const bool jobFrozen = (nowFinished == s_lastFinished);
    if (jobFrozen && !fetchMoving && engineMoving)
        ++s_gaugeSavedBeats;
    else if (!jobFrozen || fetchMoving)
        s_gaugeSavedBeats = 0;
    if (s_gaugeSavedBeats >= 6) {
        s_gaugeSavedBeats = 0;
        LogWriteF("beat-gauge: job counter frozen for 6+ beats but engine activity moved (job=%s engineact=%lld) -- NOT a wedge, no dump",
            eng.currentJob(), nowActivity);
    }
    // Apotheosis 2026-09-18: progress resumed, so a wedge that was already dumped is over and the
    // next one is a NEW event. This reset closes the hole that cost the dzen.ru autopsy: the
    // one-shot flag below made the SECOND wedge in a session invisible. Measured that day -- a
    // benign wedge at 13:45 (finished=220) consumed the only dump, the fatal one at 13:48
    // (finished=940) left nothing at all, and the death read as unexplained. Clearing on recovery
    // keeps the flag's actual purpose (repeated beats of the SAME wedge must not rewrite the same
    // frame) and removes the hole.
    if (!noProgress) {
        s_stuckBeats = 0;
        s_wedgeDumped = false;
    } else if (!s_wedgeDumped && (eng.busy() || m_loading)) {
        ++s_stuckBeats;
        // Apotheosis 2026-09-18: one line per stuck beat, into log.txt. LogWrite opens and closes
        // the stream per line, so this lands as it happens. heartbeat.txt only ever holds the LAST
        // beat, which cannot distinguish "the UI thread was still beating when the process died"
        // from "the UI thread itself stopped first" -- and that is exactly the question that decides
        // whether to hunt an engine wedge or a UI-thread death.
        LogWriteF("beat-stuck #%d busy=%d pending=%u finished=%llu job=%s loading=%d inflight=%d fetchprog=%lld engineact=%lld",
            s_stuckBeats, eng.busy() ? 1 : 0, (unsigned)eng.pending(),
            (unsigned long long)nowFinished, eng.currentJob(), m_loading ? 1 : 0,
            fetchInFlight ? 1 : 0, nowFetchProg, nowActivity);
        unsigned long tid = g_engineTid;
        // Apotheosis 2026-09-18: this threshold used to be calibrated against a NUMBER -- "6 x 700 ms
        // = 4.2 s, comfortably past the 2.5 s fetch ceiling" (Src/port/WebCoreDriver.cpp waited a fixed
        // 2500 ms for the background document fetch). That ceiling is gone: the wait now re-arms for as
        // long as bytes keep arriving, up to a 9.5 s ceiling, so a perfectly healthy 3 MB page blocks
        // the engine thread for ~4.4 s. Measured that day, the first slow fetch after the change
        // produced a WEDGE line and a full stack dump for a page that was loading correctly.
        //
        // The threshold no longer has to clear a known duration, because the question is asked
        // directly: the engine's own activity gauge and the transfer's byte/hop counter (above) both
        // count as progress even while `finished` stands still, so s_stuckBeats only accumulates over
        // beats where NOTHING moved at all.
        //
        // Apotheosis 2026-09-19: **the residual window is per-job, so the number is per-job too.** The
        // gauge cannot cover one case: a single engine operation that takes longer than the threshold
        // while publishing nothing in between. Those operations are bounded, and each bound is the
        // port's own, so the number is derived rather than guessed -- and the measured failing cases
        // (04:35:35 and 04:50:09, both healthy dzen.ru loads dumped as WEDGEs) sit inside them:
        //   * a top-level fetch in flight: the engine thread is *supposed* to be blocked, and the port
        //     ends the wait at a 9.5 s ceiling -> 16 beats (~11.2 s).
        //   * a `nav-load` job: the ceiling above, plus pumpLoop's own 5 s watchdog, plus the tail
        //     (layout, link extraction, first paint) -> 30 beats (~21 s). A 3.6 MB page measured 6.9 s
        //     end to end on this bench, so this is the same order with room for the phone's slower CPU.
        //     The tail is not itself bounded by a number in the port, which is why the gauge matters more
        //     here than the threshold does.
        //   * anything else (interactive jobs, the live tick): six, unchanged. The live tick is a
        //     sequence of operations whose step index advances the gauge, so six beats of silence there
        //     means a SINGLE step lasted ~4.2 s -- which is a real finding and the shape the device's
        //     2026-08-22 hang had. Measured: a live tick on a loaded dzen.ru page can hold the engine for
        //     ~2.1 s, which the gauge absorbs (it never got past 2 beats in the 2026-09-19 runs).
        //
        // The number six is kept for the no-progress-with-no-fetch case only, for the two reasons it was
        // picked for in 2026-09-03: one beat fired on a healthy fetch (the engine parked in
        // _Cnd_timedwait_for_impl, mid-fetch, bounded -- a false positive that cost an hour of reading it
        // as a hang), and six lands inside the few seconds the platform allows before it kills a frozen
        // app, so the dump happens while there is still a process to dump. That last reason is about the
        // UI thread, which is alive and beating in every case here (it is executing this tick), so it
        // does not bind the per-job numbers above -- but it is why six is not simply raised everywhere.
        const char* jobName = eng.currentJob();
        const int wedgedBeats = fetchInFlight ? 16
            : ((jobName && std::strcmp(jobName, "nav-load") == 0) ? 30 : 6);
        if (tid && s_stuckBeats >= wedgedBeats) {
            s_wedgeDumped = true;
            LogWriteF("WEDGE: no progress for %d beats, needed=%d inflight=%d job=%s (finished=%llu busy=%d loading=%d fetchprog=%lld engineact=%lld) -- dumping engine tid=%lu",
                s_stuckBeats, wedgedBeats, fetchInFlight ? 1 : 0, jobName ? jobName : "?",
                (unsigned long long)nowFinished, eng.busy() ? 1 : 0, m_loading ? 1 : 0,
                nowFetchProg, nowActivity, tid);
            WriteWedgeDump(tid);
        }
    }
    s_lastFinished = nowFinished;
}

void MainPage::OnLoadWatchdog(Platform::Object^, Platform::Object^)
{
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    if (!(m_loading || m_interacting)) return;

    // Apotheosis: deliberately no WebCoreGetDiag call here. That is a C ABI call and the iron rule
    // is that those run on the one engine thread; calling it from the UI thread while the engine
    // thread was mid-job is why every watchdog dump reported the *pre-resize* geometry and read
    // like a frozen renderer. The engine's own stage markers
    // (LocalState\gpuinit-steps.txt, flushed per line) carry that truth now, and they carry it from
    // inside the job that is actually stuck.
    const bool engineBusy = WebEngine::instance().busy();
    LogWriteF("WATCHDOG: loading=%d interacting=%d engineBusy=%d pending=%u finished=%llu mem=%s",
        m_loading ? 1 : 0, m_interacting ? 1 : 0, engineBusy ? 1 : 0,
        (unsigned)WebEngine::instance().pending(), WebEngine::instance().finished(), MemoryLine().c_str());

    if (engineBusy) {
        // The job is still running, so tearing the UI state down would be a fabrication. Worse,
        // the old code did ++m_opSeq here, which discarded the completion callback of the very job
        // still in flight: kW/kH could then never be adopted, "engine rc=" never appeared, and
        // m_sessionActive=false turned later taps into session=0 no-ops. Leave the bookkeeping
        // intact, say what is actually happening, and re-arm -- if the job does return, its
        // completion still runs and still works.
        TitleText->Text = ref new String(GetStr(m_uiLang, S_ENGINE_BUSY).c_str());
        if (m_loadWatchdog) m_loadWatchdog->Start();
        return;
    }

    // Engine idle but our operation never reported back: a genuinely lost completion, which is the
    // case this watchdog was built for. Reset so navigation cannot stay locked out for good.
    ++m_opSeq;
    m_interacting = false;
    m_sessionActive = false;
    ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    TitleText->Text = ref new String(GetStr(m_uiLang, S_LOAD_TIMEOUT).c_str());
    SetLoading(false);
    UpdateNavButtons();
}

// ============================================================================
// Apotheosis: keep the engine viewport in step with the window.
//
// ContentArea is the render host. In software mode RenderImage sits inside it with Stretch=None
// and Top/Left alignment, so one DIP is one engine pixel and the engine only has to be told to
// paint at ContentArea's size -- which is what the hardcoded 720x1080 viewport never did, hence
// the page laying out in a 720px column with empty grey filling the rest of the window.
//
// GPU present mode is deliberately left out: the ANGLE render surface is sized once, when
// EnableGpu creates it from EGLRenderSurfaceSizeProperty, and resizing it means recreating the
// surface on the panel dispatcher while the engine thread is presenting into it. Until that is
// done properly the GPU path keeps its creation-time size (as it always has).
// ============================================================================
void MainPage::OnContentAreaSizeChanged(Platform::Object^, Windows::UI::Xaml::SizeChangedEventArgs^)
{
    if (!m_resizeDebounce) {
        m_resizeDebounce = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan iv; iv.Duration = 2500000;   // 250 ms in 100ns units
        m_resizeDebounce->Interval = iv;
        Platform::Agile<MainPage^> self(this);
        m_resizeDebounce->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [self](Platform::Object^, Platform::Object^) {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_resizeDebounce) s->m_resizeDebounce->Stop();
                s->ApplyViewportSize();
            });
    }
    // Restarting an already-running DispatcherTimer resets its countdown, so a continuous drag
    // only ever fires once the window stops changing size.
    m_resizeDebounce->Stop();
    m_resizeDebounce->Start();
}

// The one invariant this whole path exists to keep: kW/kH is the size the ENGINE is painting at,
// never merely the size the window wants. Every paint entry point sizes its buffer kW*kH*4 while
// the engine writes g_session->w*h*4, so the moment the two disagree a paint overruns the heap
// (that is a real crash this code caused, not a hypothetical: AV on the engine thread inside the
// first live tick after the window turned out to be 1024x694 and the engine was still at
// 720x1080). So the window's wish is parked in m_pendW/m_pendH and kW/kH is only advanced once
// the engine has accepted the new size -- or while no session owns a viewport at all.
//
// Apotheosis 2026-09-19: GPU mode works in PHYSICAL pixels and software mode in DIPs, and mixing the
// two is what made a real page look magnified and cropped. The panel is a SwapChainPanel whose
// CompositionScale is the display's DPI scale (2.00 on the bench), and ANGLE sizes the swapchain in
// the units handed to it -- so a surface created at the DIP size (1368x758) is displayed across the
// panel's 2736x1516 physical pixels, while the engine, told the same DIP size, lays the page out at
// 1368 CSS px. Measured 0.1.10.24 on dzen.ru/news: `Viewport[gpu-on]: content=1368x758
// panel=1368x758 compScale=2.00x2.00 engine=1368x758`, and the window showed the page's top-left
// quarter enlarged 2x, cut off at the right edge. The engine's own viewport must therefore be the
// surface's physical size; kW/kH follow it, and MapTapToEngine's kW/ActualWidth ratio then converts
// a DIP tap into the physical pixel the engine expects.
//
// Apotheosis 2026-09-19: the display's DPI scale as the panel reports it. Guarded because the
// property is meaningless before the panel has a composition scale (and on a fresh element it reads
// 0), and a zero here would size the surface to nothing.
double MainPage::GpuPixelScaleX()
{
    try { const double s = GpuPanel ? GpuPanel->CompositionScaleX : 0.0; return s > 0.0 ? s : 1.0; }
    catch (...) { return 1.0; }
}
double MainPage::GpuPixelScaleY()
{
    try { const double s = GpuPanel ? GpuPanel->CompositionScaleY : 0.0; return s > 0.0 ? s : 1.0; }
    catch (...) { return 1.0; }
}

void MainPage::LogViewportMetrics(const char* tag)
{
    try {
        char buf[384];
        const double cw = ContentArea ? ContentArea->ActualWidth : 0.0;
        const double ch = ContentArea ? ContentArea->ActualHeight : 0.0;
        const double pw = GpuPanel ? GpuPanel->ActualWidth : 0.0;
        const double ph = GpuPanel ? GpuPanel->ActualHeight : 0.0;
        double sx = 0.0, sy = 0.0;
        if (GpuPanel) { sx = GpuPanel->CompositionScaleX; sy = GpuPanel->CompositionScaleY; }
        sprintf_s(buf, "Viewport[%s]: content=%.0fx%.0f panel=%.0fx%.0f compScale=%.2fx%.2f engine=%dx%d",
                  tag, cw, ch, pw, ph, sx, sy, kW, kH);
        LogWrite(buf);
    } catch (...) {}
}

// Apotheosis 2026-09-19: keep the engine's CSS page zoom in step with how the frame is sized.
//
// GPU present renders into the panel's PHYSICAL pixels, so the engine viewport -- and therefore
// kW/kH, and therefore the layout width -- is `DIP x CompositionScale`. Left alone, the page lays
// out at that width: on the bench that measured `contents=2736x12368` where software mode gave
// `contents=1368x12368` for the same window, i.e. the desktop layout at half size, which is what a
// photograph of dzen.ru/news showed. Zoom is the lever that separates the two: with
// zoom = CompositionScale the layout viewport is viewport/zoom = DIP width, and because zoom
// multiplies every layer's geometry the TextureMapper tiles come out at the physical resolution
// instead of being upscaled. Software present has a DIP-sized viewport and must stay at 1.0.
//
// This posts and returns: WebCoreSetPageZoom re-lays the page out but does not repaint, and both
// callers (EnableGpu arming the GPU, ReevaluateGpuForDocument handing it back) call
// ApplyViewportSize immediately afterwards, which relayouts and paints anyway. Doing it here rather
// than inline in those two places keeps the scale-vs-zoom decision in one place -- the two must
// never disagree, or the page is laid out at one width and drawn at another.
void MainPage::PushPageZoom()
{
    const float z = m_gpuPresent ? static_cast<float>(GpuPixelScaleX()) : 1.0f;
    WebEngine::instance().post("page-zoom", [z]() {
        int rc = -999;
        try { rc = WebCoreSetPageZoom(z); } catch (...) { rc = -1000; }
        if (rc != 0) LogWriteF("PushPageZoom: WebCoreSetPageZoom(%.2f) rc=%d", (double)z, rc);
    });
}

void MainPage::ApplyViewportSize()
{
    int w = static_cast<int>(ContentArea->ActualWidth + 0.5);
    int h = static_cast<int>(ContentArea->ActualHeight + 0.5);
    if (w <= 0 || h <= 0) return;              // no layout pass yet
    if (w > 8192) w = 8192;                    // matches the driver's sanity ceiling
    if (h > 8192) h = 8192;
    // GPU mode is NOT skipped anymore: the ANGLE window surface was created once at the init size
    // (720x1080) and the SwapChainPanel stretches it, so a size change must recreate the surface
    // via WebCoreGpuResize (handled below). kW/kH tracks the engine/surface size in both modes.
    m_pendW = w;
    m_pendH = h;
    // Apotheosis 2026-09-19: compared against the DIP record, not against kW/kH. In GPU mode kW/kH
    // are PHYSICAL pixels (the DIP size times the panel's CompositionScale), so `w == kW` can only
    // hold when the scale is 1.0 -- on the bench it is 2.00, and comparing the two units directly
    // would re-run the whole resize on every layout pass. m_appliedDipW/H is written wherever kW/kH
    // are (both sites), so this test is the old one, in the unit the caller actually supplied.
    if (w == m_appliedDipW && h == m_appliedDipH) return;

    // Apotheosis 2026-09-19: GPU mode renders at PHYSICAL pixels, software mode keeps DIPs. The
    // panel is a SwapChainPanel whose CompositionScale is the display DPI scale (2.00 on the bench),
    // and ANGLE sizes the swapchain in whatever units it is handed -- so a surface created at the DIP
    // size (1368x758) is stretched across the panel's 2736x1516 physical pixels while the engine,
    // told the same DIP size, lays the page out at 1368 CSS px. The result is the page's top-left
    // quarter enlarged 2x and cut off at the right edge; measured 0.1.10.24 on dzen.ru/news as
    // `Viewport[gpu-on]: content=1368x758 panel=1368x758 compScale=2.00x2.00 engine=1368x758`.
    // Everything downstream -- the EGL surface, the engine viewport, the readback buffer, and
    // therefore kW/kH -- takes this one number, and MapTapToEngine's kW/ActualWidth ratio then
    // converts a DIP tap into the physical pixel the engine expects. `GpuPixelScale*()` is 1.0 when
    // the panel has no scale yet, so the software path and a scale-less GPU panel both fall through
    // unchanged.
    int ew = m_gpuPresent ? static_cast<int>(w * GpuPixelScaleX() + 0.5) : w;
    int eh = m_gpuPresent ? static_cast<int>(h * GpuPixelScaleY() + 0.5) : h;
    if (ew > 8192) ew = 8192;
    if (eh > 8192) eh = 8192;

    // A load or an interaction owns the engine and was started at the old size; its buffer, its
    // viewport and its blit all agree on that size and must be left to finish. The pending size is
    // picked up afterwards -- by the resize callback below, or by OnNavDone.
    if (m_loading || m_interacting) return;

    // Apotheosis: a parked navigation outranks this resize. The load relays the page out at
    // m_pendW/m_pendH when it runs, so paying for a relayout plus a full backing-store
    // regeneration here would buy nothing and would delay the navigation queued behind it.
    // ScheduleNavRetry's tick picks the navigation up.
    if (!m_pendNavUrl.empty()) {
        LogWriteF("ApplyViewportSize: %dx%d deferred, navigation parked", w, h);
        return;
    }

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);

    LogWriteF("ApplyViewportSize: %dx%d dip, %dx%d engine (session=%d gpu=%d)",
              w, h, ew, eh, m_sessionActive ? 1 : 0, m_gpuPresent ? 1 : 0);
    LogViewportMetrics("enter");

    if (!m_sessionActive) {
        // Nothing in the engine owns a viewport, so kW/kH can be adopted straight away: the next
        // WebCoreSessionLoad / WebCoreRenderHtml passes the size in and the engine adopts it. In GPU
        // mode these are the PHYSICAL size -- see the block comment above for why.
        kW = ew;
        kH = eh;
        m_appliedDipW = w;
        m_appliedDipH = h;
        // about:home is plain HTML through WebCoreRenderHtml, so re-rendering it is what makes the
        // first frame come out at window size instead of 720px. An error page is left alone: its
        // m_currentUrl is the URL that failed and re-navigating would re-hit the network.
        if (m_currentUrl == L"about:home")
            NavigateTo(ref new String(L"about:home"), false);
        if (m_gpuPresent) {
            // The swapchain is still the init size; the about:home re-render above creates a live
            // session at kW/kH, so re-enter once the load settles to recreate the surface to match.
            auto t = ref new Windows::UI::Xaml::DispatcherTimer();
            Windows::Foundation::TimeSpan ts; ts.Duration = 6000000LL;   // 600 ms
            t->Interval = ts;
            t->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
                [t, self](Platform::Object^, Platform::Object^) {
                    t->Stop();
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_sessionActive && !s->m_loading && !s->m_interacting) s->ApplyViewportSize();
                });
            t->Start();
        }
        return;
    }

    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();

    unsigned long long mySeq = ++m_opSeq;
    // GPU mode: a fresh PropertySet carries the new EGLRenderSurfaceSizeProperty; ANGLE marshals
    // surface create/destroy to the panel dispatcher internally (engine-thread call, like GpuInit).
    // The props stay alive through the callback below and become the new m_gpuProps on success --
    // the old props are released only after the old surface was destroyed inside WebCoreGpuResize.
    void* gpuWin = nullptr;
    Windows::Foundation::Collections::PropertySet^ gpuProps = nullptr;
    if (m_gpuPresent) {
        gpuProps = ref new Windows::Foundation::Collections::PropertySet();
        gpuProps->Insert(L"EGLNativeWindowTypeProperty", GpuPanel);
        // Physical pixels, like the engine viewport below: ANGLE builds the swapchain at this size
        // and the panel then stretches that surface over exactly these many physical pixels.
        gpuProps->Insert(L"EGLRenderSurfaceSizeProperty",
                        Windows::Foundation::PropertyValue::CreateSize(Windows::Foundation::Size((float)ew, (float)eh)));
        gpuWin = reinterpret_cast<void*>(reinterpret_cast<IInspectable*>(gpuProps));
    }
    // ew/eh are captured rather than re-read from kW/kH: the buffer, the engine viewport and the
    // blit all have to agree on one size even if another resize lands while this one is in flight.
    const unsigned navGenAtPost = g_navGen.load(std::memory_order_acquire);
    const unsigned myResizeGen = g_resizeGen.fetch_add(1, std::memory_order_acq_rel) + 1;
    WebEngine::instance().post("viewport-resize", [disp, self, w, h, ew, eh, mySeq, gpuWin, gpuProps, navGenAtPost, myResizeGen]() {
        // Apotheosis: last chance to step aside, taken on the engine thread immediately before the
        // expensive call. A navigation requested after this job was queued makes the resize
        // pointless, and paying for it anyway would stall that navigation -- one thread drains this
        // queue, so seconds spent here are seconds the user waits for the page they asked for.
        //
        // The g_resizeGen half is defence in depth: today m_interacting keeps more than one resize
        // from ever being in flight, so only the navigation case can actually fire. It is here so
        // that relaxing that gate later cannot silently reintroduce a stale-size repaint.
        if (g_navGen.load(std::memory_order_acquire) != navGenAtPost
            || g_resizeGen.load(std::memory_order_acquire) != myResizeGen) {
            LogWriteF("ApplyViewportSize: %dx%d skipped, superseded (navGen %u->%u resizeGen %u->%u)",
                w, h, navGenAtPost, g_navGen.load(std::memory_order_acquire),
                myResizeGen, g_resizeGen.load(std::memory_order_acquire));
            try {
                disp->RunAsync(CoreDispatcherPriority::Normal,
                    ref new DispatchedHandler([self, mySeq]() {
                        MainPage^ s = self.Get(); if (!s) return;
                        // Release the engine so the navigation can be posted, but do NOT adopt
                        // kW/kH: the engine still paints at the old size. The pending size is
                        // applied by OnNavDone once the navigation settles.
                        s->m_interacting = false;
                        if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                        s->SetLoading(false);
                    }));
            } catch (...) {}
            return;
        }
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)ew * eh * 4, 0);
        int rc = -999;
        try { rc = gpuWin ? WebCoreGpuResize(gpuWin, ew, eh, rgba->data()) : WebCoreSessionResize(ew, eh, rgba->data()); } catch (...) { rc = -1000; }
        std::string dgCopy;
        if (rc == 0) { char dg[2048] = ""; try { if (WebCoreGetDiag(dg, (int)sizeof dg) > 0) dgCopy = dg; } catch (...) {} }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            // Link rects are viewport-relative, so a width change invalidates all of them.
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx=0,ly=0,lw=0,lh=0; char lu[1200]="";
                if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) {
                    Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, links, rcCopy, mySeq, w, h, ew, eh, gpuProps, dgCopy]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    s->SetLoading(false);
                    LogWriteF("ApplyViewportSize: engine rc=%d for %dx%d dip (%dx%d px)", rcCopy, w, h, ew, eh);
                    if (rcCopy != 0) return;
                    if (!dgCopy.empty()) LogWriteF("ApplyViewportSize: post-resize diag: %s", dgCopy.c_str());
                    // The engine now paints at ew*eh, so and only so may kW/kH follow; the DIP size
                    // goes into m_appliedDipW/H, which is what the next call's early-out compares.
                    kW = ew;
                    kH = eh;
                    s->m_appliedDipW = w;
                    s->m_appliedDipH = h;
                    // The new surface was created from gpuProps inside WebCoreGpuResize; keep it
                    // alive as the session's current window-surface property set (old one released).
                    if (s->m_gpuPresent && gpuProps) s->m_gpuProps = gpuProps;
                    if (!s->m_gpuPresent) {
                        auto wb = ref new WriteableBitmap(ew, eh);
                        BlitToBitmap(wb, *rgba, ew, eh);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                    }
                    s->m_pageLinks = *links;
                    s->m_lastFrameHash = 0;
                    s->LogViewportMetrics("after-resize");
                    // A resize that arrived while this one was in flight was skipped above; run it
                    // now that the engine is idle again, so the final size always wins.
                    if (s->m_pendW == w && s->m_pendH == h) s->StartLiveMode();
                    else s->ApplyViewportSize();
                }));
        } catch (...) {}
    });
}

// MapTapToEngine: turn a tap position in ContentArea (DIPs) into engine pixels.
// Software mode: RenderImage has Stretch=None and Top/Left alignment, so the bitmap is shown at
// 1:1 and one DIP is one engine pixel -- the position passes through unchanged.
// GPU present mode: the SwapChainPanel stretches the render surface to fill ContentArea, so the
// position has to be scaled back by the ratio of surface size (kW/kH) to ContentArea's size.
void MainPage::MapTapToEngine(double dipX, double dipY, int& outPx, int& outPy)
{
    if (dipX < 0.0 || dipY < 0.0) {
        outPx = -1;
        outPy = -1;
        return;
    }
    double aw = ContentArea->ActualWidth, ah = ContentArea->ActualHeight;
    if (m_gpuPresent && aw > 1.0 && ah > 1.0) {
        outPx = static_cast<int>(dipX * static_cast<double>(kW) / aw + 0.5);
        outPy = static_cast<int>(dipY * static_cast<double>(kH) / ah + 0.5);
    } else {
        outPx = static_cast<int>(dipX + 0.5);
        outPy = static_cast<int>(dipY + 0.5);
    }
}

void MainPage::OnPageTapped(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    HideSuggestions();   // ???????????????????????????????????????(?????????????????????/????????? ??? "?????????")
    // ????????? ContentArea(????????????/????????????,??????????????????)??????????????? ????????? RenderImage:????????????????????????
    //   Collapsed(????????? GpuPanel),?????????????????? GetPosition ???????????? ??? ????????????(?????????????????????????????????)???
    //   ContentArea ????????? = ??????????????????,?????????????????????????????????,???????????????????????????
    auto pt = e->GetPosition(ContentArea);
    HandleTapAt(pt.X, pt.Y, /*scripted*/ false);
}

// Apotheosis 2026-09-18: the body of a tap, callable from a finger and from a script.
//
// Why it is a separate function: `Doc/TOPLEVEL-FETCH-BUDGET.md` ends with the maintainer's own report
// -- "I tapped a link on dzen.ru, the page only flinched and no navigation happened" -- and the branch
// that caused it could not be confirmed, because a tap is only reachable from a real XAML manipulation
// event and synthetic input never reaches that stack on the bench (`synthetic-input-cannot-reach-uwp`).
// So the one code path whose behaviour matters most on a phone was the one path with no way to drive
// it. Everything else here is scriptable; this now is too, through `LocalState\nav.txt` (`TapFromScript`
// below), and deliberately through *this* function rather than a parallel one -- a script that drives a
// second implementation of the tap proves nothing about the first.
void MainPage::HandleTapAt(double dipX, double dipY, bool scripted)
{
    if (m_loading || m_interacting) {
        // Only a script can land here without a human watching, so only a script says so out loud.
        if (scripted) LogWriteF("simtap: REFUSED dip=(%.1f,%.1f) loading=%d interacting=%d",
                                dipX, dipY, m_loading ? 1 : 0, m_interacting ? 1 : 0);
        return;
    }
    int px, py; MapTapToEngine(dipX, dipY, px, py);
    // Apotheosis: temporary — tap routing had no trace at all, so a lost tap and a tap
    // that simply matched no link were indistinguishable in the log.
    LogWriteF("OnPageTapped: dip=(%.1f,%.1f) engine=(%d,%d) session=%d links=%u sim=%d",
              dipX, dipY, px, py, m_sessionActive ? 1 : 0, (unsigned)m_pageLinks.size(), scripted ? 1 : 0);
    if (px < 0 || py < 0 || px >= kW || py >= kH) {
        if (scripted) LogWriteF("simtap: OUT-OF-VIEWPORT engine=(%d,%d) surface=%dx%d", px, py, kW, kH);
        return;
    }

    // ?????????:???????????????????????????????????????????????????????????????????????????????????????/?????????(z-order)?????????????????????
    // ????????????(??????????????????=??????)???????????????????????????????????????,?????????"???????????????"(???????????????????????????
    // ???????????????????????????????????????????????? ??? ???????????????)???ForwardClickToEngine ????????????????????????
    if (m_sessionActive) {
        ForwardClickToEngine(px, py);
        return;
    }
    // ?????????(??????/?????????):????????????????????????
    for (auto it = m_pageLinks.rbegin(); it != m_pageLinks.rend(); ++it) {
        const PageLink& l = *it;
        if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) {
            NavigateTo(ref new String(l.url.c_str()), true);
            return;
        }
    }
}

// Apotheosis 2026-09-18: drive a tap from a script, so the tap path is reproducible on the bench.
//
// A real tap arrives only from a XAML manipulation event, and synthetic input does not reach that stack
// here (`synthetic-input-cannot-reach-uwp`, measured), so before this the tap dispatcher's decision --
// `ForwardClickToEngine` / `TapDone: … branch=` -- could not be exercised without a human finger on a
// touchscreen. That is how a real report ("I tapped a link and the page only flinched") stayed
// unanswerable from the logs.
//
// It goes through `HandleTapAt`, i.e. exactly the same function the finger uses, and it goes through
// `NavigateTo`, the parking logic, NavRetry and the GPU-enable callback. Commands are written into
// `LocalState\nav.txt`, one per write, same as a URL (see `StartNavWatch`):
//
//   tap:<x>,<y>          tap at ContentArea DIP coordinates -- what a finger reports
//   taplink:<n>          tap the centre of link n of the current link table (0 = first extracted)
//   taplinkstr:<text>    tap the centre of the first link whose URL contains <text>
//
// `taplink`/`taplinkstr` exist because raw coordinates are brittle: the link table is rebuilt on every
// frame, so an index or a substring names the target far more reliably than a pixel a layout may have
// moved. The chosen link is printed, so the log always says which element was tapped.
void MainPage::TapFromScript(const std::string& cmd)
{
    auto fail = [&](const char* why) { LogWriteF("simtap: BAD COMMAND '%s' -- %s", cmd.c_str(), why); };

    if (cmd.rfind("tap:", 0) == 0) {
        double dx = 0.0, dy = 0.0;
        // sscanf_s, not std::sscanf_s: the MSVC CRT declares the _s overloads in the global namespace
        // only, and the plain sscanf is a C4996 error in this project (warnings are errors).
        if (sscanf_s(cmd.c_str() + 4, "%lf,%lf", &dx, &dy) != 2) { fail("expected tap:<x>,<y>"); return; }
        LogWriteF("simtap: tap dip=(%.1f,%.1f)", dx, dy);
        HandleTapAt(dx, dy, /*scripted*/ true);
        return;
    }

    // Shared by both link forms: resolve a PageLink, convert its engine-pixel centre back to the DIP
    // the finger would have reported (the exact inverse of MapTapToEngine), and tap there. The inverse
    // matters: MapTapToEngine scales by surface/ContentArea when the SwapChainPanel stretches the
    // surface, so a centre passed through unscaled would miss the link by the same ratio.
    auto tapLink = [&](const PageLink& l, int index, const char* how) {
        double aw = ContentArea->ActualWidth, ah = ContentArea->ActualHeight;
        double sx = (m_gpuPresent && aw > 1.0) ? aw / static_cast<double>(kW) : 1.0;
        double sy = (m_gpuPresent && ah > 1.0) ? ah / static_cast<double>(kH) : 1.0;
        double dx = (l.x + l.w * 0.5) * sx;
        double dy = (l.y + l.h * 0.5) * sy;
        LogWriteF("simtap: %s -> link #%d rect=(%d,%d %dx%d) dip=(%.1f,%.1f) url=%s",
                  how, index, l.x, l.y, l.w, l.h, dx, dy, WideToUtf8(l.url).c_str());
        HandleTapAt(dx, dy, /*scripted*/ true);
    };

    if (cmd.rfind("taplink:", 0) == 0) {
        int n = -1;
        if (sscanf_s(cmd.c_str() + 8, "%d", &n) != 1 || n < 0) { fail("expected taplink:<n>, n>=0"); return; }
        if (n >= (int)m_pageLinks.size()) {
            LogWriteF("simtap: taplink:%d -- only %u links in the table", n, (unsigned)m_pageLinks.size());
            return;
        }
        tapLink(m_pageLinks[(size_t)n], n, "taplink");
        return;
    }

    if (cmd.rfind("taplinkstr:", 0) == 0) {
        std::wstring needle = Utf8ToWide(cmd.substr(11));
        if (needle.empty()) { fail("expected taplinkstr:<text>"); return; }
        for (size_t i = 0; i < m_pageLinks.size(); ++i) {
            if (m_pageLinks[i].url.find(needle) != std::wstring::npos) {
                tapLink(m_pageLinks[i], (int)i, "taplinkstr");
                return;
            }
        }
        LogWriteF("simtap: taplinkstr:<%s> -- no match in %u links",
                  WideToUtf8(needle).c_str(), (unsigned)m_pageLinks.size());
        return;
    }

    fail("unknown command; use tap:<x>,<y> | taplink:<n> | taplinkstr:<text>");
}

// ??? (px,py) ????????????????????????????????????????????????????????????????????????????????????;???????????????????????????
// (URL ??????),??? UI ??????????????????/???????????????/???????????????????????????????????????????????? ??? ?????????????????????
void MainPage::ForwardClickToEngine(int px, int py)
{
    LogWriteF("ForwardClickToEngine: px=%d py=%d interacting=%d loading=%d", px, py, m_interacting ? 1 : 0, m_loading ? 1 : 0);
    if (m_interacting) return;
    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();   // ??????:?????????/????????????,40s ????????????
    TitleText->Text = ref new String(GetStr(m_uiLang, S_PROCESSING).c_str());

    // ???????????????(??????????????????????????????????????????)
    auto linkHit = std::make_shared<std::wstring>();
    for (auto it = m_pageLinks.rbegin(); it != m_pageLinks.rend(); ++it) {
        const PageLink& l = *it;
        if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) { *linkHit = l.url; break; }
    }

    // Apotheosis 2026-09-19: there used to be a `std::wstring prevUrl = m_currentUrl;` here, captured
    // into the tap lambda. It is gone deliberately -- see the read taken inside the lambda, just before
    // the click. Doc/TAP-DISPATCH.md Sec.7.
    //
    // What stays is a copy of the same value for LOGGING ONLY, printed as `cur=` on the TapDone line
    // beside the engine-side `pre=`. The fix removed this snapshot from the decision, but the reason it
    // was wrong is a claim about timing that no run has ever shown in a log -- so both values are now
    // printed and the claim becomes measurable: on a tap where `pre=` and `cur=` differ, the old term
    // would have read "the URL changed" for a click that changed nothing. It feeds no branch.
    std::wstring curUrlSnapshot = m_currentUrl;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;

    WebEngine::instance().post("tap", [disp, self, px, py, linkHit, mySeq, curUrlSnapshot]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        // Apotheosis 2026-09-19: the URL this click is about to act on, read HERE, on the engine thread,
        // immediately BEFORE the click -- not `m_currentUrl` as it was on the UI thread when the finger
        // landed. The tap is queued and can wait behind a load or a long settle pump, so the UI thread's
        // snapshot can be stale by the time the click runs; the old term then compared "the URL now"
        // against "the URL then" and read a difference for a click that changed nothing. The result was
        // navUrl non-empty -> branch=in-page -> the harness showed the engine frame and never navigated
        // the link under the finger, which is the "the page only flinched" report arriving through a
        // different term than the one Sec.1 was about. Measured cost of the old form: none of these are
        // visible in the log, because the comparison had no line of its own.
        std::wstring urlBeforeClick;
        try { char ub[1024] = ""; WebCoreGetUrl(ub, sizeof ub); urlBeforeClick = ToWide(ub); } catch (...) {}
        int rc = -999;
        unsigned hashBefore = WebCoreGetFrameHash();
        try { rc = WebCoreClickAt(px, py, rgba->data()); } catch (...) { rc = -1000; }
        unsigned hashAfter = (rc == 0) ? WebCoreGetFrameHash() : hashBefore;
        bool changed = (hashAfter != hashBefore);   // ?????????????????????????????????(??????????????????/?????? vs ?????????)
        int editable = 0;
        try { if (rc == 0) editable = WebCoreFocusedEditable(); } catch (...) {}   // 1 if the tap focused a text field
        // Apotheosis 2026-09-18: the page's own answer on this tap, straight from the `click` event the
        // engine dispatched -- WebCoreLastClickDefaultPrevented in WebCoreDriver.h. It replaces
        // `changed` as the gate below, because the frame hash cannot tell "the page refused the default
        // action and will update itself later" from "the page ignored the tap". Measured on
        // linkframe3.html: a handler calling preventDefault() with no synchronous repaint gave
        // changed=0, navEmpty=1, dpAfter=1 -- and the harness then navigated the link anyway, against
        // the page's explicit wish. `changed` is still logged: it answers a different question.
        int refused = -1;
        try { refused = WebCoreLastClickDefaultPrevented(); } catch (...) {}

        std::wstring navUrl, title;
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            char t[512] = ""; WebCoreGetTitle(t, sizeof t); title = ToWide(t);
            char u[1024] = ""; WebCoreGetUrl(u, sizeof u);
            std::wstring newUrl = ToWide(u);
            // Apotheosis 2026-09-19: against the pre-click read taken on this thread, not against a
            // UI-thread snapshot of m_currentUrl. This term now means what the gate says it means --
            // "THIS click moved the document URL" -- with no dependence on how current m_currentUrl is.
            if (!newUrl.empty() && newUrl != urlBeforeClick) navUrl = newUrl;
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx=0,ly=0,lw=0,lh=0; char lu[1200]="";
                if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) {
                    Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        }
        auto titleW = std::make_shared<std::wstring>(title);
        auto navW = std::make_shared<std::wstring>(navUrl);
        // Apotheosis 2026-09-19: carried out so the decision line can show both sides of the URL
        // comparison. `preEmpty=` on that line is the one case where this term degrades: if the engine
        // had no document URL to read before the click, the comparison is against nothing and the term
        // falls back to the old behaviour for that tap alone.
        auto preW = std::make_shared<std::wstring>(urlBeforeClick);
        // The term this fix replaced, carried out for the log only. See the note at ForwardClickToEngine.
        auto curW = std::make_shared<std::wstring>(curUrlSnapshot);
        int rcCopy = rc;
        bool changedCopy = changed;
        int editableCopy = editable;
        int refusedCopy = refused;
        // Apotheosis 2026-09-18: the dispatcher below decides the whole outcome of a tap, and until now
        // it logged none of its inputs -- so "the tap was swallowed by the frame-hash test" and "the tap
        // matched no link" were indistinguishable, and a real report ("the page only flinched") could not
        // be settled from the logs. Same reason WebCoreClickAt got its [HIT] trace. Printed on the UI
        // thread with the rest of the decision, so the four values and the branch they produce sit on
        // one line.
        unsigned hashBCopy = hashBefore, hashACopy = hashAfter;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, titleW, navW, preW, curW, links, rcCopy, changedCopy, editableCopy, refusedCopy, linkHit, mySeq, hashBCopy, hashACopy]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // ????????????/???????????????,??????????????????
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    // Apotheosis 2026-09-18: the branch this line predicts is the branch taken below.
                    // `branch=` names it outright so a report never has to be guessed at again:
                    //   nav-link  = the harness navigates, using the link under the finger
                    //   nav-fail  = the click failed AND a link was known -> navigate anyway
                    //   in-page   = treated as the page's own business; NO navigation happens here
                    //   dead      = nothing to do but restore the chrome
                    //
                    // The gate is `refused`, NOT `changed`. refused=1 means the page's own click handler
                    // called preventDefault(): the page has taken responsibility for this tap and the
                    // harness must not undo that, however little it repainted synchronously. refused=-1
                    // means no `click` event was recorded at all -- the anchor's default action never had
                    // a turn either -- so the old fallback (use the link under the finger) stays in force
                    // for that case alone, which is what it was written for.
                    const char* branch = (rcCopy == 0)
                        ? ((navW->empty() && !refusedCopy && !linkHit->empty()) ? "nav-link" : "in-page")
                        : (linkHit->empty() ? "dead" : "nav-fail");
                    // Apotheosis 2026-09-19: `preEmpty=` and `pre=` close Sec.7 of Doc/TAP-DISPATCH.md.
                    // The URL term is now read on the engine thread immediately before the click, so it
                    // answers "did THIS click move the document URL" -- but that read can itself come
                    // back empty (no document URL to compare against), and then the term is vacuous and
                    // this tap degrades to the old fallback. `preEmpty=1` names that case outright;
                    // `pre=` prints what was read, so a stale-looking value is visible rather than
                    // inferred. Both are diagnostic only and change no branch.
                    std::string preU8 = WideToUtf8(*preW);
                    std::string curU8 = WideToUtf8(*curW);
                    // `termDiff=1` is the positive control for the fix: `pre` and `cur` disagree on this
                    // tap, so the two candidate terms would have produced different branch decisions, and
                    // the one this code uses is the engine-side read. It is a measurement, not a gate.
                    LogWriteF("TapDone: rc=%d changed=%d navEmpty=%d linkHit=%d refused=%d hashB=%08x hashA=%08x preEmpty=%d termDiff=%d branch=%s pre=%s cur=%s",
                              rcCopy, changedCopy ? 1 : 0, navW->empty() ? 1 : 0, linkHit->empty() ? 0 : 1,
                              refusedCopy, hashBCopy, hashACopy, preW->empty() ? 1 : 0,
                              (*preW != *curW) ? 1 : 0, branch, preU8.c_str(), curU8.c_str());
                    if (rcCopy == 0) {
                        Platform::String^ title = ref new String(titleW->c_str());
                        Platform::String^ navUrl = navW->empty() ? nullptr : ref new String(navW->c_str());
                        // The engine navigated on its own (navW non-empty) or the page refused the default
                        // action (refused=1) -> show what the engine produced and leave the page alone.
                        // Only when neither happened, and a link was under the finger, does the harness
                        // fall back to navigating that link itself.
                        if (navW->empty() && !refusedCopy && !linkHit->empty()) {
                            s->SetLoading(false);
                            s->NavigateTo(ref new String(linkHit->c_str()), true);
                        } else {
                            s->ApplyEngineFrame(rgba, title, navUrl, links);
                            s->SetLoading(false);
                            // in-page: show keyboard only when the tap landed on an editable element.
                            if (navW->empty()) { if (editableCopy) s->OpenKeyboard(); else s->CloseKeyboard(); }
                            else s->CloseKeyboard();
                        }
                    } else if (!linkHit->empty()) {
                        s->SetLoading(false);   // ?????? m_loading ?????? NavigateTo ??????
                        s->NavigateTo(ref new String(linkHit->c_str()), true);   // ??????????????????
                    } else {
                        // ????????????(-12 ????????? / -14 ?????????):??????????????????????????????,????????????????????????
                        if (rcCopy == -12 || rcCopy == -14) {
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                        }
                        s->SetLoading(false);
                        s->TitleText->Text = ref new String(s->m_currentTitle.empty() ? L"EdgeHTML Reborn" : s->m_currentTitle.c_str());
                    }
                }));
        } catch (...) {}
    });
}

// ??????????????????????????????????????? + ????????????/?????????;navUrl ?????? = ?????????????????????(???????????????/???/??????)???
void MainPage::ApplyEngineFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba,
                               Platform::String^ title, Platform::String^ navUrl,
                               const std::shared_ptr<std::vector<PageLink>>& links)
{
    // present ??????:??????????????? swapBuffers ??? GpuPanel,???????????????(BlitToBitmap ????????????,RenderImage ?????????)???
    if (!m_gpuPresent) {
        auto wb = ref new WriteableBitmap(kW, kH);
        BlitToBitmap(wb, *rgba, kW, kH);
        wb->Invalidate();
        RenderImage->Source = wb;
    }
    m_pageLinks = *links;
    m_lastFrameHash = 0;        // ???????????????????????????(??????????????????)
    StartLiveMode();            // ?????????????????????(?????????????????????/SPA ??????)
    if (title && title->Length() > 0) {
        m_currentTitle = std::wstring(title->Data());
        TitleText->Text = title;
    }
    if (navUrl != nullptr) {
        std::wstring nu = std::wstring(navUrl->Data());
        m_currentUrl = nu;
        m_urlSyncing = true;
        UrlBox->Text = navUrl;
        m_urlSyncing = false;
        UpdateLockIcon();
        UpdateUrlActionGlyph();
        // ????????????????????????????????????,???????????????????????????/?????????????????????????????????(??????"???????????????")???
        bool dup = (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() && m_navStack[m_navIndex] == nu);
        if (!dup) {
            if (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1)
                m_navStack.erase(m_navStack.begin() + m_navIndex + 1, m_navStack.end());
            m_navStack.push_back(nu);
            m_navIndex = (int)m_navStack.size() - 1;
            UpdateNavButtons();
        }
        AddHistory(m_currentUrl, m_currentTitle);
    }
}

// ????????????(?????????????????????/??????????????????)???dy>0 ????????????????????????????????????,native ??????????????????
void MainPage::EngineScroll(int dy)
{
    if (!m_sessionActive || m_loading || m_interacting) return;
    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    WebEngine::instance().post("scroll", [disp, self, dy, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreScrollBy(0, dy, rgba->data()); } catch (...) { rc = -1000; }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx=0,ly=0,lw=0,lh=0; char lu[1200]="";
                if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) {
                    Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, links, rcCopy, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // ????????????/???????????????,??????????????????
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    s->SetLoading(false);
                    if (rcCopy == 0) {
                        if (!s->m_gpuPresent) {
                            auto wb = ref new WriteableBitmap(kW, kH);
                            BlitToBitmap(wb, *rgba, kW, kH);
                            wb->Invalidate();
                            s->RenderImage->Source = wb;
                        }
                        s->m_pageLinks = *links;
                        s->m_lastFrameHash = 0;
                        s->StartLiveMode();   // ?????????????????????(?????????????????????/??????)
                    }
                }));
        } catch (...) {}
    });
}
void MainPage::OnScrollUp(Platform::Object^, RoutedEventArgs^)   { EngineScroll(-900); }
void MainPage::OnScrollDown(Platform::Object^, RoutedEventArgs^) { EngineScroll(900); }

// ---- ????????????:???????????? / ?????? ??? ???????????? ??? ?????????????????????(??? spinner,??????)----
void MainPage::FreeScrollBy(int dx, int dy)
{
    if (!m_sessionActive || (dx == 0 && dy == 0)) return;
    m_scrollAccumX += dx;
    m_scrollAccum += dy;
    if (!m_scrollBusy) PumpScroll();
}
void MainPage::PumpScroll()
{
    if ((m_scrollAccum == 0 && m_scrollAccumX == 0) || !m_sessionActive) { m_scrollBusy = false; return; }
    int dy = m_scrollAccum; m_scrollAccum = 0;
    int dx = m_scrollAccumX; m_scrollAccumX = 0;
    m_scrollBusy = true;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // ?????????:???????????????????????????/????????????,??????????????????(??????????????????????????????)
    bool present = m_gpuPresent;
    // ??? ??????:??????????????????????????? FFI ???????????????(????????????????????? extractLinks),present ????????? WriteableBitmap
    //   ?????????(????????????????????? GpuPanel,BlitToBitmap ????????????)????????????????????????????????? SyncLinksAfterScroll ???????????????
    WebEngine::instance().post("pump-scroll", [disp, self, dx, dy, mySeq, present]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreScrollBy(dx, dy, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, mySeq, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) { s->m_scrollBusy = false; s->m_scrollAccum = 0; return; }   // ?????????/????????????,???????????????
                if (rcCopy == 0) {
                    if (!present) { auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb; }
                    s->m_lastFrameHash = 0;
                }
                s->m_scrollBusy = false;
                if (s->m_scrollAccum != 0) s->PumpScroll();   // ???????????????????????????,????????????
                else { s->SyncLinksAfterScroll(); s->StartLiveMode(); }  // ???????????? ??? ???????????? + ????????????(??????????????????/??????)
            }));
        } catch (...) {}
    });
}

// ?????????????????????????????????????????????(???????????????????????????????????? extractLinks)????????????????????????????????????(??????),
// ????????????????????????????????????/????????????;??????????????????"????????????????????????"??????,?????????
void MainPage::SyncLinksAfterScroll()
{
    if (!m_sessionActive) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;
    WebEngine::instance().post("sync-links", [disp, self, mySeq]() {
        int rc = -999;
        try { rc = WebCoreSyncLinks(); } catch (...) { rc = -1000; }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) { int lx=0,ly=0,lw=0,lh=0; char lu[1200]=""; if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) { Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu); links->push_back(std::move(pl)); } }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self, links, rcCopy, mySeq]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;   // ??????????????????
                if (rcCopy == 0) s->m_pageLinks = *links;
            }));
        } catch (...) {}
    });
}
// ????????????:????????? ManipulationDelta(?????? ScrollViewer ???,??????????????????)???????????????????????? ??Y ??? ???????????????
// TranslateInertia ???????????????????????????(ManipulationDelta ????????????????????????)???????????? Tapped ???(???????????????
// ???????????? vs ??????,?????????=Tapped????????????=Manipulation,????????????)???
// Apotheosis: hold or drop the DisplayRequest that keeps the screen from blanking, per the keepawake
// setting and whether we currently have the foreground. This exists to make remote testing possible on
// the Lumia: the phone blanks after about five minutes, an app that loses focus is suspended and then
// reaped by the OS, and the log then reports a clean exit for a run that never rendered anything --
// which has already sent this investigation down two false trails. With keepawake=1 in settings.ini one
// launch from the tile keeps the device testable for as long as it is left running.
// The request follows focus rather than outliving it, so it cannot pin the screen on behind the user's
// back. It is a debug aid and stays off by default: a browser has no business holding the display awake.
// Apotheosis: drive the harness navigation path from a file, so scenarios that need more than one
// navigation can be reproduced without touching the screen. Write a URL into LocalState\nav.txt and the
// app navigates to it, going through NavigateTo, the parking logic, NavRetry and the GPU-enable
// callback -- everything autodiag.txt deliberately bypasses.
//
// The same file also carries scripted taps since 2026-09-18 (`tap:<x>,<y>`, `taplink:<n>`,
// `taplinkstr:<text>`); those go through `TapFromScript` -> `HandleTapAt`, the identical function a
// finger enters, because the tap dispatcher's branch is the one thing on this page that could not be
// reproduced without a touchscreen.
//
// Why this exists: on 2026-08-21 the autodiag path loaded example.com, example.com, Hacker News and
// example.com in one session and every one of them painted, while the same sequence performed by hand
// on the Lumia went blank after the first. autodiag calls WebCoreSessionLoad directly and initialises
// the GPU offscreen (`WebCoreGpuInit(nullptr, ...)`), so it cannot reproduce a defect that lives in the
// harness or in the present path with a real SwapChainPanel. Without a scriptable way into that path
// the only reproduction tool was a human finger and a 20-minute deploy.
//
// One second between polls, and each poll is a single GetFileAttributesEx -- cheap enough to leave on
// permanently, which matters because a debug aid you have to enable first is one you will not have when
// you need it. The stamp is seeded at startup so an existing nav.txt does not fire on launch: the file
// means "go here now", not "go here at startup" -- that is what testurl.txt is for.
void MainPage::StartNavWatch()
{
    if (m_navWatch) return;
    std::wstring dir = LocalStateDir();
    if (dir.empty()) return;
    const std::wstring path = dir + L"\\nav.txt";

    WIN32_FILE_ATTRIBUTE_DATA fad {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        m_navWatchStamp = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)
                        | (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
    }

    m_navWatch = ref new Windows::UI::Xaml::DispatcherTimer();
    Windows::Foundation::TimeSpan ts; ts.Duration = 10000000LL;   // 1 s in 100 ns units
    m_navWatch->Interval = ts;
    Platform::Agile<MainPage^> self(this);
    m_navWatch->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
        [self, path](Platform::Object^, Platform::Object^) {
            MainPage^ s = self.Get(); if (!s) return;
            WIN32_FILE_ATTRIBUTE_DATA a {};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return;
            const unsigned long long stamp = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32)
                                           | (unsigned long long)a.ftLastWriteTime.dwLowDateTime;
            if (stamp == s->m_navWatchStamp) return;
            s->m_navWatchStamp = stamp;
            std::string line;
            try {
                std::ifstream f(WideToUtf8(path), std::ios::binary);
                if (!f) return;
                std::getline(f, line);
            } catch (...) { return; }
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'
                                  || line.back() == ' '  || line.back() == '\t'))
                line.pop_back();
            if (line.empty()) return;
            // Apotheosis 2026-09-18: `nav.txt` also carries scripted taps now (`tap:`, `taplink:`,
            // `taplinkstr:` -- see TapFromScript). Same file, same one-command-per-write rule: two
            // watchers would mean two files to keep in step, and a tap is a navigation request from
            // where a user sits. Anything else is a URL, as before.
            if (line.rfind("tap:", 0) == 0 || line.rfind("taplink:", 0) == 0
                || line.rfind("taplinkstr:", 0) == 0) {
                LogWriteF("navwatch: nav.txt changed -> scripted tap '%s'", line.c_str());
                s->TapFromScript(line);
                return;
            }
            LogWriteF("navwatch: nav.txt changed -> %s", line.c_str());
            s->NavigateTo(NormalizeUrl(ref new String(Utf8ToWide(line).c_str())), true);
        });
    m_navWatch->Start();
    LogWrite("navwatch: watching LocalState\\nav.txt (write a URL there to navigate)");
}

// Apotheosis: play a navigation sequence that shipped inside the package, through the harness path.
//
// This is the device-side counterpart of nav.txt. On the bench a script writes URLs into
// LocalState\nav.txt and the watcher picks them up; on the Lumia that is impossible, because Device
// Portal on OS 15254.603 reads LocalState but cannot write to it -- every upload form answers HTTP 500,
// verified against three different path spellings on 2026-08-21. The appx is the only channel that
// arrives, so the sequence travels in it, and so does its on/off switch: `enabled=1` inside the file.
// A marker file in LocalState would have been the natural design and is exactly what cannot be
// delivered.
//
// It drives NavigateTo, which is the point. autodiag.txt calls WebCoreSessionLoad directly and
// initialises the GPU offscreen, so it skips NavigateTo, the parking logic, NavRetry and the GPU-enable
// callback -- it loaded four pages in one session with all four painting while the device went blank
// after the first. A driver that avoids the code under test cannot test it.
//
// delay=0 means "next tick", not "same instant": the navigation has to land while the previous load is
// still running (loading=1), and that is the condition behind the crash currently being chased. 250 ms
// is short enough to overlap a heavy page and long enough for the dispatcher to breathe.
void MainPage::StartNavSeq()
{
    if (m_navSeqTimer) return;
    std::wstring install = InstallDir();
    if (install.empty()) return;

    std::string body;
    try {
        std::ifstream f(WideToUtf8(install) + "\\Assets\\navseq.txt", std::ios::binary);
        if (!f) return;                                  // not packaged: nothing to do, and no noise
        std::ostringstream ss; ss << f.rdbuf(); body = ss.str();
    } catch (...) { return; }

    bool enabled = false;
    int delay = 25;
    bool wantKeepAwake = false;
    bool disableJit = false;
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'
                              || line.back() == ' '  || line.back() == '\t'))
            line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line[0] == '#') continue;
        if (line.rfind("enabled=", 0) == 0)   { enabled = (line.substr(8) == "1"); continue; }
        if (line.rfind("keepawake=", 0) == 0) { wantKeepAwake = (line.substr(10) == "1"); continue; }
        if (line.rfind("jit=", 0) == 0)       { disableJit = (line.substr(4) == "0"); continue; }
        if (line.rfind("delay=", 0) == 0)     { delay = std::atoi(line.c_str() + 6); if (delay < 0) delay = 0; continue; }
        m_navSeq.emplace_back(delay, Utf8ToWide(line));
    }

    // Apotheosis: honour keepawake from the PACKAGE, because settings.ini lives in LocalState and Device
    // Portal cannot write there -- so the setting shipped a day earlier had never once been active on the
    // phone. This is a diagnostic necessity rather than a comfort: an app that loses foreground focus is
    // suspended and then reaped by the OS, and that looks identical to a crash from the outside -- no UEF
    // line, no minidump, a log that stops mid-load. Holding the display removes that explanation from the
    // table instead of leaving two hypotheses to argue over. Applied even when the sequence itself is not
    // playing, since a hand-driven session needs the screen just as much.
    if (wantKeepAwake && !m_keepAwake) {
        m_keepAwake = true;
        LogWrite("navseq: keepawake=1 from the package -- holding the display while in the foreground");
        UpdateKeepAwake(true);
    }

    // Apotheosis: jit=0 turns the JIT off for this build, by setting the environment variable
    // JavaScriptCore itself reads. Options.cpp scans the environment for names beginning with "JSC_"
    // when JSC::initialize() runs, and initialize() runs on the engine thread at the first session
    // load -- well after this parse, which happens in the MainPage constructor.
    //
    // It is here rather than in settings.ini for the usual reason: Device Portal cannot write
    // LocalState, so a switch that has to reach the phone must ship in the package.
    //
    // Why the switch exists at all: it was added to test whether the device's JIT was behind the
    // phone's death inside RunLoop::run(), versus a bench that survived the identical sequence -- see
    // Doc/PUMPLOOP-SILENT-DEATH.md. **The premise written here was wrong and is kept only as history:
    // the JIT is NOT blocked on the x64 bench.** Measured 2026-09-18 on 0.1.9.107 with this line
    // removed: dzen.ru rendered under the JIT (`nonwhite=302013/710656`, `js=1/1`) exactly as it had
    // under the interpreter, and the freeze that followed was identical either way. The bench and the
    // phone also build the same tiers -- LLInt + baseline JIT, ENABLE_DFG_JIT=OFF, ENABLE_FTL_JIT=OFF
    // on both (both CMakeCache.txt files, and CLAUDE.md's hard constraints). See
    // Doc/DZEN-SCROLL-DEATH.md sections 10b-10d. The switch still works and is still worth keeping as
    // a bisect knob; it is no longer a live hypothesis.
    if (disableJit) {
        _putenv_s("JSC_useJIT", "false");
        LogWrite("navseq: jit=0 from the package -- JSC_useJIT=false, the engine will interpret");
    }

    if (!enabled) {
        LogWrite("navseq: Assets\\navseq.txt present but enabled=1 is absent -- not playing");
        m_navSeq.clear();
        return;
    }
    if (m_navSeq.empty()) {
        LogWrite("navseq: enabled but no URLs listed");
        return;
    }

    // Keep a copy where it can be read back over Device Portal, which is the half of WDP that works --
    // so a run can be checked against the sequence the device actually received, not the one intended.
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ofstream out(WideToUtf8(d) + "\\navseq-active.txt", std::ios::binary | std::ios::trunc);
            if (out) out.write(body.data(), body.size());
        }
    } catch (...) {}

    LogWriteF("navseq: playing %zu navigations from the package", m_navSeq.size());
    m_navSeqIndex = 0;
    m_navSeqTimer = ref new Windows::UI::Xaml::DispatcherTimer();
    Platform::Agile<MainPage^> self(this);
    m_navSeqTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
        [self](Platform::Object^, Platform::Object^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->m_navSeqTimer->Stop();
            if (s->m_navSeqIndex >= s->m_navSeq.size()) {
                LogWrite("navseq: sequence finished");
                return;
            }
            const auto& step = s->m_navSeq[s->m_navSeqIndex];
            const size_t n = s->m_navSeqIndex + 1;
            LogWriteF("navseq[%zu/%zu] delay=%d -> %s", n, s->m_navSeq.size(),
                      step.first, WideToUtf8(step.second).c_str());
            s->m_navSeqIndex++;
            s->NavigateTo(NormalizeUrl(ref new String(step.second.c_str())), true);
            // Arm for the entry after this one, using ITS delay.
            if (s->m_navSeqIndex < s->m_navSeq.size()) {
                const int next = s->m_navSeq[s->m_navSeqIndex].first;
                Windows::Foundation::TimeSpan ts;
                ts.Duration = (next <= 0) ? 2500000LL : (long long)next * 10000000LL;   // 250 ms, or N s
                s->m_navSeqTimer->Interval = ts;
                s->m_navSeqTimer->Start();
            }
        });
    // The first entry's own delay applies from startup, so the home page has time to settle first.
    Windows::Foundation::TimeSpan first;
    const int d0 = m_navSeq[0].first;
    first.Duration = (d0 <= 0) ? 2500000LL : (long long)d0 * 10000000LL;
    m_navSeqTimer->Interval = first;
    m_navSeqTimer->Start();
}

void MainPage::UpdateKeepAwake(bool foreground)
{
    try {
        if (!m_keepAwake || !foreground) {
            if (m_displayRequestActive && m_displayRequest) {
                m_displayRequest->RequestRelease();
                m_displayRequestActive = false;
                LogWrite("keepawake: released");
            }
            return;
        }
        if (!m_displayRequest) m_displayRequest = ref new Windows::System::Display::DisplayRequest();
        if (!m_displayRequestActive) {
            m_displayRequest->RequestActive();
            m_displayRequestActive = true;
            LogWrite("keepawake: active -- the screen will not blank while this app is in front");
        }
    } catch (Platform::Exception^ ex) {
        LogWriteF("keepawake: FAILED hr=0x%08X msg=%ls", (unsigned)ex->HResult,
                  ex->Message ? ex->Message->Data() : L"(none)");
    } catch (...) {
        LogWrite("keepawake: FAILED (non-WinRT exception)");
    }
}

void MainPage::OnImageManipDelta(Platform::Object^, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e)
{
    // Apotheosis: this handler used to be completely silent, which made its silence unreadable. On
    // 2026-08-20 the home page would not scroll on the device, the log held no manipulation lines, and
    // that was twice taken as evidence that touch never reached the app -- when in fact the gesture
    // arrived and was discarded one line below, because about:home has no engine session. A gesture
    // that is deliberately ignored and a gesture that never arrived have to look different in the log.
    // Throttled to one line per second: manipulation fires dozens of times per drag and would bury
    // everything else.
    {
        static unsigned long long lastLog = 0;
        const unsigned long long now = GetTickCount64();
        if (now - lastLog > 1000) {
            lastLog = now;
            LogWriteF("ManipDelta: session=%d dx=%.1f dy=%.1f scale=%.3f pinching=%d",
                m_sessionActive ? 1 : 0, e->Delta.Translation.X, e->Delta.Translation.Y,
                e->Delta.Scale, m_pinching ? 1 : 0);
        }
    }
    if (!m_sessionActive) return;
    // M4 ????????????:???????????? Scale???1(??????????????????)??? ????????????:???????????????????????? ScaleTransform(???????????????,
    //   ??????),??????????????????;??????(OnImageManipCompleted)?????????????????????????????????????????????????????????
    float ds = e->Delta.Scale;
    if (m_pinching || (ds > 0.0f && (ds > 1.002f || ds < 0.998f))) {
        m_pinching = true;
        if (ds > 0.0f) m_liveScale *= ds;
        float total = m_pageScale * m_liveScale;          // ??????????????? [0.5,6.0]
        if (total < 0.5f) m_liveScale = 0.5f / m_pageScale;
        if (total > 6.0f) m_liveScale = 6.0f / m_pageScale;
        auto fp = e->Position;                            // ????????????(?????? ContentArea = ????????????)
        m_focalX = fp.X; m_focalY = fp.Y;
        ApplyLiveZoom();
        return;
    }
    double dx = -e->Delta.Translation.X;             // ????????????(??X<0)??? ????????????(dx>0)
    double dy = -e->Delta.Translation.Y;             // ????????????(??Y<0)??? ????????????(dy>0)
    int idx = (int)(dx < 0 ? dx - 0.5 : dx + 0.5);
    int idy = (int)(dy < 0 ? dy - 0.5 : dy + 0.5);
    if (idx != 0 || idy != 0) FreeScrollBy(idx, idy);
}

// ??????????????????:??? ScaleTransform(??????????????????)?????????????????????(present=GpuPanel,readback=RenderImage)???
//   ???????????????????????? ??? ???????????? 60fps ??????,???????????????
void MainPage::ApplyLiveZoom()
{
    auto t = ref new Windows::UI::Xaml::Media::ScaleTransform();
    t->ScaleX = m_liveScale; t->ScaleY = m_liveScale;
    t->CenterX = m_focalX; t->CenterY = m_focalY;
    if (m_gpuPresent) GpuPanel->RenderTransform = t;
    else RenderImage->RenderTransform = t;
}

// ????????????:??????????????????????????????(WebCoreSetPageScale ????????????????????? ??? ????????????),??? UI ??????????????? + ??????????????????
void MainPage::OnImageManipCompleted(Platform::Object^, Windows::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs^)
{
    if (!m_pinching) return;
    m_pinching = false;
    float live = m_liveScale; m_liveScale = 1.0f;
    float newScale = m_pageScale * live;
    if (newScale < 0.5f) newScale = 0.5f;
    if (newScale > 6.0f) newScale = 6.0f;
    // ???????????????????????????(DIP,?????? ScaleTransform ??????)????????????????????????????????????,????????????????????????
    int fpx, fpy; MapTapToEngine(m_focalX, m_focalY, fpx, fpy);
    PinchCommit(newScale, fpx, fpy);
}

// ??????????????????????????????:WebCoreSetPageScale ??? ????????????;??? UI ???????????????????????? + ?????? RenderTransform + ?????????
void MainPage::PinchCommit(float newScale, int focalX, int focalY)
{
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    bool present = m_gpuPresent;
    WebEngine::instance().post("pinch-commit", [disp, self, newScale, focalX, focalY, mySeq, present]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreSetPageScale(newScale, focalX, focalY, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, newScale, mySeq, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;            // ??????????????????,???????????????
                if (rcCopy == 0) {
                    s->m_pageScale = newScale;
                    if (!present) { auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb; }
                    // present ??????:????????? swapBuffers ??? GpuPanel,??????????????????????????????
                }
                // ??????????????????(??????????????????????????????????????????;????????????,????????????????????????)???
                s->GpuPanel->RenderTransform = nullptr;
                s->RenderImage->RenderTransform = nullptr;
                s->StartLiveMode();
            }));
        } catch (...) {}
    });
}

// ---- GPU ??????1 ?????? ----
void MainPage::OnGpuPanelLoaded(Platform::Object^, RoutedEventArgs^)
{
    static bool s_done = false;
    if (s_done) return;   // ????????????
    s_done = true;
    try { RunGpuProbe(GpuPanel, ref new String(LocalStateDir().c_str())); } catch (...) {}
}

// ---- ?????????/???????????? ----
void MainPage::OpenKeyboard()
{
    m_imeOpen = true;
    m_imeSyncing = true;
    ImeBox->Text = ref new String(L"");
    m_lastImeText.clear();
    m_imeSyncing = false;
    ImeBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);   // ???????????? TextBox ??? ??????????????????
}
void MainPage::CloseKeyboard()
{
    if (!m_imeOpen) return;
    m_imeOpen = false;
    try { Windows::UI::ViewManagement::InputPane::GetForCurrentView()->TryHide(); } catch (...) {}
}
void MainPage::OnImeTextChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^)
{
    // ????????????:??????????????????????????? + ???????????? + ImeBox ????????????(??? LocalState\imedebug.txt,??????????????????)???
    // ??????????????????????????? ??? OnImeTextChanged ????????? ??? ?????? ImeBox ????????? IME ??????(UI ?????????);
    // ?????????????????????????????? ??? ?????????(??? SendKeyToEngine ?????? rc:kErrNoDocument=canEdit ?????????)???
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ofstream f(WideToUtf8(d) + "\\imedebug.txt", std::ios::app | std::ios::binary);
            if (f) { std::string s = "TC open=" + std::to_string(m_imeOpen) + " sess=" + std::to_string(m_sessionActive)
                + " sync=" + std::to_string(m_imeSyncing) + " textLen=" + std::to_string(ImeBox->Text ? ImeBox->Text->Length() : 0) + "\n"; f.write(s.data(), s.size()); }
        }
    } catch (...) {}
    if (m_imeSyncing || !m_imeOpen || !m_sessionActive) return;
    std::wstring cur = ImeBox->Text ? std::wstring(ImeBox->Text->Data()) : L"";
    std::wstring prev = m_lastImeText;
    if (cur == prev) return;
    if (cur.size() > prev.size() && cur.compare(0, prev.size(), prev) == 0) {
        SendKeyToEngine(0, ref new String(cur.substr(prev.size()).c_str()));   // ????????????
    } else if (cur.size() < prev.size() && prev.compare(0, cur.size(), cur) == 0) {
        int n = static_cast<int>(prev.size() - cur.size());
        for (int i = 0; i < n; ++i) SendKeyToEngine(2, nullptr);   // ????????????
    } else {
        // ????????????/IME ??????:?????????????????????(????????????????????????)???
        for (size_t i = 0; i < prev.size(); ++i) SendKeyToEngine(2, nullptr);
        if (!cur.empty()) SendKeyToEngine(0, ref new String(cur.c_str()));
    }
    m_lastImeText = cur;
}
void MainPage::OnImeKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (!m_imeOpen) return;
    if (e->Key == Windows::System::VirtualKey::Enter) {
        e->Handled = true;
        SendKeyToEngine(1, nullptr);   // ??????(??????????????????????????????)
        m_imeSyncing = true; ImeBox->Text = ref new String(L""); m_imeSyncing = false; m_lastImeText.clear();
    } else if (e->Key == Windows::System::VirtualKey::Back && m_lastImeText.empty()) {
        SendKeyToEngine(2, nullptr);   // ??????????????????(TextChanged ?????????)
    }
}
void MainPage::SendKeyToEngine(int kind, Platform::String^ text)
{
    if (!m_sessionActive) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string utf8 = (kind == 0 && text) ? ToUtf8(text) : std::string();
    WebEngine::instance().post("key-input", [disp, self, kind, utf8]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try {
            if (kind == 0) rc = WebCoreTypeText(utf8.c_str(), rgba->data());
            else if (kind == 1) rc = WebCoreKeyAction(1, rgba->data());
            else rc = WebCoreKeyAction(0, rgba->data());
        } catch (...) { rc = -1000; }
        // ??????:???????????????(rc=-6/kErrNoDocument ??? ????????? canEdit ??? false=?????????????????????;rc=0 ??? ???????????????)???
        char edbg[256] = ""; try { WebCoreEditDebug(edbg, sizeof edbg); } catch (...) {}
        try { std::wstring dd = LocalStateDir(); if (!dd.empty()) { std::ofstream f(WideToUtf8(dd) + "\\imedebug.txt", std::ios::app | std::ios::binary); if (f) { std::string s = "  SK kind=" + std::to_string(kind) + " rc=" + std::to_string(rc) + " [" + edbg + "]\n"; f.write(s.data(), s.size()); } } } catch (...) {}
        std::wstring navUrl, title;
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0 && kind == 1) {   // ?????????????????? ??? ?????? url/title/??????
            char t[512] = ""; WebCoreGetTitle(t, sizeof t); title = ToWide(t);
            char u[1024] = ""; WebCoreGetUrl(u, sizeof u); navUrl = ToWide(u);
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) { int lx=0,ly=0,lw=0,lh=0; char lu[1200]=""; if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) { Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu); links->push_back(std::move(pl)); } }
        }
        auto navW = std::make_shared<std::wstring>(navUrl); auto titleW = std::make_shared<std::wstring>(title);
        int rcCopy = rc; int kindCopy = kind;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, kindCopy, navW, titleW, links]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (rcCopy != 0) return;
                if (kindCopy == 1 && !navW->empty() && std::wstring(navW->c_str()) != s->m_currentUrl) {
                    s->ApplyEngineFrame(rgba, ref new String(titleW->c_str()), ref new String(navW->c_str()), links);   // ????????????:???????????????/??????
                    s->CloseKeyboard();
                } else {
                    if (!s->m_gpuPresent) { auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb; }
                    s->m_lastFrameHash = 0;
                    s->StartLiveMode();   // ??????????????????????????? ??? ?????????????????????????????????/????????????(??????????????????????????????)
                }
            }));
        } catch (...) {}
    });
}

// ---- ??????????????????:?????????????????????????????????/SPA ???????????? ----
void MainPage::StartLiveMode()
{
    if (!m_sessionActive || !m_appForeground) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (DiagPage && DiagPage->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    m_liveBusy = false;        // ??????(????????? RunAsync ???/??????????????????,????????????)
    m_liveBusyAge = 0;
    m_liveStaticTicks = 0;
    m_liveTotalTicks = 0;
    m_liveSettleTicks = 0;
    if (!m_liveTimer) {
        m_liveTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        m_liveTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnLiveTick);
    }
    // ??????????????????????????????(?????????????????????????????????)???
    Windows::Foundation::TimeSpan ts; ts.Duration = 2000000LL;   // 200ms(100ns ??????)??? 5fps
    m_liveTimer->Interval = ts;
    m_liveTimer->Start();
}
void MainPage::StopLiveMode()
{
    if (m_liveTimer) m_liveTimer->Stop();
}
void MainPage::OnLiveTick(Platform::Object^, Platform::Object^)
{
    if (!m_sessionActive || !m_appForeground || m_loading || m_interacting) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible
        || ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible
        || SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible
        || (DiagPage && DiagPage->Visibility == Windows::UI::Xaml::Visibility::Visible)
        || TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) { StopLiveMode(); return; }
    // Apotheosis: never stack a live tick on top of engine work. The self-heal below declares an
    // outstanding tick lost after five timer ticks (~1s) and posts a fresh one, which is sound only
    // if a tick is cheap. On a heavy page it is not: one tick on hh.ru costs ~4s, so ticks were being
    // produced 4x faster than the single engine thread could retire them and the FIFO grew without
    // bound -- measured 22 jobs deep after 39s of idle repainting, and a resize queued behind them
    // did not run for 130s. The queue is shared with navigation and taps, so the backlog is paid by
    // whatever the user does next. While the engine is busy or has anything queued the tick is late,
    // not lost, so there is nothing to heal and nothing to post.
    {
        WebEngine& eng = WebEngine::instance();
        if (eng.busy() || eng.pending() > 0) { m_liveBusyAge = 0; return; }
    }
    if (m_liveBusy) {                      // ??????????????????????????????
        if (++m_liveBusyAge < 5) return;   // ?????????(~1s ???)
        m_liveBusy = false;                // ????????? ??? RunAsync ???????????????,??????????????????
    }
    m_liveBusyAge = 0;
    m_liveBusy = true;
    unsigned long long mySeq = m_opSeq;    // ???????????????:?????????????????????,???????????????????????????????????????
    bool present = m_gpuPresent;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    WebEngine::instance().post("live-tick", [disp, self, mySeq, present]() {
        MainPage^ s0 = self.Get();
        if (!s0 || !s0->m_appForeground) return;   // ????????????:?????? PLM ?????????????????? JS+??????(m_liveBusy ???????????? StartLiveMode ???)
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999; unsigned hash = 0; int pending = 0;
        // Apotheosis: time the tick so the timer can be paced by what a repaint actually costs on
        // this page. The tick-count backoff further down only knows how many ticks have gone by, so a
        // page needing 4s per frame kept a 200ms timer for its first 150 ticks.
        const unsigned long long t0 = GetTickCount64();
        try { rc = WebCoreLiveTick(rgba->data()); if (rc == 0) { hash = WebCoreGetFrameHash(); pending = WebCoreGetPendingResourceCount(); } } catch (...) { rc = -1000; }
        const unsigned costMs = (unsigned)(GetTickCount64() - t0);
        int rcCopy = rc; unsigned hashCopy = hash; int pendingCopy = pending;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low,
                ref new DispatchedHandler([self, rgba, rcCopy, hashCopy, pendingCopy, mySeq, present, costMs]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->m_liveBusy = false;
                    ++s->m_liveSettleTicks;
                    if (s->m_opSeq != mySeq) return;   // ?????????????????????/??????/??????/?????? ??? ?????????????????????(???????????????)
                    if (!s->m_sessionActive || s->m_loading || s->m_interacting || !s->m_appForeground) return;
                    if (rcCopy != 0) {
                        if (rcCopy == -12 || rcCopy == -14) {   // ????????????:?????? + ????????????
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                            s->StopLiveMode();
                        }
                        return;
                    }
                    if (hashCopy == s->m_lastFrameHash) {        // ????????????:???????????????????????????
                        // Apotheosis: pace the timer by what a repaint actually costs on this page --
                        // but only here, where the expensive tick produced no new pixels at all. That
                        // is the case worth throttling: on hh.ru a tick costs ~4s, so a 200ms timer
                        // asked the single engine thread for 5x more work than it could retire and the
                        // job queue grew without bound, delaying whatever the user did next. While the
                        // page is still changing, throttling instead freezes the animation the user is
                        // waiting for, so the changed branch below undoes it.
                        if (s->m_liveTimer && s->m_liveSettleTicks > 4) {
                            long long want = (costMs > 2000) ? 30000000LL      // >2s per tick -> 1 per 3s
                                           : (costMs > 700)  ? 10000000LL      // >0.7s        -> 1fps
                                           : 0LL;
                            if (want && s->m_liveTimer->Interval.Duration < want) {
                                Windows::Foundation::TimeSpan slow; slow.Duration = want;
                                s->m_liveTimer->Interval = slow;
                            }
                        }
                        if (pendingCopy > 0) {
                            s->m_liveStaticTicks = 0;            // ????????????/???????????????:?????? tick,???????????????????????????
                            int ticks = ++s->m_liveTotalTicks;
                            if (ticks == 150 && s->m_liveTimer) {
                                Windows::Foundation::TimeSpan slow; slow.Duration = 10000000LL;   // 1s ??? 1fps
                                s->m_liveTimer->Interval = slow;
                            }
                            if (ticks >= 300)
                                s->StopLiveMode();
                            return;
                        }
                        if (++s->m_liveStaticTicks >= 40) s->StopLiveMode();   // ~8s ?????? ??? ???,????????????/???????????????
                        return;
                    }
                    s->m_liveStaticTicks = 0;
                    s->m_lastFrameHash = hashCopy;
                    // Apotheosis: new pixels arrived, so something on the page is still moving -- take
                    // back any slow-down the static branch imposed earlier. Without this a page that
                    // had gone quiet (and been throttled to one frame per 3s) kept that rate for the
                    // whole of its next animation, which is how dismissing a cookie banner turned into
                    // a visible twitch followed by seconds of nothing. Not applied past the 150-tick
                    // mark: a page animating that long is a spinner, not a response to a tap.
                    if (s->m_liveTimer && s->m_liveTotalTicks < 150 && s->m_liveTimer->Interval.Duration > 2000000LL) {
                        Windows::Foundation::TimeSpan fast; fast.Duration = 2000000LL;   // back to 200ms
                        s->m_liveTimer->Interval = fast;
                    }
                    if (!present) {
                        auto wb = ref new WriteableBitmap(kW, kH);
                        BlitToBitmap(wb, *rgba, kW, kH);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                    }
                    // ?????????????????????:??????????????? ~150 ???(30s)????????? ??? ?????? ~1fps(?????????,??????????????????);
                    // ????????????/??????/???????????? StartLiveMode ????????????????????? 200ms???
                    if (++s->m_liveTotalTicks == 150 && s->m_liveTimer) {
                        Windows::Foundation::TimeSpan slow; slow.Duration = 10000000LL;   // 1s ??? 1fps
                        s->m_liveTimer->Interval = slow;
                    }
                }));
        } catch (...) {}
    });
}

// ---- ??????????????? ----
void MainPage::OnGo(Platform::Object^, RoutedEventArgs^) { NavigateTo(NormalizeUrl(UrlBox->Text), true); }
void MainPage::OnUrlKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter) {
        // Apotheosis: mark it handled. Without this the Enter keeps bubbling and the same gesture
        // reaches a second navigation path: on the device one tap produced a Click on the action
        // button and this KeyDown 28 ms apart, so every address-bar navigation loaded the page
        // twice. Symbolising the duplicate's backtrace named this handler (frames #2/#3 are
        // KeyEventHandler / KeyRoutedEventArgs), which is how the pair was identified rather than
        // guessed. NavigateTo also drops such a duplicate defensively; this stops it at the source.
        e->Handled = true;
        NavigateTo(NormalizeUrl(UrlBox->Text), true);
    }
}
void MainPage::OnHome(Platform::Object^, RoutedEventArgs^) { NavigateTo(ref new String(g_homeUrl.c_str()), true); }
void MainPage::OnBack(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading || m_navIndex <= 0) return;
    --m_navIndex;
    NavigateTo(ref new String(m_navStack[m_navIndex].c_str()), false);
}
void MainPage::OnForward(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading || m_navIndex >= (int)m_navStack.size() - 1) return;
    ++m_navIndex;
    NavigateTo(ref new String(m_navStack[m_navIndex].c_str()), false);
}
void MainPage::OnMenu(Platform::Object^, RoutedEventArgs^) { ShowActionMenu(); }

// UA ??????:??????/?????????????????? UA ???????????????????????????(??????????????????)
void MainPage::OnToggleUA(Platform::Object^, RoutedEventArgs^)
{
    HideDrawer();
    DoToggleUA();
}

// GPU ????????????(M2):???????????????(????????? g_gpuActive ??? teardown,???????????????)????????? = ????????????
// WebCoreGpuInit(nullptr=??????)?????? ??? g_gpuActive=true ??? ??????????????? ??? buildSession ????????? ??? ???
// TextureMapper ????????????????????????readback ?????????(?????? WriteableBitmap ??????)???????????? gpuinit.txt ??????????????????
// GPU ????????????:???? GPU??<HV/H/V/->(??????????????????????????????????????????)???
static Platform::String^ GpuOrientLabel(int orient)
{
    const wchar_t* tag = (orient == 0) ? L"-" : (orient == 1) ? L"H" : (orient == 2) ? L"V" : L"HV";
    return ref new Platform::String((std::wstring(L"\U0001F5A5 GPU??") + tag).c_str());   // ???? GPU??HV
}

// GPU ????????????(M2):?????? = ???????????? WebCoreGpuInit(??????)??? ????????????????????????(buildSession ????????????
//   ??? TextureMapper ?????? readback ?????????)???????????????????????? = ?????? 4 ??? readback ??????(none/H/V/HV)?????????
//   ???????????????????????????????????????,????????????????????????,?????????(H/V/HV/-)??????????????????????????????????????????
void MainPage::OnToggleGpu(Platform::Object^, RoutedEventArgs^)
{
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);

    if (!m_gpuOn) { HideDrawer(); EnableGpu(); return; }   // ???????????? ??? ????????? EnableGpu(?????????????????????)

    // ??????(???????????? none):?????? = ???????????????????????? ??? ??? LocalState\layertree.txt + ????????????????????????
    //   (scrollPos/contents/view/docBg/usesCompositing),?????????"???????????? / ????????????"???
    HideDrawer();
    WebEngine::instance().post("toggle-gpu", [disp, self]() {
        auto buf = std::make_shared<std::vector<char>>(65536, 0);
        try { WebCoreGpuLayerInfo(buf->data(), (int)buf->size()); } catch (...) {}
        std::string info(buf->data());
        try {
            std::wstring d = LocalStateDir();
            if (!d.empty()) {
                std::ofstream f(WideToUtf8(d) + "\\layertree.txt", std::ios::binary | std::ios::trunc);
                if (f) f.write(info.data(), info.size());
            }
        } catch (...) {}
        std::string head = info.substr(0, info.find('\n'));
        std::wstring headW = Utf8ToWide(head);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, headW]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->TitleText->Text = ref new Platform::String(headW.c_str());   // ??????????????????????????????
                }));
        } catch (...) {}
    });
}

// ---- ?????? ----
void MainPage::OnDrawerClose(Platform::Object^, RoutedEventArgs^) { HideDrawer(); }
void MainPage::OnTabFav(Platform::Object^, RoutedEventArgs^)  { ShowDrawer(DrawerTab::Favorites); }
void MainPage::OnTabHist(Platform::Object^, RoutedEventArgs^) { ShowDrawer(DrawerTab::History); }
void MainPage::OnTabDl(Platform::Object^, RoutedEventArgs^)   { ShowDrawer(DrawerTab::Downloads); }

void MainPage::OnPrimaryAction(Platform::Object^, RoutedEventArgs^)
{
    if (m_tab == DrawerTab::Favorites) {
        ToggleBookmark();   // ??????/??????(??????????????? + ???????????????????????????)
        ActionBtn->Content = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? ref new String(GetStr(m_uiLang, S_ACTION_DEL_FAV).c_str()) : ref new String(GetStr(m_uiLang, S_ACTION_ADD_FAV).c_str());
    } else if (m_tab == DrawerTab::History) {
        m_historyList.clear(); SaveHistory(); RebuildDrawerList();
    } else {
        // ??????:?????????????????????
        if (!m_currentUrl.empty() && m_currentUrl != L"about:home")
            StartDownload(ref new String(m_currentUrl.c_str()));
    }
}

void MainPage::ShowDrawer(DrawerTab tab)
{
    // Apotheosis (MVP, 2026-09-25): drawer (History / Favorites / Downloads) disabled -- it popped up
    // over the page and fought the address bar for taps. Delete with the XAML pass; see PLAN.
    if (true) return;
    m_tab = tab;
    HideSuggestions();
    Drawer->Visibility = Windows::UI::Xaml::Visibility::Visible;
    // ????????????????????????????????????
    if (tab == DrawerTab::Favorites)
        ActionBtn->Content = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl))
            ? ref new String(GetStr(m_uiLang, S_ACTION_DEL_FAV).c_str())
            : ref new String(GetStr(m_uiLang, S_ACTION_ADD_FAV).c_str());
    else if (tab == DrawerTab::History)
        ActionBtn->Content = ref new String(GetStr(m_uiLang, S_ACTION_CLEAR).c_str());
    else
        ActionBtn->Content = ref new String(GetStr(m_uiLang, S_ACTION_DL_PAGE).c_str());
    RebuildDrawerList();
    StopLiveMode();   // ??????????????????,????????????????????????
}
void MainPage::HideDrawer()
{
    Drawer->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();   // ????????????,??????????????????
}

// ?????????????????????(?????? + URL,????????????;??????/????????????)
static Border^ MakeRow(Platform::String^ title, Platform::String^ sub, Color titleColor)
{
    auto sp = ref new StackPanel();
    sp->Margin = Thickness(14, 10, 14, 10);
    auto t = ref new TextBlock();
    t->Text = title; t->FontSize = 20; t->Foreground = ref new SolidColorBrush(titleColor);
    t->TextTrimming = TextTrimming::CharacterEllipsis; t->MaxLines = 1;
    auto u = ref new TextBlock();
    u->Text = sub; u->FontSize = 15; u->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x80, 0x86, 0x8b));
    u->TextTrimming = TextTrimming::CharacterEllipsis; u->MaxLines = 1; u->Margin = Thickness(0, 2, 0, 0);
    sp->Children->Append(t); sp->Children->Append(u);
    auto b = ref new Border();
    b->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x2B, 0x2D, 0x31));
    b->CornerRadius = CornerRadius(10);
    b->Margin = Thickness(0, 0, 0, 8);
    b->Child = sp;
    return b;
}

void MainPage::RebuildDrawerList()
{
    DrawerList->Children->Clear();
    const std::vector<Entry>* list = nullptr;
    if (m_tab == DrawerTab::Favorites) list = &m_bookmarks;
    else if (m_tab == DrawerTab::History) list = &m_historyList;
    else list = &m_downloads;

    if (list->empty()) {
        int emptyStrId = (m_tab == DrawerTab::Favorites) ? S_EMPTY_FAV : (m_tab == DrawerTab::History ? S_EMPTY_HIST : S_EMPTY_DL);
        auto empty = ref new TextBlock();
        empty->Text = ref new String(GetStr(m_uiLang, emptyStrId).c_str());
        empty->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x80, 0x86, 0x8b));
        empty->FontSize = 18; empty->Margin = Thickness(14, 20, 0, 0);
        DrawerList->Children->Append(empty);
        return;
    }

    Platform::Agile<MainPage^> self(this);
    bool isDownloads = (m_tab == DrawerTab::Downloads);
    bool isFav = (m_tab == DrawerTab::Favorites);
    for (size_t i = 0; i < list->size(); ++i) {
        const Entry& e = (*list)[i];
        Platform::String^ titleS = ref new String(e.title.empty() ? e.url.c_str() : e.title.c_str());
        Platform::String^ subS = ref new String((isDownloads ? (e.extra + L"  ??  " + e.url) : e.url).c_str());
        auto row = MakeRow(titleS, subS, ColorHelper::FromArgb(255, 0xF0, 0xF0, 0xF0));

        if (isDownloads) {
            DrawerList->Children->Append(row);
            continue;
        }
        // ????????? ??? ??????
        std::wstring u = e.url;
        auto btn = ref new Button();
        btn->Background = ref new SolidColorBrush(Colors::Transparent);
        btn->BorderThickness = Thickness(0);
        btn->Padding = Thickness(0);
        btn->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->Content = row;
        btn->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideDrawer();
            s->NavigateTo(ref new String(u.c_str()), true);
        });
        DrawerList->Children->Append(btn);

        // ????????????(??????/??????)
        if (isFav) {
            auto del = ref new Button();
            del->Content = ref new String(GetStr(m_uiLang, S_DEL_FAV).c_str());
            del->FontSize = 15;
            del->Background = ref new SolidColorBrush(Colors::Transparent);
            del->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0xD9, 0x30, 0x25));
            del->BorderThickness = Thickness(0);
            del->Margin = Thickness(8, -6, 0, 8);
            del->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
                MainPage^ s = self.Get(); if (!s) return;
                s->m_bookmarks.erase(std::remove_if(s->m_bookmarks.begin(), s->m_bookmarks.end(),
                    [&](const Entry& en) { return en.url == u; }), s->m_bookmarks.end());
                s->SaveBookmarks(); s->RebuildDrawerList();
            });
            DrawerList->Children->Append(del);
        }
    }
}

// ---- ?????? ----
void MainPage::StartDownload(Platform::String^ url)
{
    std::wstring wurl = url->Data();
    // ?????????:URL ????????????(??? query),?????? index.html
    std::wstring fn = wurl;
    size_t q = fn.find(L'?'); if (q != std::wstring::npos) fn = fn.substr(0, q);
    size_t sl = fn.find_last_of(L'/');
    fn = (sl == std::wstring::npos) ? fn : fn.substr(sl + 1);
    if (fn.empty() || fn.find(L'.') == std::wstring::npos) fn = L"index.html";

    std::wstring dlDir = LocalStateDir() + L"\\Downloads";
    CreateDirectoryW(dlDir.c_str(), nullptr);
    // ???????????????:?????????????????????????????? (1)(2)???,????????????????????????????????????
    std::wstring outPath = dlDir + L"\\" + fn;
    if (GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring stem = fn, ext;
        size_t dot = fn.find_last_of(L'.');
        if (dot != std::wstring::npos) { stem = fn.substr(0, dot); ext = fn.substr(dot); }
        for (int i = 1; i < 1000; ++i) {
            std::wstring cand = stem + L"(" + std::to_wstring(i) + L")" + ext;
            std::wstring candPath = dlDir + L"\\" + cand;
            if (GetFileAttributesW(candPath.c_str()) == INVALID_FILE_ATTRIBUTES) { fn = cand; outPath = candPath; break; }
        }
    }

    int uiLang = m_uiLang;
    TitleText->Text = ref new String((GetStr(uiLang, S_DL_STARTED) + L"  " + fn).c_str());
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string u8url = ToUtf8(url);
    std::string u8out = WideToUtf8(outPath);
    std::wstring fnCopy = fn;
    std::wstring urlCopy = wurl;

    // ?????????????????????,????????????????????????(???????????????????????????????????? 120s)???WebCoreDownload ?????????
    // curl_easy ??????,?????? CURLOPT_SHARE????????? WebKit ???????????????,?????????????????????;CA(g_caBytes)
    // ????????????????????? SetupRuntimeEnv ??????(???????????????????????????????????????)???
    std::thread([disp, self, u8url, u8out, fnCopy, urlCopy, uiLang]() {
        int code = WebCoreDownload(u8url.c_str(), u8out.c_str());
        long long sz = 0;
        try { std::ifstream f(u8out, std::ios::binary | std::ios::ate); if (f) sz = (long long)f.tellg(); } catch (...) {}
        std::wstring status = (code >= 200 && code < 400)
            ? (GetStr(uiLang, S_DL_DONE) + L"  " + std::to_wstring(sz / 1024) + L" KB")
            : (GetStr(uiLang, S_DL_FAILED) + std::to_wstring(code) + L")");
        auto st = std::make_shared<std::wstring>(status);
        auto fnC = std::make_shared<std::wstring>(fnCopy);
        auto urlC = std::make_shared<std::wstring>(urlCopy);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, st, fnC, urlC]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    Entry e; e.url = *urlC; e.title = *fnC; e.extra = *st;
                    s->m_downloads.insert(s->m_downloads.begin(), e);
                    if (s->m_downloads.size() > 100) s->m_downloads.resize(100);
                    s->SaveDownloads();
                    s->TitleText->Text = ref new String((*fnC + L"  " + *st).c_str());
                    if (s->Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible && s->m_tab == DrawerTab::Downloads)
                        s->RebuildDrawerList();
                }));
        } catch (...) {}    }).detach();
}

// ============================================================================
// ??????1:?????????(???????????? Go/??????/?????? + ???????????? + ??????/??????????????????)
// ??? UI ???,?????? ContentArea ???????????? / ?????????????????????
// ============================================================================

void MainPage::Reload()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home")
        NavigateTo(ref new String(L"about:home"), false);
    else
        NavigateTo(ref new String(m_currentUrl.c_str()), false);
}

// ????????????:?????????=??????;??????????????????=Go;??????=??????????????????
void MainPage::OnUrlAction(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading) {
        // ??????:??????????????????(opSeq++),????????????,???????????????????????????(?????????????????????,?????? job ??????????????????)???
        ++m_opSeq;
        m_interacting = false;
        if (m_loadWatchdog) m_loadWatchdog->Stop();
        WebEngine::instance().post("close-session", []() { try { WebCoreCloseSession(); } catch (...) {} });
        m_sessionActive = false;
        ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        SetLoading(false);
        TitleText->Text = ref new String(GetStr(m_uiLang, S_CANCELLED).c_str());
        return;
    }
    std::wstring boxText = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    bool pendingEdit = (m_currentUrl == L"about:home") ? !boxText.empty() : (boxText != m_currentUrl);
    HideSuggestions();
    if (pendingEdit) NavigateTo(NormalizeUrl(UrlBox->Text), true);
    else Reload();
}

void MainPage::UpdateUrlActionGlyph()
{
    if (!UrlActionBtn) return;
    if (m_loading) { UrlActionBtn->Content = ref new String(L"\x2715"); return; }   // ??? ??????
    std::wstring boxText = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    bool pendingEdit = (m_currentUrl == L"about:home") ? !boxText.empty() : (boxText != m_currentUrl);
    UrlActionBtn->Content = ref new String(pendingEdit ? L"\x2192" : L"\x21BB");     // ??? Go / ??? ??????
}

// Segoe MDL2 Assets:Lock=E72E,Warning=E7BA?????????/???????????????
void MainPage::UpdateLockIcon()
{
    if (!LockIcon) return;
    const std::wstring& u = m_currentUrl;
    if (u.empty() || u == L"about:home" || u.rfind(L"about:", 0) == 0) {
        LockIcon->Text = ref new String(L"");
    } else if (u.rfind(L"https://", 0) == 0) {
        LockIcon->Text = ref new String(L"\xE72E");
        LockIcon->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x5C, 0xB8, 0x5C));
    } else if (u.rfind(L"http://", 0) == 0) {
        LockIcon->Text = ref new String(L"\xE7BA");
        LockIcon->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0xE0, 0xA0, 0x30));
    } else {
        LockIcon->Text = ref new String(L"");
    }
}

void MainPage::OnUrlChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^)
{
    UpdateUrlActionGlyph();
    if (m_urlSyncing) { HideSuggestions(); return; }   // ????????????????????????(??????/??????):???????????????
    if (!m_urlFocused) { HideSuggestions(); return; }   // ?????????????????????:?????????(??????"??????????????????")
    std::wstring q = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    if (q.empty()) { HideSuggestions(); return; }
    ShowSuggestions(q);
}

void MainPage::OnUrlGotFocus(Platform::Object^, RoutedEventArgs^) { m_urlFocused = true; }
// ?????? LostFocus ????????????:???????????????????????????????????? Click,??????????????????????????????????????????(OnPageTapped)/????????????
void MainPage::OnUrlLostFocus(Platform::Object^, RoutedEventArgs^) { m_urlFocused = false; }

// ?????? + ??????????????????(url/title,???????????????),??????,?????? 8 ????????????????????????
void MainPage::ShowSuggestions(const std::wstring& query)
{
    if (!SuggestPanel || !SuggestList) return;
    SuggestList->Children->Clear();
    std::wstring ql = query;
    std::transform(ql.begin(), ql.end(), ql.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });

    std::vector<Entry> matches;
    std::vector<std::wstring> seen;
    auto consider = [&](const std::vector<Entry>& src) {
        for (const auto& e : src) {
            if (matches.size() >= 8) break;
            std::wstring ul = e.url, tl = e.title;
            std::transform(ul.begin(), ul.end(), ul.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
            std::transform(tl.begin(), tl.end(), tl.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
            if (ul.find(ql) == std::wstring::npos && tl.find(ql) == std::wstring::npos) continue;
            if (std::find(seen.begin(), seen.end(), e.url) != seen.end()) continue;
            seen.push_back(e.url);
            matches.push_back(e);
        }
    };
    consider(m_bookmarks);
    consider(m_historyList);
    if (matches.empty()) { HideSuggestions(); return; }

    Platform::Agile<MainPage^> self(this);
    for (const auto& e : matches) {
        std::wstring u = e.url;
        auto row = MakeRow(ref new String(e.title.empty() ? e.url.c_str() : e.title.c_str()),
                           ref new String(e.url.c_str()),
                           ColorHelper::FromArgb(255, 0xF0, 0xF0, 0xF0));
        auto btn = ref new Button();
        btn->Background = ref new SolidColorBrush(Colors::Transparent);
        btn->BorderThickness = Thickness(0);
        btn->Padding = Thickness(0);
        btn->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->Content = row;
        btn->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideSuggestions();
            s->m_urlSyncing = true; s->UrlBox->Text = ref new String(u.c_str()); s->m_urlSyncing = false;
            s->NavigateTo(ref new String(u.c_str()), true);
        });
        SuggestList->Children->Append(btn);
    }
    SuggestPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::HideSuggestions()
{
    if (SuggestPanel) SuggestPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

// ============================================================================
// ??????2:????????????(?????????????????? action sheet)+ ??????/????????????/??????/UA
// ============================================================================

// Apotheosis: the action-menu rows carry no x:Name. Each is a <Button Tag="..." Click="OnAction">
// with its glyph and label hardcoded inside, so neither can be reached the usual way. These two
// helpers get there by Tag instead, which is both stable across markup edits and independent of the
// XBF connection ids -- and that matters, because MainPage.xaml is effectively frozen: adding an
// x:Name renumbers those ids and MainPage.g.hpp cannot currently be regenerated (Doc/UNIFICATION.md
// section C1). The walk is over the *logical* content tree (Panel::Children, Border::Child,
// ContentControl::Content), not VisualTreeHelper, because ActionMenu starts Collapsed and its visual
// children are not realised until it has been shown and laid out at least once.
static Button^ FindRowByTag(UIElement^ node, Platform::String^ tag)
{
    if (!node) return nullptr;
    if (auto b = dynamic_cast<Button^>(node)) {
        auto bt = dynamic_cast<Platform::String^>(b->Tag);
        if (bt && bt->Equals(tag)) return b;
    }
    if (auto panel = dynamic_cast<Panel^>(node)) {
        for (unsigned i = 0; i < panel->Children->Size; ++i)
            if (auto hit = FindRowByTag(panel->Children->GetAt(i), tag)) return hit;
        return nullptr;
    }
    if (auto border = dynamic_cast<Border^>(node))
        return FindRowByTag(border->Child, tag);
    if (auto cc = dynamic_cast<ContentControl^>(node))
        return FindRowByTag(dynamic_cast<UIElement^>(cc->Content), tag);
    return nullptr;
}

static void SetRowLabel(Button^ row, const wchar_t* glyph, const wchar_t* text)
{
    if (!row) return;
    auto sp = dynamic_cast<Panel^>(row->Content);
    if (!sp || sp->Children->Size < 2) return;
    if (auto g = dynamic_cast<TextBlock^>(sp->Children->GetAt(0))) g->Text = ref new String(glyph);
    if (auto t = dynamic_cast<TextBlock^>(sp->Children->GetAt(1))) t->Text = ref new String(text);
}

void MainPage::ShowActionMenu()
{
    // Apotheosis (MVP, 2026-09-25): the full-screen action sheet is disabled. Every row in it opened a
    // half-working feature (History / Favorites / Settings / Downloads / share) and the scrim overlay
    // itself made stray taps land on menu rows instead of the page. The v1 MVP is the address bar,
    // Back, and the page; the proper fix is to delete these blocks once the XAML glue can be
    // regenerated (see PLAN). Runtime-only: XAML and MainPage.g.hpp are untouched.
    if (true) return;
    HideSuggestions();
    ApplyLanguage();
    if (ActFavLabel)
        ActFavLabel->Text = ref new String(GetStr(m_uiLang, (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? S_BOOKMARKED : S_BOOKMARK).c_str());
    if (ActUaLabel)
        ActUaLabel->Text = ref new String(GetStr(m_uiLang, m_uaMobile ? S_MOBILE_SITE : S_DESKTOP_SITE).c_str());
    // Apotheosis: the Share row is repurposed as the way into the diagnostics page. It is the one row
    // that can be given up without losing a capability -- Copy link already covers "hand this address
    // to someone" -- and the page it now opens is otherwise reachable only by scrolling to the bottom
    // of Settings, which is where the maintainer failed to find it. Done from code, by Tag, because a
    // new row would need a new x:Name and the markup is frozen; see FindRowByTag above. Re-applied on
    // every open so nothing that rewrites labels can undo it. The wording is deliberately what a
    // person with a broken page looks for rather than what a developer would call it; the page keeps
    // its technical title. E7BA is the MDL2 warning glyph.
    SetRowLabel(FindRowByTag(ActionMenu, L"share"), L"", L"Report a problem");
    ActionMenu->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideActionMenu()
{
    ActionMenu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::OnActionScrimTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
    HideActionMenu();   // ????????????????????????
}

void MainPage::OnSheetTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;  // ?????????????????????????????????(???????????????????????????)
}

// ????????????:??? Button.Tag????????????????????????(????????????????????? UI ?????????????????????)???
void MainPage::OnAction(Platform::Object^ sender, RoutedEventArgs^)
{
    std::wstring t;
    auto btn = dynamic_cast<Button^>(sender);
    if (btn) { auto tag = dynamic_cast<Platform::String^>(btn->Tag); if (tag) t = std::wstring(tag->Data()); }
    HideActionMenu();
    if (t == L"reload") Reload();
    else if (t == L"share") { HideActionMenu(); ShowDiagPage(); }   // repurposed row, see ShowActionMenu
    else if (t == L"copylink") DoCopyLink();
    else if (t == L"bookmark") ToggleBookmark();
    else if (t == L"newtab") NewTab();
    else if (t == L"home") NavigateTo(ref new String(g_homeUrl.c_str()), true);
    else if (t == L"ua") DoToggleUA();
    else if (t == L"find") ShowFindBar();
    else if (t == L"download") { if (!m_currentUrl.empty() && m_currentUrl != L"about:home") StartDownload(ref new String(m_currentUrl.c_str())); }
    else if (t == L"bookmarks") ShowDrawer(DrawerTab::Favorites);
    else if (t == L"history") ShowDrawer(DrawerTab::History);
    else if (t == L"downloads") ShowDrawer(DrawerTab::Downloads);
    else if (t == L"settings") ShowSettings();
}

void MainPage::ToggleBookmark()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") return;
    if (IsBookmarked(m_currentUrl)) {
        m_bookmarks.erase(std::remove_if(m_bookmarks.begin(), m_bookmarks.end(),
            [&](const Entry& e) { return e.url == m_currentUrl; }), m_bookmarks.end());
        TitleText->Text = ref new String(GetStr(m_uiLang, S_TOAST_UNBOOKMARKED).c_str());
    } else {
        Entry e; e.url = m_currentUrl; e.title = m_currentTitle.empty() ? m_currentUrl : m_currentTitle;
        m_bookmarks.insert(m_bookmarks.begin(), e);
        TitleText->Text = ref new String(GetStr(m_uiLang, S_TOAST_BOOKMARKED).c_str());
    }
    SaveBookmarks();
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible && m_tab == DrawerTab::Favorites)
        RebuildDrawerList();
}

// Apotheosis: currently unreferenced. The action menu's Share row was repurposed as the entry point
// to the diagnostics page (see ShowActionMenu), because a new row would need a new x:Name and the
// markup is frozen until MainPage.g.hpp can be regenerated. Kept, not deleted: sharing the current
// page is a real capability and should come back as its own row once the markup is editable again.
// Copy link covers the need in the meantime.
void MainPage::DoShare()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") { TitleText->Text = ref new String(GetStr(m_uiLang, S_NO_SHARE).c_str()); return; }
    try { Windows::ApplicationModel::DataTransfer::DataTransferManager::ShowShareUI(); } catch (...) {}
}

void MainPage::DoCopyLink()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") return;
    try {
        auto dp = ref new Windows::ApplicationModel::DataTransfer::DataPackage();
        dp->SetText(ref new String(m_currentUrl.c_str()));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        TitleText->Text = ref new String(GetStr(m_uiLang, S_COPIED).c_str());
    } catch (...) {}
}

void MainPage::DoToggleUA()
{
    m_uaMobile = !m_uaMobile;
    if (UaBtn) UaBtn->Content = ref new String((m_uaMobile ? GetStr(m_uiLang, S_MOBILE_SITE) : GetStr(m_uiLang, S_DESKTOP_SITE)).c_str());
    int mobile = m_uaMobile ? 1 : 0;
    WebEngine::instance().post("set-ua-mobile", [mobile]() { try { WebCoreSetUserAgentMobile(mobile); } catch (...) {} });
    if (!m_currentUrl.empty() && m_currentUrl != L"about:home")
        NavigateTo(ref new String(m_currentUrl.c_str()), false);   // ???????????? UA ??????
}

// ============================================================================
// ??????3:?????????(????????????/??????/??????UA/??????/????????????)+ ???????????? + ????????????
// ============================================================================

void MainPage::ApplySettings()
{
    g_searchPrefix = SearchPrefixFor(m_setSearch);
    if (m_defaultZoom < 50) m_defaultZoom = 50;
    if (m_defaultZoom > 200) m_defaultZoom = 200;
    m_uaMobile = !m_setUaDesktop;
    int mobile = m_uaMobile ? 1 : 0;
    std::string ua = WideToUtf8(m_uaCustom);
    // Apotheosis: push content toggles to the driver for the next session build.
    int js = m_jsEnabled ? 1 : 0;
    int img = m_imagesEnabled ? 1 : 0;
    WebEngine::instance().post("apply-settings", [mobile, ua, js, img]() {
        try { WebCoreSetUserAgentMobile(mobile); } catch (...) {}
        try { WebCoreSetUserAgentString(ua.empty() ? nullptr : ua.c_str()); } catch (...) {}
        try { WebCoreConfigure(js, img); } catch (...) {}
    });
    if (UaBtn) UaBtn->Content = ref new String((m_uaMobile ? GetStr(m_uiLang, S_MOBILE_SITE) : GetStr(m_uiLang, S_DESKTOP_SITE)).c_str());
    // Apotheosis 2026-08-25: post the apply-settings job to the engine queue. The job is queued, not
    // synchronous -- it lands on the engine thread FIFO -- so if it runs AFTER the nav-load job that
    // contains buildSession, the session is already built before WebCoreConfigure() sets the js/img
    // flags, and the session reads them as -1 "not configured" => scripts off. That is exactly the
    // "settings says js=1 but scr=defer=0" white-screen bug. Nothing here changes on the UI thread;
    // the ordering fix is in WebCoreDriver.cpp buildSession, which now reads the flags at session
    // build time rather than relying on the settings job having run first.
}   // end ApplySettings

void MainPage::LoadSettings()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) { ApplySettings(); return; }
    std::ifstream f(WideToUtf8(d) + "\\settings.ini", std::ios::binary);
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "search") m_setSearch = atoi(v.c_str());
            else if (k == "home") g_homeUrl = v.empty() ? L"about:home" : Utf8ToWide(v);
            else if (k == "ua") m_setUaDesktop = (atoi(v.c_str()) != 0);
            else if (k == "zoom") m_defaultZoom = atoi(v.c_str());
            else if (k == "tabmode") m_tabMode = atoi(v.c_str());
            else if (k == "gpudefault") m_gpuDefault = (atoi(v.c_str()) != 0);
            else if (k == "ua_custom") m_uaCustom = Utf8ToWide(v);
            else if (k == "lang") m_uiLang = atoi(v.c_str());
            // Apotheosis: keepawake=1 asks the system not to blank the screen while the browser is in
            // the foreground, through Windows::System::Display::DisplayRequest -- the same mechanism a
            // video player uses, and it needs no manifest capability. It exists for remote testing on
            // the Lumia: the phone blanks after about five minutes, an app that loses foreground focus
            // is suspended and then reaped by the OS, and the log then records `clean-exit` for a run
            // that never drew anything. With this on, one launch from the tile keeps the device
            // testable for as long as it is left running, so the maintainer does not have to stand
            // over it. Off by default, because holding the screen on is wrong for an actual user and
            // right only while debugging.
            else if (k == "keepawake") m_keepAwake = (atoi(v.c_str()) != 0);
            else if (k == "js") m_jsEnabled = (atoi(v.c_str()) != 0);
            else if (k == "images") m_imagesEnabled = (atoi(v.c_str()) != 0);
        }
    }
    if (m_setSearch < 0 || m_setSearch > 3) m_setSearch = 0;
    if (m_uiLang < 0 || m_uiLang > 2) m_uiLang = 1;   // English is the fallback, not Chinese
    ApplySettings();
    ApplyLanguage();
}

void MainPage::SaveSettings()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) return;
    std::string s;
    s += "search=" + std::to_string(m_setSearch) + "\n";
    s += "home=" + (g_homeUrl == L"about:home" ? std::string() : WideToUtf8(g_homeUrl)) + "\n";
    s += "ua=" + std::to_string(m_setUaDesktop ? 1 : 0) + "\n";
    s += "zoom=" + std::to_string(m_defaultZoom) + "\n";
    s += "tabmode=" + std::to_string(m_tabMode) + "\n";
    s += "gpudefault=" + std::to_string(m_gpuDefault ? 1 : 0) + "\n";
    s += "ua_custom=" + WideToUtf8(m_uaCustom) + "\n";
    s += "lang=" + std::to_string(m_uiLang) + "\n";
    s += "keepawake=" + std::to_string(m_keepAwake ? 1 : 0) + "\n";
    s += "js=" + std::to_string(m_jsEnabled ? 1 : 0) + "\n";
    s += "images=" + std::to_string(m_imagesEnabled ? 1 : 0) + "\n";
    std::ofstream f(WideToUtf8(d) + "\\settings.ini", std::ios::binary | std::ios::trunc);
    if (f) f.write(s.data(), s.size());
}

void MainPage::ShowSettings()
{
    // Apotheosis (MVP, 2026-09-25): the Settings page is disabled -- it covered the whole window and
    // was the worst offender for "cannot type a URL". Delete with the XAML pass; see PLAN.
    if (true) return;
    HideActionMenu();
    if (SetSearchCombo) SetSearchCombo->SelectedIndex = m_setSearch;
    if (SetHomeBox) SetHomeBox->Text = ref new String(g_homeUrl == L"about:home" ? L"" : g_homeUrl.c_str());
    if (SetUaSwitch) SetUaSwitch->IsOn = m_setUaDesktop;
    if (SetTabModeSwitch) SetTabModeSwitch->IsOn = (m_tabMode == 1);
    if (SetZoomSlider) SetZoomSlider->Value = m_defaultZoom;
    if (SetZoomLabel) SetZoomLabel->Text = ref new String((std::to_wstring(m_defaultZoom) + L"%").c_str());
    if (SetGpuSwitch) SetGpuSwitch->IsOn = m_gpuDefault;
    if (SetUaCustomBox) SetUaCustomBox->Text = ref new String(m_uaCustom.c_str());
    if (SetLangCombo) SetLangCombo->SelectedIndex = m_uiLang;
    if (VersionText) {
        auto pv = Windows::ApplicationModel::Package::Current->Id->Version;
        std::wstring v = GetStr(m_uiLang, S_SET_ABOUT) + L" " + std::to_wstring(pv.Major) + L"." + std::to_wstring(pv.Minor)
                       + L"." + std::to_wstring(pv.Build) + L"." + std::to_wstring(pv.Revision);
        VersionText->Text = ref new String(v.c_str());
    }
    SettingsPage->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideSettings()
{
    if (SetSearchCombo && SetSearchCombo->SelectedIndex >= 0) m_setSearch = SetSearchCombo->SelectedIndex;
    if (SetHomeBox) {
        std::wstring h = SetHomeBox->Text ? std::wstring(SetHomeBox->Text->Data()) : L"";
        while (!h.empty() && (h.front() == L' ' || h.front() == L'\t')) h.erase(h.begin());
        while (!h.empty() && (h.back() == L' ' || h.back() == L'\t')) h.pop_back();
        if (h.empty() || h == L"about:home") g_homeUrl = L"about:home";
        else { if (h.rfind(L"http", 0) != 0 && h.rfind(L"about:", 0) != 0) h = L"https://" + h; g_homeUrl = h; }
    }
    if (SetUaSwitch) m_setUaDesktop = SetUaSwitch->IsOn;
    if (SetTabModeSwitch) m_tabMode = SetTabModeSwitch->IsOn ? 1 : 0;
    if (SetZoomSlider) m_defaultZoom = (int)(SetZoomSlider->Value + 0.5);
    if (SetGpuSwitch) m_gpuDefault = SetGpuSwitch->IsOn;
    // Apotheosis: push content toggles to the driver for the next navigation.
    try { WebCoreConfigure(m_jsEnabled ? 1 : 0, m_imagesEnabled ? 1 : 0); } catch (...) {}
    if (SetUaCustomBox) {
        std::wstring u = SetUaCustomBox->Text ? std::wstring(SetUaCustomBox->Text->Data()) : L"";
        while (!u.empty() && (u.front() == L' ' || u.front() == L'\t')) u.erase(u.begin());
        while (!u.empty() && (u.back() == L' ' || u.back() == L'\t' || u.back() == L'\r' || u.back() == L'\n')) u.pop_back();
        m_uaCustom = u;
    }
    if (SetLangCombo && SetLangCombo->SelectedIndex >= 0) m_uiLang = SetLangCombo->SelectedIndex;
    ApplySettings();
    ApplyLanguage();
    SaveSettings();
    SettingsPage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::OnSettingsBack(Platform::Object^, RoutedEventArgs^) { HideSettings(); }

void MainPage::OnZoomChanged(Platform::Object^, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e)
{
    if (SetZoomLabel) SetZoomLabel->Text = ref new String((std::to_wstring((int)(e->NewValue + 0.5)) + L"%").c_str());
}

void MainPage::OnLangChanged(Platform::Object^, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^)
{
    if (SetLangCombo && SetLangCombo->SelectedIndex >= 0) {
        m_uiLang = SetLangCombo->SelectedIndex;
        ApplyLanguage();
        SaveSettings();
    }
}

void MainPage::OnSettingsBtn(Platform::Object^ sender, RoutedEventArgs^)
{
    std::wstring t;
    auto b = dynamic_cast<Button^>(sender);
    if (b) { auto tag = dynamic_cast<Platform::String^>(b->Tag); if (tag) t = std::wstring(tag->Data()); }
    if (t == L"clearhist") { m_historyList.clear(); SaveHistory(); TitleText->Text = ref new String(GetStr(m_uiLang, S_TOAST_HIST_CLEARED).c_str()); }
    else if (t == L"clearfav") { m_bookmarks.clear(); SaveBookmarks(); TitleText->Text = ref new String(GetStr(m_uiLang, S_TOAST_FAV_CLEARED).c_str()); }
    else if (t == L"cleardl") { m_downloads.clear(); SaveDownloads(); TitleText->Text = ref new String(GetStr(m_uiLang, S_TOAST_DL_CLEARED).c_str()); }
    else if (t == L"viewlogs") ShowDiagPage();
    else if (t == L"export") ExportDebug();
    else if (t == L"gpu") { HideSettings(); OnToggleGpu(nullptr, nullptr); }
    else if (t == L"checkupdate") CheckForUpdate(true);
}

// ---- ???????????? ----
// ????????????(?????? curl,WebCoreDownload ???????????? Page ??????,?????????????????????)??? GitHub Releases API,
// ???????????? appx ??????(Package.Current)??????????????????????????????????????????????????????????????????????????? appx ????????????
// manual=true:????????????????????????,?????????/???????????????;false:??????????????????,????????????????????????
static bool ParseDottedVersion(const std::string& s, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    int idx = 0; long cur = 0; bool any = false;
    for (size_t i = 0; i <= s.size() && idx < 4; ++i) {
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') { cur = cur * 10 + (s[i] - '0'); any = true; }
        else if (i == s.size() || s[i] == '.') { out[idx++] = (int)cur; cur = 0; if (i == s.size()) break; }
        else break;   // ???????????????(??? tag ??????)??? ???
    }
    return any;
}

void MainPage::CheckForUpdate(bool manual)
{
    if (m_updateChecking) return;
    m_updateChecking = true;
    if (manual) TitleText->Text = ref new String(GetStr(m_uiLang, S_UPDATE_CHECKING).c_str());

    auto pv = Windows::ApplicationModel::Package::Current->Id->Version;
    int cur[4] = { pv.Major, pv.Minor, pv.Build, pv.Revision };
    std::wstring dir = LocalStateDir();
    std::string jsonPath = dir.empty() ? std::string() : WideToUtf8(dir + L"\\update.json");

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::thread([disp, self, manual, jsonPath, cur]() {
        // Apotheosis: whole-body guard. An exception escaping a std::thread's function calls
        // std::terminate -> abort -> instant silent process death (no UEF, no dump on this
        // kernel) -- the exact signature of the deaths under investigation.
        try {
            int rc = -1;
            std::string body;
            if (!jsonPath.empty()) {
                try { rc = WebCoreDownload(
                    "https://api.github.com/repos/Jimmyxiao2009/Project-Apotheosis/releases/latest",
                    jsonPath.c_str()); } catch (...) {}
                if (rc == 200) {
                    std::ifstream f(jsonPath, std::ios::binary);
                    if (f) { std::stringstream ss; ss << f.rdbuf(); body = ss.str(); }
                }
            }
            auto pick = [&](const char* key) -> std::string {
                std::string pat = std::string("\"") + key + "\"";
                size_t p = body.find(pat); if (p == std::string::npos) return {};
                p = body.find(':', p + pat.size()); if (p == std::string::npos) return {};
                size_t a = body.find('"', p); if (a == std::string::npos) return {};
                size_t b = body.find('"', a + 1); if (b == std::string::npos) return {};
                return body.substr(a + 1, b - a - 1);
            };
            std::string tag = pick("tag_name");
            std::string page = pick("html_url");
            bool ok = (rc == 200 && !tag.empty());
            bool newer = false;
            std::string verStr = tag;
            if (!verStr.empty() && (verStr[0] == 'v' || verStr[0] == 'V')) verStr = verStr.substr(1);
            if (ok) {
                int rel[4]; ParseDottedVersion(verStr, rel);
                for (int i = 0; i < 4; ++i) { if (rel[i] != cur[i]) { newer = rel[i] > cur[i]; break; } }
            }
            std::wstring tagW = Utf8ToWide(tag), pageW = Utf8ToWide(page);
            try {
                disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler(
                    [self, manual, ok, newer, tagW, pageW]() {
                        MainPage^ s = self.Get(); if (!s) return;
                        s->m_updateChecking = false;
                        if (!ok) { if (manual) s->TitleText->Text = ref new String(GetStr(s->m_uiLang, S_UPDATE_FAIL).c_str()); return; }
                        if (!newer) { if (manual) s->TitleText->Text = ref new String((GetStr(s->m_uiLang, S_UPDATE_LATEST) + tagW).c_str()); return; }
                        s->TitleText->Text = ref new String((GetStr(s->m_uiLang, S_UPDATE_FOUND) + tagW).c_str());
                        std::wstring target = pageW.empty()
                            ? std::wstring(L"https://github.com/Jimmyxiao2009/Project-Apotheosis/releases/latest")
                            : pageW;
                        try {
                            auto dlg = ref new Windows::UI::Popups::MessageDialog(
                                ref new String((GetStr(s->m_uiLang, S_UPDATE_FOUND) + tagW + L"\n" + GetStr(s->m_uiLang, S_UPDATE_DLG_GO) + L"?").c_str()),
                                ref new String(GetStr(s->m_uiLang, S_UPDATE_DLG_TITLE).c_str()));
                            auto go = ref new Windows::UI::Popups::UICommand(ref new String(GetStr(s->m_uiLang, S_UPDATE_DLG_GO).c_str()));
                            auto later = ref new Windows::UI::Popups::UICommand(ref new String(GetStr(s->m_uiLang, S_UPDATE_DLG_LATER).c_str()));
                            dlg->Commands->Append(go);
                            dlg->Commands->Append(later);
                            dlg->DefaultCommandIndex = 0;
                            dlg->CancelCommandIndex = 1;
                            Platform::Agile<MainPage^> self2(s);
                            concurrency::create_task(dlg->ShowAsync()).then(
                                [self2, go, target](Windows::UI::Popups::IUICommand^ chosen) {
                                    MainPage^ s2 = self2.Get(); if (!s2) return;
                                    if (chosen == go) {
                                        if (s2->SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible) s2->HideSettings();
                                        s2->NavigateTo(ref new String(target.c_str()), true);
                                    }
                                });
                        } catch (...) {}
                    }));
            } catch (...) {}
        } catch (...) {
            // Apotheosis: any escape from the guarded body lands here -- logged, not fatal.
            try {
                std::wstring d = LocalStateDir();
                if (!d.empty()) {
                    std::ofstream ef(WideToUtf8(d + L"\update-thread-error.txt"), std::ios::app);
                    if (ef) ef << LogTimestamp() << " exception on update thread\n";
                }
            } catch (...) {}
        }
    }).detach();
}

// ============================================================================
// Diagnostics viewer
//
// Apotheosis: on a real Lumia there is no debugger (VS 2022 dropped ARM32 device debugging) and an
// App Container hides its LocalState from the user, so reading the engine's own logs on the device
// is otherwise a Device Portal exercise -- which needs the phone on WiFi, and the phone drops WiFi.
// This page is the always-available channel. Clipboard and share cost no manifest capability, so
// nothing here can break installation on the phone.
// ============================================================================

// The single authoritative list of log files, shared by the viewer and by ExportDebug.
// Names must match what the harness and the driver actually write (see CLAUDE.md, "Diagnostics").
// The list ExportDebug used to carry had drifted: it named gpuinit.txt (the file is
// gpuinit-steps.txt), gpuresult.txt and diag.txt (never written by any build), and it omitted
// log.txt and port-trace.txt -- the two most useful logs there are.
// tailKb caps each file inside the full report; glyph.log in particular grows without bound.
// glyph.log is capped hard at 2 KB on purpose. It is per-character tracing (`wt: gdc` / `wt: adv`,
// one or two lines per glyph drawn), so at the 16 KB it used to get it contributed roughly nine
// tenths of an exported report while answering nothing about why a site misbehaved -- verified on a
// real export, 2026-08-17, where 651 KB of glyph log buried log.txt and the diag: line. Two
// kilobytes still shows which font was picked and whether lookups succeeded, which is all the font
// path is ever asked in a bug report. The viewer still opens the whole file for the rare case where
// the font path itself is the suspect.
struct DiagFileDesc { const char* name; unsigned tailKb; };
static const DiagFileDesc kDiagFiles[] = {
    { "log.txt",           128 },   // harness: startup, navigation, [STAGE], the diag: line
    { "ctor-trace.txt",      4 },   // harness: pre-LogInit checkpoints, the only trace of an early death
    { "crashverdict.txt",    2 },   // crash verdict: CRASHED last heartbeat=... / clean-exit
    { "heartbeat.txt",       2 },   // crash verdict: 2 s UI-timer freshness marker
    { "wedgedump.txt",     128 },   // Apotheosis: PC/LR/SP + full stack words of the engine thread, taken by the UI thread on the first stuck beat
    { "crash-report.txt",  128 },   // previous crashed session's log.txt, preserved pre-truncation
    { "port-trace.txt",     64 },   // driver: loader scheduling, settle loop
    { "gpuinit-steps.txt",  64 },   // driver: GPU init / present markers
    { "stage.txt",           4 },   // harness: last stage reached
    { "stage-port.txt",      4 },   // driver: last stage reached
    { "jitresult.txt",       8 },   // JIT probe result
    { "layertree.txt",      64 },   // compositing layer tree dump (written on demand)
    { "imedebug.txt",       32 },   // soft keyboard / text input
    { "autodump.txt",      128 },   // unattended auto-diagnostics run
    { "glyph.log",           2 },   // font and glyph decisions, unbounded; capped hard, see above
};
static const int kDiagFileCount = (int)(sizeof(kDiagFiles) / sizeof(kDiagFiles[0]));

// What a single file shows on screen. Deliberately smaller than the report caps above: a XAML
// TextBox holding hundreds of KB of non-wrapping text is painfully slow on a Lumia, and the tail is
// the part that matters. The full text is still available through Copy / Share / Save.
static const size_t kDiagViewTailBytes = 48 * 1024;

static std::string FormatBytes(unsigned long long n)
{
    char buf[64];
    if (n < 1024ULL) sprintf_s(buf, "%llu B", n);
    else if (n < 1024ULL * 1024ULL) sprintf_s(buf, "%.1f KB", (double)n / 1024.0);
    else sprintf_s(buf, "%.1f MB", (double)n / (1024.0 * 1024.0));
    return std::string(buf);
}

// Read at most maxBytes from the end of a LocalState file. outTotal receives the full size, so the
// caller can say "last 48 KB of 3.2 MB" instead of pretending it showed everything.
std::string MainPage::ReadDiagTail(const char* name, size_t maxBytes, size_t* outTotal)
{
    if (outTotal) *outTotal = 0;
    std::wstring d = LocalStateDir();
    if (d.empty() || !name) return std::string();
    std::ifstream f(WideToUtf8(d) + "\\" + name, std::ios::binary);
    if (!f) return std::string();
    f.seekg(0, std::ios::end);
    const long long total = (long long)f.tellg();
    if (total <= 0) return std::string();
    if (outTotal) *outTotal = (size_t)total;
    const long long off = ((unsigned long long)total > maxBytes) ? total - (long long)maxBytes : 0;
    f.seekg(off, std::ios::beg);
    std::string s;
    s.resize((size_t)(total - off));
    f.read(&s[0], (std::streamsize)s.size());
    s.resize((size_t)f.gcount());
    // Drop the partial first line, so a truncated tail still starts at a record boundary.
    if (off > 0) {
        size_t nl = s.find('\n');
        if (nl != std::string::npos && nl + 1 < s.size()) s.erase(0, nl + 1);
    }
    // A NUL byte would truncate the Platform::String and silently hide the rest of the log.
    for (char& c : s) { if (c == '\0') c = ' '; }
    return s;
}

// Every log in one text, for the clipboard, the share target and the file export.
// Apotheosis: the export routes hand this text to someone else -- the clipboard, a mail client, a
// file on an SD card -- and the report carries every URL visited in the session as well as the
// LocalState path, which embeds the user's account name. Somebody mailing a bug report to a stranger
// should not have to work that out for themselves. The notice rides on DiagStatus, the line directly
// above the Copy / Share / Save buttons, and is appended to every status rather than replacing it, so
// no later message can overwrite it. It leads with a newline so a long status cannot push it off the
// line. Deliberately not a modal dialog: somebody exporting logs for the third time would learn to
// dismiss a dialog without reading it, and this has to stay readable to be honest.
static const wchar_t* kDiagPrivacyNote =
    L"\nThis report includes the addresses of pages visited in this session.";

std::string MainPage::BuildDiagReport()
{
    std::string r = "=== Apotheosis / EdgeHTML Reborn diagnostics report ===\n";
    try {
        auto id = Windows::ApplicationModel::Package::Current->Id;
        auto v = id->Version;
        char buf[128];
        sprintf_s(buf, "version %u.%u.%u.%u\n", (unsigned)v.Major, (unsigned)v.Minor, (unsigned)v.Build, (unsigned)v.Revision);
        r += buf;
        const wchar_t* arch = L"unknown";
        switch (id->Architecture) {
        case Windows::System::ProcessorArchitecture::X86:     arch = L"x86"; break;
        case Windows::System::ProcessorArchitecture::X64:     arch = L"x64"; break;
        case Windows::System::ProcessorArchitecture::Arm:     arch = L"ARM32"; break;
        case Windows::System::ProcessorArchitecture::Neutral: arch = L"neutral"; break;
        default: break;
        }
        r += "arch " + WideToUtf8(arch) + " / harness + WebCore 2.52.4 (webkitgtk) UWP\n";
    } catch (...) {}
    r += "url  " + WideToUtf8(m_currentUrl) + "\n";
    r += "gpu  " + std::string(m_gpuOn ? "on" : "off") + "\n";
    r += "path " + WideToUtf8(LocalStateDir()) + "\n";
    // Apotheosis: one line that answers the most common first question about a dead phone --
    // did the previous session crash, and how long before the process died did it last breathe?
    {
        const std::string v = ReadMarkerFile(L"crashverdict.txt");
        if (!v.empty()) r += "crash " + v + "\n";
    }

    // Apotheosis: the six lines below exist so that a report mailed in from a stranger's phone can be
    // reproduced on the x64 line, which is the only place a bug can be debugged at all now that VS 2022
    // has dropped ARM32 device debugging. Large portals branch hard on the user agent and the locale,
    // layout breaks at particular viewport widths, and a Lumia 950 is not a Lumia 640 -- so without
    // these a log tail says what went wrong but not what to set up. See Doc/X64-AS-ARM-EMULATOR.md for
    // the modelling side. Each group is wrapped separately: one unavailable WinRT property must not
    // cost the whole report.
    {
        // The exact UA string is assembled inside the engine and the C ABI exposes only setters
        // (WebCoreSetUserAgentMobile / WebCoreSetUserAgentString), so report the inputs instead: the
        // mode that was pushed and the override if one is in force. Together with the os/dev lines
        // below that is enough to rebuild the same request.
        r += std::string("ua   ") + (m_uaMobile ? "mobile" : "desktop");
        if (!m_uaCustom.empty())
            r += " override=" + WideToUtf8(m_uaCustom);
        r += "\n";
    }
    try {
        auto langs = Windows::System::UserProfile::GlobalizationPreferences::Languages;
        r += "lang " + std::string(langs->Size ? WideToUtf8(langs->GetAt(0)->Data()) : std::string("(none)"));
        r += " tz=" + WideToUtf8((ref new Windows::Globalization::Calendar())->GetTimeZone()->Data());
        r += " ui=" + std::to_string((int)m_uiLang) + "\n";
    } catch (...) { r += "lang (unavailable)\n"; }
    try {
        auto vi = Windows::System::Profile::AnalyticsInfo::VersionInfo;
        // DeviceFamilyVersion is a decimal string holding four 16-bit fields packed into a ulong.
        const unsigned long long packed = _wcstoui64(vi->DeviceFamilyVersion->Data(), nullptr, 10);
        char osbuf[192];
        sprintf_s(osbuf, "os   %s %u.%u.%u.%u\n", WideToUtf8(vi->DeviceFamily->Data()).c_str(),
                  (unsigned)((packed >> 48) & 0xFFFF), (unsigned)((packed >> 32) & 0xFFFF),
                  (unsigned)((packed >> 16) & 0xFFFF), (unsigned)(packed & 0xFFFF));
        r += osbuf;
    } catch (...) { r += "os   (unavailable)\n"; }
    try {
        // EasClientDeviceInformation needs no manifest capability, which is why it is used here
        // rather than anything under Windows.Devices.
        auto dev = ref new Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation();
        r += "dev  " + WideToUtf8(dev->SystemManufacturer->Data())
           + " / " + WideToUtf8(dev->SystemProductName->Data()) + "\n";
    } catch (...) { r += "dev  (unavailable)\n"; }
    {
        double aw = 0.0, ah = 0.0;
        if (ContentArea) { aw = ContentArea->ActualWidth; ah = ContentArea->ActualHeight; }
        char vbuf[192];
        sprintf_s(vbuf, "view engine %dx%d px, surface %.0fx%.0f dip, zoom %d%% (settings), %s present\n",
                  kW, kH, aw, ah, m_defaultZoom, m_gpuPresent ? "GPU" : "software");
        r += vbuf;
    }
    r += "note This report contains the addresses of pages visited in this session.\n\n";

    int found = 0;
    for (const auto& df : kDiagFiles) {
        size_t total = 0;
        std::string body = ReadDiagTail(df.name, (size_t)df.tailKb * 1024, &total);
        if (!total) continue;
        ++found;
        r += std::string("---------- ") + df.name + " (" + FormatBytes(total);
        if (body.size() < total) r += ", last " + FormatBytes(body.size());
        r += ") ----------\n";
        r += body;
        if (!body.empty() && body.back() != '\n') r += "\n";
        r += "\n";
    }
    if (!found) r += "(no log files in LocalState yet)\n";
    r += "---------- crash dumps ----------\n";
    r += "Minidumps stay in LocalState and are not included here; pull them with Device Portal.\n";
    return r;
}

// Apotheosis: export the LocalState logs as one text file through a FileSavePicker (the picker
// grants write access without any manifest capability, and on a Lumia it can target the SD card,
// which can then simply be pulled out of the phone). The report itself is built by
// BuildDiagReport() so the viewer and the export can never disagree about which logs exist.
void MainPage::ExportDebug()
{
    std::string report = BuildDiagReport();
    std::wstring d = LocalStateDir();
    if (!d.empty()) {
        // Also leave a copy in LocalState so Device Portal can fetch a single file.
        try {
            std::ofstream o(WideToUtf8(d) + "\\debug-report.txt", std::ios::binary | std::ios::trunc);
            if (o) o.write(report.data(), report.size());
        } catch (...) {}
    }
    Platform::String^ reportW = ref new String(Utf8ToWide(report).c_str());
    try {
        auto picker = ref new Windows::Storage::Pickers::FileSavePicker();
        picker->SuggestedStartLocation = Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary;
        picker->SuggestedFileName = ref new String(L"apotheosis-debug");
        auto exts = ref new Platform::Collections::Vector<Platform::String^>();
        exts->Append(".txt");
        picker->FileTypeChoices->Insert(ref new String(L"Text Files"), exts);
        concurrency::create_task(picker->PickSaveFileAsync()).then([reportW](Windows::Storage::StorageFile^ file) {
            if (file) concurrency::create_task(Windows::Storage::FileIO::WriteTextAsync(file, reportW));
        });
        TitleText->Text = ref new String(GetStr(m_uiLang, S_EXPORT_CHOOSE).c_str());
    } catch (...) {
        TitleText->Text = ref new String(GetStr(m_uiLang, S_EXPORT_FAIL).c_str());
    }
}

void MainPage::ShowDiagPage()
{
    HideActionMenu();
    if (!DiagPage) return;
    // The file list lives in kDiagFiles, so fill the picker from there once. Setting SelectedIndex
    // fires OnDiagFileChanged, which is what actually loads the first file.
    if (DiagFileCombo) {
        if (DiagFileCombo->Items->Size == 0) {
            for (const auto& df : kDiagFiles)
                DiagFileCombo->Items->Append(ref new String(ToWide(df.name).c_str()));
            DiagFileCombo->Items->Append(ref new String(L"All logs (report)"));
            DiagFileCombo->SelectedIndex = 0;
        } else {
            LoadDiagFile();
        }
    }
    // DiagPage is declared after SettingsPage in XAML, so it draws on top and Back returns to
    // Settings without any extra bookkeeping.
    DiagPage->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideDiagPage()
{
    if (DiagPage) DiagPage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    // Never let a log body linger and hijack the next "share this page".
    m_pendingShareText.clear();
    StartLiveMode();   // returns early by itself if Settings is still up underneath
}

void MainPage::OnDiagBack(Platform::Object^, RoutedEventArgs^) { HideDiagPage(); }

void MainPage::OnDiagFileChanged(Platform::Object^, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^)
{
    LoadDiagFile();
}

void MainPage::LoadDiagFile()
{
    if (!DiagText || !DiagFileCombo) return;
    const int idx = DiagFileCombo->SelectedIndex;
    std::string body;
    std::wstring status;
    if (idx >= 0 && idx < kDiagFileCount) {
        size_t total = 0;
        body = ReadDiagTail(kDiagFiles[idx].name, kDiagViewTailBytes, &total);
        status = ToWide(kDiagFiles[idx].name) + L" — ";
        if (!total)
            status += L"empty or not written yet";
        else if (body.size() < total)
            status += L"last " + Utf8ToWide(FormatBytes(body.size())) + L" of " + Utf8ToWide(FormatBytes(total));
        else
            status += Utf8ToWide(FormatBytes(total));
    } else {
        // The trailing "All logs (report)" entry: exactly the text Copy / Share / Save produce.
        body = BuildDiagReport();
        const size_t full = body.size();
        const size_t viewCap = 128 * 1024;
        if (full > viewCap) {
            body.erase(0, full - viewCap);
            size_t nl = body.find('\n');
            if (nl != std::string::npos && nl + 1 < body.size()) body.erase(0, nl + 1);
            status = L"all logs — " + Utf8ToWide(FormatBytes(full))
                   + L", showing the last " + Utf8ToWide(FormatBytes(body.size()))
                   + L"; Copy / Share / Save carry all of it";
        } else {
            status = L"all logs — " + Utf8ToWide(FormatBytes(full))
                   + L"; Copy / Share / Save carry exactly this text";
        }
    }
    if (body.empty()) body = "(nothing to show)";
    DiagText->Text = ref new String(Utf8ToWide(body).c_str());
    if (DiagStatus) DiagStatus->Text = ref new String((status + kDiagPrivacyNote).c_str());
}

void MainPage::OnDiagBtn(Platform::Object^ sender, RoutedEventArgs^)
{
    std::wstring t;
    auto b = dynamic_cast<Button^>(sender);
    if (b) { auto tag = dynamic_cast<Platform::String^>(b->Tag); if (tag) t = std::wstring(tag->Data()); }

    if (t == L"refresh") { LoadDiagFile(); return; }
    if (t == L"save")    { ExportDebug(); return; }

    // Copy and share always work on the full report, never on the truncated on-screen tail:
    // half a log pasted into a bug report is worse than no log.
    std::string report = BuildDiagReport();
    if (t == L"copy") {
        bool ok = false;
        try {
            auto dp = ref new Windows::ApplicationModel::DataTransfer::DataPackage();
            dp->SetText(ref new String(Utf8ToWide(report).c_str()));
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
            ok = true;
        } catch (...) {}
        // Flush() makes the content outlive the app; failing to flush is not a failure to copy.
        if (ok) { try { Windows::ApplicationModel::DataTransfer::Clipboard::Flush(); } catch (...) {} }
        if (DiagStatus) {
            const std::wstring msg = ok
                ? GetStr(m_uiLang, S_COPIED) + L" — " + Utf8ToWide(FormatBytes(report.size()))
                : GetStr(m_uiLang, S_EXPORT_FAIL);
            DiagStatus->Text = ref new String((msg + kDiagPrivacyNote).c_str());
        }
    } else if (t == L"share") {
        // Consumed by the DataRequested handler registered in the constructor.
        m_pendingShareText = Utf8ToWide(report);
        try {
            Windows::ApplicationModel::DataTransfer::DataTransferManager::ShowShareUI();
        } catch (...) {
            m_pendingShareText.clear();
            if (DiagStatus) DiagStatus->Text = ref new String((GetStr(m_uiLang, S_EXPORT_FAIL) + kDiagPrivacyNote).c_str());
        }
    }
}

// ============================================================================
// ??????4:????????????(????????? + ?????? WebCoreFindString/Next/Clear)
// ============================================================================

void MainPage::ShowFindBar()
{
    if (!m_sessionActive) { TitleText->Text = ref new String(GetStr(m_uiLang, S_TOAST_CANNOT_FIND).c_str()); return; }
    HideActionMenu();
    HideSuggestions();
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Visible;
    FindCount->Text = ref new String(L"");
    FindBox->Text = ref new String(L"");   // ?????????????????????(??????????????????),??????
    FindBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::OnFindChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^) { DoFind(0); }

void MainPage::OnFindKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter) { e->Handled = true; DoFind(1); }
}

void MainPage::OnFindNext(Platform::Object^, RoutedEventArgs^) { DoFind(1); }
void MainPage::OnFindPrev(Platform::Object^, RoutedEventArgs^) { DoFind(2); }

void MainPage::OnFindClose(Platform::Object^, RoutedEventArgs^)
{
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    FindBox->Text = ref new String(L"");   // ??????????????? ??? ??????????????????
}

// ????????????(??????????????????)???mode:0=??????(????????????+????????????),1=?????????,2=??????????????????=?????????
void MainPage::DoFind(int mode)
{
    if (!m_sessionActive) { if (FindCount) FindCount->Text = ref new String(L""); return; }
    if (m_loading || m_interacting) return;
    std::wstring query = FindBox->Text ? std::wstring(FindBox->Text->Data()) : L"";
    bool clear = (mode == 0 && query.empty());

    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();
    std::string q = WideToUtf8(query);
    bool present = m_gpuPresent;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    WebEngine::instance().post("find", [disp, self, q, mode, clear, present, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try {
            if (clear) rc = WebCoreFindClear(rgba->data());
            else if (mode == 0) rc = WebCoreFindString(q.c_str(), /*matchCase*/ 0, /*wrap*/ 1, rgba->data());
            else rc = WebCoreFindNext(mode == 1 ? 1 : 0, rgba->data());
        } catch (...) { rc = -1000; }
        int rcCopy = rc; int modeCopy = mode; bool clearCopy = clear;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, rcCopy, modeCopy, clearCopy, present, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // ???????????????/???????????????
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    s->SetLoading(false);
                    if (rcCopy < 0) {
                        if (rcCopy == -12 || rcCopy == -14) {   // ????????????
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                        }
                        s->FindCount->Text = ref new String(L"");
                        return;
                    }
                    if (!present) {
                        auto wb = ref new WriteableBitmap(kW, kH);
                        BlitToBitmap(wb, *rgba, kW, kH);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                    }
                    s->m_lastFrameHash = 0;
                    if (clearCopy) s->FindCount->Text = ref new String(L"");
                    else if (modeCopy == 0) s->FindCount->Text = ref new String(rcCopy > 0 ? (std::to_wstring(rcCopy) + GetStr(s->m_uiLang, S_FOUND_N)).c_str() : GetStr(s->m_uiLang, S_FOUND_NONE).c_str());
                    else s->FindCount->Text = ref new String(rcCopy ? L"" : GetStr(s->m_uiLang, S_FOUND_NO_MORE).c_str());
                }));
        } catch (...) {}
    });
}

// ============================================================================
// ??????5:??????(Mode A ????????????)???????????????????????????=????????????;???????????? m_tabs ??????????????????
// ============================================================================

void MainPage::UpdateTabCount()
{
    if (TabCountText) TabCountText->Text = ref new String(std::to_wstring(m_tabs.size()).c_str());
}

void MainPage::SaveActiveTab()
{
    if (m_activeTab < 0 || m_activeTab >= (int)m_tabs.size()) return;
    Tab& t = m_tabs[m_activeTab];
    t.navStack = m_navStack;
    t.navIndex = m_navIndex;
    t.currentUrl = m_currentUrl.empty() ? L"about:home" : m_currentUrl;
    t.currentTitle = m_currentTitle;
    t.pageScale = m_pageScale;
}

void MainPage::RestoreTab(int i)
{
    if (i < 0 || i >= (int)m_tabs.size()) return;
    m_activeTab = i;
    const Tab& t = m_tabs[i];
    m_navStack = t.navStack;
    m_navIndex = t.navIndex;
    m_currentUrl = t.currentUrl;
    m_currentTitle = t.currentTitle;
    m_pageScale = t.pageScale;
    // ???????????????:???????????????????????????,????????? URL ?????????????????????
    ++m_opSeq;
    m_interacting = false;
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    m_loading = false;
    UpdateNavButtons();
    UpdateLockIcon();
    m_urlSyncing = true;
    UrlBox->Text = ref new String(m_currentUrl == L"about:home" ? L"" : m_currentUrl.c_str());
    m_urlSyncing = false;
    NavigateTo(ref new String(m_currentUrl.c_str()), false);
}

void MainPage::NewTab()
{
    SaveActiveTab();
    Tab t; t.currentUrl = g_homeUrl;
    m_tabs.push_back(t);
    m_activeTab = (int)m_tabs.size() - 1;
    // ????????????,????????????,???????????????
    ++m_opSeq;
    m_interacting = false;
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    m_loading = false;
    m_navStack.clear(); m_navIndex = -1;
    m_currentUrl.clear(); m_currentTitle.clear();
    m_pageScale = 1.0f;
    UpdateTabCount();
    NavigateTo(ref new String(g_homeUrl.c_str()), true);
}

void MainPage::CloseTab(int i)
{
    if (i < 0 || i >= (int)m_tabs.size()) return;
    bool wasActive = (i == m_activeTab);
    m_tabs.erase(m_tabs.begin() + i);
    if (m_tabs.empty()) {                      // ?????????:?????????????????????
        Tab t; t.currentUrl = g_homeUrl;
        m_tabs.push_back(t);
        m_activeTab = 0;
        ++m_opSeq; m_interacting = false; if (m_loadWatchdog) m_loadWatchdog->Stop(); m_loading = false;
        m_navStack.clear(); m_navIndex = -1; m_currentUrl.clear(); m_currentTitle.clear(); m_pageScale = 1.0f;
        UpdateTabCount();
        NavigateTo(ref new String(g_homeUrl.c_str()), true);
        return;
    }
    if (m_activeTab >= (int)m_tabs.size()) m_activeTab = (int)m_tabs.size() - 1;
    else if (i < m_activeTab) m_activeTab--;   // ????????????
    UpdateTabCount();
    if (wasActive) RestoreTab(m_activeTab);    // ???????????????????????? ??? ?????????????????????
}

void MainPage::SwitchTab(int i)
{
    if (i == m_activeTab) return;
    SaveActiveTab();
    RestoreTab(i);
}

void MainPage::OnTabs(Platform::Object^, RoutedEventArgs^) { ShowTabSwitcher(); }
void MainPage::OnNewTab(Platform::Object^, RoutedEventArgs^) { HideTabSwitcher(); NewTab(); }
void MainPage::OnTabSwitcherDone(Platform::Object^, RoutedEventArgs^) { HideTabSwitcher(); }

void MainPage::ShowTabSwitcher()
{
    // Apotheosis (MVP, 2026-09-25): tab switcher disabled (full-screen). Delete with the XAML pass.
    if (true) return;
    HideActionMenu();
    HideSuggestions();
    SaveActiveTab();
    RebuildTabSwitcher();
    TabSwitcher->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideTabSwitcher()
{
    TabSwitcher->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::RebuildTabSwitcher()
{
    UpdateTabCount();
    // Apotheosis 2026-09-18: this heading used a destroyed literal (six '?' bytes) and the title had
    // been hardcoded Russian in an otherwise three-language UI. S_TAB_SWITCHER_TITLE is the entry
    // that was already meant for it.
    if (TabSwitcherTitle) TabSwitcherTitle->Text = ref new String((GetStr(m_uiLang, S_TAB_SWITCHER_TITLE) + L" (" + std::to_wstring(m_tabs.size()) + L")").c_str());
    TabList->Children->Clear();
    Platform::Agile<MainPage^> self(this);
    Color blue = ColorHelper::FromArgb(255, 0x3A, 0xA0, 0xFF);
    Color white = ColorHelper::FromArgb(255, 0xF0, 0xF0, 0xF0);
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        int idx = (int)i;
        const Tab& t = m_tabs[i];
        bool active = (idx == m_activeTab);
        std::wstring title = t.currentTitle.empty()
            ? (t.currentUrl == L"about:home" ? std::wstring(L"Chinese") : t.currentUrl)
            : t.currentTitle;
        std::wstring sub = (t.currentUrl == L"about:home") ? std::wstring(L"about:home") : t.currentUrl;

        auto cell = ref new Grid();
        cell->Margin = Thickness(0, 0, 0, 8);

        auto sw = ref new Button();
        sw->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x2B, 0x2D, 0x31));
        sw->BorderThickness = active ? Thickness(2) : Thickness(0);
        sw->BorderBrush = ref new SolidColorBrush(blue);
        sw->Padding = Thickness(0, 0, 40, 0);   // ?????????????????????
        sw->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        sw->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        sw->Content = MakeRow(ref new String(title.c_str()), ref new String(sub.c_str()), active ? blue : white);
        sw->Click += ref new RoutedEventHandler([self, idx](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideTabSwitcher();
            s->SwitchTab(idx);
        });
        cell->Children->Append(sw);

        auto cb = ref new Button();
        cb->Content = ref new String(L"\x2715");
        cb->Background = ref new SolidColorBrush(Colors::Transparent);
        cb->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x9A, 0xA0, 0xA6));
        cb->BorderThickness = Thickness(0);
        cb->Width = 44; cb->Height = 44;
        cb->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
        cb->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        cb->Click += ref new RoutedEventHandler([self, idx](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->CloseTab(idx);
            s->RebuildTabSwitcher();
        });
        cell->Children->Append(cb);

        TabList->Children->Append(cell);
    }
}

// ============================================================================
// ?????? GPU:??? OnToggleGpu ????????????????????????,?????????????????????(GpuInit ??????????????????????????????)???
// ============================================================================
// Apotheosis 2026-09-19: the GPU is a property of the DOCUMENT, so it has to be re-judged when the
// document changes -- judging it once per process leaves the screen showing a page that is no longer
// loaded.
//
// WebCore builds a compositing tree only for pages that need one, so PortChromeClient::rootLayer() is
// null on a plain document. paintToRGBA reads that itself: with no root layer it skips the entire GPU
// block and paints through Cairo into the harness's buffer, which is correct. But the harness, with
// m_gpuPresent still true, does not blit that buffer (`if (!s->m_gpuPresent)` at the live tick and in
// PollEngineFrame) -- so the Cairo pixels go nowhere and the swapchain panel keeps showing the
// PREVIOUS document's frame. Nothing is wrong with either page; the two simply disagree about who owns
// the screen.
//
// Measured on the bench the same day and photographed: the window showed a fully rendered dzen.ru --
// logo, search box, news feed -- while the address bar and the tab title both read
// https://example.com/ / Example Domain, because those come from OnNavDone and were correct. The
// engine's own marker agreed: `[GPU] paint path -> cairo (no root layer, or gpu inactive)`.
//
// Asking costs one ABI call on the engine thread and no composite. m_gpuOn is cleared in the same step
// so that the existing EnableGpu path can arm the GPU again when a later page does have a compositing
// tree -- reusing that path instead of growing a second one. Re-arming costs one composite, and only
// on the navigation that needs it.
void MainPage::ReevaluateGpuForDocument()
{
    Platform::Agile<MainPage^> self(this);
    Windows::UI::Core::CoreDispatcher^ disp = this->Dispatcher;
    WebEngine::instance().post("gpu-check-doc", [self, disp]() {
        // Engine thread. WebCoreEnableCompositing() is `chrome->rootLayer() != nullptr` -- a pointer
        // read, which is why this is cheap enough to do per navigation.
        int ec = 0;
        try { ec = WebCoreEnableCompositing(); } catch (...) { ec = 0; }
        if (ec == 0) {
            // Stop direct presenting too. Safe when the harness was already on software: it is the
            // resting value, and gpuPresent() must never be reached for a document with no root layer.
            try { WebCoreSetDirectPresent(0); } catch (...) {}
        }
        LogWriteF("EnableGpu: document check: EnableCompositing=%d", ec);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, ec, disp]() {
                MainPage^ s = self.Get(); if (!s) return;
                // Apotheosis 2026-09-19: this used to `return` here, and that return is why the
                // SECOND and later GPU pages were laid out at the physical width. A navigation resets
                // the engine's page zoom -- measured on the bench: 2.00 established at 07:40:56 was
                // back at 1.00 by 07:42:24 -- and the only hook the harness had for re-applying it was
                // EnableGpu, which returns at once while m_gpuOn is set. So `dzen.ru/news?n=2`
                // rendered at `contents=2736x24736` and the very next page at `2736x12368`: the same
                // layout at half size, i.e. exactly the "stretched" page the maintainer reported when
                // following links. Re-asserting here covers every navigation, not only the one that
                // arms the GPU. PushPageZoom first, then a resize whose DIP record is cleared so it is
                // not skipped -- the frame for the new document has to be produced by someone, and
                // neither the live tick nor PollEngineFrame blits while m_gpuPresent is set.
                if (ec != 0) {
                    if (!s->m_gpuPresent) return;  // GPU not on for this session -- EnableGpu will arm it
                    s->PushPageZoom();             // the new document lost the factor; put it back
                    s->m_appliedDipW = 0;
                    s->m_appliedDipH = 0;
                    s->ApplyViewportSize();
                    return;
                }
                if (!s->m_gpuPresent) return;    // already on the software path; nothing to hand back
                LogWriteF("EnableGpu: document has no compositing layer -- handing the screen back to software");
                s->m_gpuPresent = false;
                s->m_gpuOn = false;              // let EnableGpu arm the GPU again for a later document
                // g_directPresent is the harness's own "the panel is presenting" flag, and BlitToBitmap
                // silently returns while it is set (`if (g_directPresent.load()) return;`). It is set in
                // EnableGpu's success branch and cleared in its failure branch -- but the hand-back is a
                // THIRD path, and it used to leave the flag set. Measured 0.1.10.22 on the bench: the
                // hand-back logged `handback frame blitted (1024x694 hash=3cf56188)` and the window
                // still showed a flat #F0F0F0, because every one of those blits was being dropped here.
                // A silent early return with a log line claiming success is the worst of both.
                g_directPresent.store(false);
                s->m_lastFrameHash = 0;          // the last GPU frame is not this document's frame
                try { s->GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed; } catch (...) {}
                try { s->RenderImage->Visibility = Windows::UI::Xaml::Visibility::Visible; } catch (...) {}
                try { s->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::Gray); } catch (...) {}
                // The buffer for this document has been painted all along (through Cairo) but never
                // shown: every blit site is `if (!m_gpuPresent)`, and this document was painted while
                // m_gpuPresent was still true, so its pixels were produced and dropped. Measured on the
                // bench: after the hand-back the panel was collapsed, RenderImage was Visible, and the
                // window showed a flat #F0F0F0 -- ContentArea's own background -- because nothing had
                // replaced the empty Source.
                //
                // The fix used to be a dedicated "gpu-handback-frame" job that called WebCoreLiveTick
                // and blitted the result here. 0.1.10.25 replaces it with the ordinary resize, and the
                // reason is the DPI split: in GPU mode the engine viewport and kW/kH are the panel's
                // PHYSICAL pixels and the page carries zoom = CompositionScale, so the frame that job
                // would blit is 2736 px wide and lands 1:1 in a 1368-DIP window -- the left half of a
                // correctly rendered page. The hand-back has to RESIZE back to DIP first, and
                // ApplyViewportSize already does exactly that, blit included: its software branch
                // (`if (!s->m_gpuPresent)`) builds the WriteableBitmap and assigns RenderImage->Source
                // from the same rgba buffer the engine painted.
                //
                // So the hand-back clears the DIP latch to make that resize real. Without it the resize
                // returns at once: the DIP size has not changed, so `w == m_appliedDipW` still holds
                // even though kW/kH are now twice it.
                s->PushPageZoom();               // viewport is about to be DIPs again -> zoom back to 1.0
                s->m_appliedDipW = 0;
                s->m_appliedDipH = 0;
                s->StartLiveMode();
                s->ApplyViewportSize();
            }));
        } catch (...) {}
    });
}

void MainPage::EnableGpu()
{
    LogWriteF("EnableGpu: m_gpuOn=%d m_gpuPresent=%d m_sessionActive=%d", m_gpuOn ? 1 : 0, m_gpuPresent ? 1 : 0, m_sessionActive ? 1 : 0);
    if (m_gpuOn) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    auto props = ref new Windows::Foundation::Collections::PropertySet();
    props->Insert(L"EGLNativeWindowTypeProperty", GpuPanel);
    props->Insert(L"EGLRenderSurfaceSizeProperty",
                  Windows::Foundation::PropertyValue::CreateSize(Windows::Foundation::Size((float)kW, (float)kH)));
    m_gpuProps = props;
    void* win = reinterpret_cast<void*>(reinterpret_cast<IInspectable*>(props));
    // ??????????????????:??? GPU ?????? gpu-crash.flag;??????(?????????????????????)?????????GpuInit ?????????????????????????????????????????????????????????????????????GPU???
    {
        std::wstring fd = LocalStateDir();
        if (!fd.empty()) {
            try { std::ofstream f(WideToUtf8(fd) + "\\gpu-crash.flag", std::ios::binary | std::ios::trunc); if (f) f << "1"; } catch (...) {}
            try { WebCoreSetGpuInitLogFile((WideToUtf8(fd) + "\\gpuinit-steps.txt").c_str()); } catch (...) {}
        }
    }
    WebEngine::instance().post("gpu-init", [disp, self, win]() {
        int rc = -999;
        // Apotheosis: replicate WebCore's exact GPU-init EGL sequence (as in
        // PlatformDisplayWin::create + GLDisplay::create) and log each step, so a
        // hard crash (no exception stream, gpuinit.txt never written) is attributable.
        try {
            std::string eglLog;
            char ebuf[192];
            auto estep = [&](const char* m) { eglLog += m; eglLog += "\n"; };
            // Apotheosis 2026-09-19: THE PROBE RUNS ONCE PER PROCESS AND TERMINATES NOTHING IT DID NOT
            // CREATE. Both halves of that sentence are fixes for one defect, measured the hard way.
            //
            // EnableGpu() is retried on every new url while `!m_gpuOn` (MainPage.xaml.cpp:~3010), and
            // m_gpuOn only becomes 1 when the first frame's WebCoreComposite returns 0. On the bench it
            // never did, so in one session this probe ran FOUR times (23:57:42, 23:57:51, 23:58:16,
            // 23:58:41) -- and each run ended in `eglTerminate` on a display the ENGINE was already
            // using: the EXT+D3D11 display below is the same ANGLE Display object the engine resolves in
            // PlatformDisplayWin::create, because eglGetPlatformDisplay* caches per native display.
            //
            // Measured consequence: exactly the first composite of a session worked and every later one
            // failed `glctx: eglMakeCurrent FAILED err=0x3001` (EGL_NOT_INITIALIZED) -- 228 of 228 `rb:
            // enter` attempts in the 23:58 run, with `rb: makeCurrent=0` and `[GPU] present: no live GL
            // context -- falling back`. The engine's GLDisplay object is still alive and still believes
            // its display is initialized (nothing in WebCore or the port calls eglTerminate), so the
            // only thing that could have done it is this block. Which means the earlier GPU evidence
            // from `-Gpu` bench runs was gathered against a GL context that this probe had killed, and
            // `bt:`/`tm:` probes that never fired (they did fire -- twice, in the windows between a
            // composite and the next probe) were read as "TextureMapper never ran".
            //
            // An EGL display is process-scoped and cheap: leaking one costs nothing (WebCore's own
            // WIN path leaks it deliberately). Terminating one this code did not create costs a session.
            // Do not re-add a terminate here, and do not probe more than once.
            static std::atomic<bool> s_eglProbed { false };
            const bool probeThisTime = !s_eglProbed.exchange(true);
            if (!probeThisTime)
                estep("  EGL probe: skipped -- this process already probed it (EnableGpu is retried per url)");
            if (probeThisTime) {
                typedef EGLDisplay (EGLAPIENTRY* PFN_EGLGETPLATFORMDISPLAY)(EGLenum, void*, const EGLAttrib*);
                // Apotheosis: App-Container loader, for the same reason as icuuc75.dll above -- the
                // GetModuleHandleA / LoadLibraryA pair sits outside WINAPI_PARTITION_APP in SDK 19041
                // and is therefore uncompilable on the ARM line. libEGL.dll is a package payload
                // file, so the leaf name is all LoadPackagedLibrary needs.
                HMODULE hEgl = LoadPackagedLibrary(L"libEGL.dll", 0);
                estep("  EGL probe: hEgl loaded");
                auto pGetPlat = (PFN_EGLGETPLATFORMDISPLAY)GetProcAddress(hEgl, "eglGetPlatformDisplay");
                estep("  EGL probe: eglGetPlatformDisplay addr");
                EGLDisplay dpy = pGetPlat ? pGetPlat(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
                sprintf_s(ebuf, "  EGL probe: eglGetPlatformDisplay(ANGLE, default, nullptr) -> %p err=0x%X", dpy, eglGetError());
                estep(ebuf);
                if (dpy != EGL_NO_DISPLAY) {
                    EGLint mv = 0, nv = 0;
                    EGLBoolean ini = eglInitialize(dpy, &mv, &nv);
                    sprintf_s(ebuf, "  EGL probe: eglInitialize -> %d (v%d.%d) err=0x%X", (int)ini, mv, nv, eglGetError());
                    estep(ebuf);
                    const char* vs = eglQueryString(dpy, EGL_VENDOR);
                    const char* es = eglQueryString(dpy, EGL_EXTENSIONS);
                    sprintf_s(ebuf, "  EGL probe: vendor=%s ext=%s", vs ? vs : "?", es ? es : "?");
                    estep(ebuf);
                    // Apotheosis 2026-09-19: NO eglTerminate here. This display is process-scoped and,
                    // for the EXT+D3D11 branch below, it is literally the one the engine will use -- see
                    // the once-guard and the 0x3001 measurement above.
                    (void)ini;
                } else {
                    // Does the plain (non-EXT) entry work when given the D3D11 type attribs?
                    const EGLAttrib dattrPlain[] = {
                        EGL_PLATFORM_ANGLE_TYPE_ANGLE, (EGLAttrib)EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                        EGL_NONE
                    };
                    EGLDisplay dpyP = pGetPlat ? pGetPlat(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dattrPlain) : EGL_NO_DISPLAY;
                    sprintf_s(ebuf, "  EGL probe: eglGetPlatformDisplay(ANGLE, default, D3D11) -> %p err=0x%X", dpyP, eglGetError());
                    estep(ebuf);
                    if (dpyP != EGL_NO_DISPLAY) {
                        EGLint mv = 0, nv = 0;
                        EGLBoolean ini = eglInitialize(dpyP, &mv, &nv);
                        sprintf_s(ebuf, "  EGL probe: eglInitialize(plain+D3D11) -> %d (v%d.%d) err=0x%X", (int)ini, mv, nv, eglGetError());
                        estep(ebuf);
                        // Apotheosis 2026-09-19: NO eglTerminate -- same display the engine uses.
                        (void)ini;
                    }
                    // Some ANGLE builds export eglGetPlatformDisplayEXT instead.
                    typedef EGLDisplay (EGLAPIENTRY* PFN_EGLGETPLATFORMDISPLAYEXT)(EGLenum, void*, const EGLint*);
                    auto pGetPlatExt = (PFN_EGLGETPLATFORMDISPLAYEXT)eglGetProcAddress("eglGetPlatformDisplayEXT");
                    if (pGetPlatExt) {
                        const EGLint dattr[] = {
                            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                            EGL_NONE
                        };
                        EGLDisplay dpy2 = pGetPlatExt(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dattr);
                        sprintf_s(ebuf, "  EGL probe: eglGetPlatformDisplayEXT(D3D11) -> %p err=0x%X", dpy2, eglGetError());
                        estep(ebuf);
                        if (dpy2 != EGL_NO_DISPLAY) {
                            EGLint mv = 0, nv = 0;
                            EGLBoolean ini = eglInitialize(dpy2, &mv, &nv);
                            sprintf_s(ebuf, "  EGL probe: eglInitialize(EXT) -> %d (v%d.%d) err=0x%X", (int)ini, mv, nv, eglGetError());
                            estep(ebuf);
                            // Apotheosis 2026-09-19: NO eglTerminate -- this is the exact display the
                            // engine resolves in PlatformDisplayWin::create (EXT + D3D11 attribs).
                            (void)ini;
                        }
                    } else {
                        estep("  EGL probe: neither eglGetPlatformDisplay nor EXT exported");
                    }
                }
            }
            try {
                std::wstring dd = LocalStateDir();
                if (!dd.empty()) { std::ofstream f(WideToUtf8(dd) + "\\eglprobe.txt", std::ios::binary | std::ios::trunc); if (f) f.write(eglLog.data(), eglLog.size()); }
            } catch (...) {}
        } catch (...) {}
        try { rc = WebCoreGpuInit(win, kW, kH); } catch (...) { rc = -1000; }
        try {
            std::wstring d = LocalStateDir();
            if (!d.empty()) { std::ofstream f(WideToUtf8(d) + "\\gpuinit.txt", std::ios::binary | std::ios::trunc); if (f) { std::string s = "WebCoreGpuInit(window) rc=" + std::to_string(rc) + "\n"; f.write(s.data(), s.size()); } }
        } catch (...) {}
        LogWriteF("EnableGpu: WebCoreGpuInit returned %d", rc);
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rcCopy]() {
                MainPage^ s = self.Get(); if (!s) return;
                LogWriteF("EnableGpu: callback on UI thread, rcCopy=%d", rcCopy);
                std::wstring d2 = LocalStateDir();   // ????????????=????????? ??? ???????????????
                if (!d2.empty()) { try { DeleteFileW((d2 + L"\\gpu-crash.flag").c_str()); } catch (...) {} }
                if (rcCopy == 0) {
                    // Apotheosis: there is deliberately no re-navigation here. This branch used to call
                    // NavigateTo(m_currentUrl, false) to get the page re-rendered through the
                    // compositor, and on the Lumia that was fatal, 2026-08-20:
                    //   EnableGpu: WebCoreGpuInit returned 0
                    //   NavigateTo: url=https://ya.ru pushHistory=0 loading=1
                    //   NavigateTo: engine owned (loading=1 ...) -> parked https://ya.ru
                    //   after-load url=https://ya.ru rc=0 compositing=1   <- the first load was fine
                    //   NavRetry: replaying parked navigation https://ya.ru
                    //   before-load https://ya.ru                         <- log ends, process gone
                    // The engine was busy, so the reload was parked; when the first load finished
                    // NavRetry replayed it, and the second load of the same page killed the process with
                    // no exception and no dump. `compositing=1` on that first load shows the reload was
                    // not even useful. The underlying second-load defect is tracked separately; not
                    // triggering it on every page is this branch's job.
                    //
                    // The switch to the GPU surface is now CONDITIONAL on a frame actually coming out of
                    // it. It used to be unconditional -- m_gpuOn/m_gpuPresent were set, RenderImage was
                    // hidden and GpuPanel took the screen the moment WebCoreGpuInit returned 0 -- and on
                    // a page with no compositing layers that produces a white screen, because the GPU
                    // path has nothing to draw while the perfectly good software frame has just been
                    // hidden.
                    //
                    // Measured on the device, 2026-08-20, and this is the whole explanation of the
                    // white example.com:
                    //   EnableGpu: WebCoreGpuInit returned 0
                    //   EnableGpu: first frame after init: EnableCompositing=0 Composite=-12
                    //   diag: url=https://example.com/ ... nonwhite=177840/177840 rs=C
                    // EnableCompositing returning 0 means PortChromeClient::rootLayer() is null, and
                    // Composite returning kErrNoSession (-12) means the present path had no session or
                    // no active GPU to draw with. Meanwhile the page was rendered completely. Compare
                    // ya.ru, which logged `compositing=1` at load time and did show content: WebCore
                    // only builds a compositing tree when the page needs one, and a plain static
                    // document like example.com never gets a root layer at all. So "GPU initialised"
                    // and "this document can be presented through the GPU" are different facts, and
                    // only the second one may hide the software surface.
                    //
                    // The present has to happen on the engine thread, and the UI thread must never wait
                    // on it, so the decision is made in a job and marshalled back: keep the software
                    // path unless Composite reports success.
                    Platform::Agile<MainPage^> selfInner(s);
                    Windows::UI::Core::CoreDispatcher^ dispInner = s->Dispatcher;
                    WebEngine::instance().post("gpu-first-frame", [selfInner, dispInner]() {
                        int ec = 0, pc = 0;
                        try { ec = WebCoreEnableCompositing(); } catch (...) {}
                        try { pc = WebCoreComposite(); } catch (...) {}
                        LogWriteF("EnableGpu: first frame after init: EnableCompositing=%d Composite=%d", ec, pc);
                        const bool gpuUsable = (pc == 0);
                        try {
                            dispInner->RunAsync(CoreDispatcherPriority::Normal,
                                ref new DispatchedHandler([selfInner, gpuUsable, ec, pc]() {
                                    MainPage^ t = selfInner.Get(); if (!t) return;
                                    if (gpuUsable) {
                                        t->m_gpuOn = true;
                                        t->m_gpuPresent = true;
                                        g_directPresent.store(true);
                                        // Apotheosis 2026-08-29: tell the DRIVER too, not just the
                                        // harness. g_directPresent above only stops the local blit;
                                        // the driver had no way to know the panel was visible and
                                        // decided by itself, from "a native window exists at all".
                                        WebEngine::instance().post("gpu-direct-on",
                                            []() { try { WebCoreSetDirectPresent(1); } catch (...) {} });
                                        t->RenderImage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                                        t->GpuBtn->Content = GpuOrientLabel(t->m_gpuOrient);
                                        t->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::LimeGreen);
                                        LogWrite("EnableGpu: presenting through the GPU surface");
                                        t->LogViewportMetrics("gpu-on");
                                        // Apotheosis 2026-09-19: the surface GpuInit just created is
                                        // still the size the SOFTWARE session was painting at, i.e. DIPs.
                                        // ANGLE places that surface across the panel's physical pixels
                                        // (CompositionScale 2.00 on the bench), so the page would appear
                                        // magnified 2x and cut off -- measured 0.1.10.24, and the whole
                                        // reason this block exists. Re-run the resize now that
                                        // m_gpuPresent is true: ApplyViewportSize reads the DIP size,
                                        // multiplies by the panel's scale, and drives the EGL surface,
                                        // the engine viewport and kW/kH to the physical size together.
                                        //
                                        // The two calls above this one cannot do it. The
                                        // `s->ApplyViewportSize()` after the GpuInit callback runs while
                                        // m_gpuPresent is still false, and even then both would have
                                        // returned at the early-out: kW/kH already equal the size being
                                        // requested. Clearing the DIP latch is what makes the request
                                        // real. If the engine is mid-load it declines and re-runs on its
                                        // own later with the latch still clear, so the resize is never
                                        // lost -- only deferred.
                                        //
                                        // PushPageZoom first, and it matters that it is first: the
                                        // engine has to know that this viewport is physical pixels
                                        // before the resize gives it one, or the page lays out at
                                        // 2736 CSS px -- see PushPageZoom. Both are posted to the same
                                        // engine thread, so the order they are called in is the order
                                        // they run in.
                                        t->PushPageZoom();
                                        t->m_appliedDipW = 0;
                                        t->m_appliedDipH = 0;
                                        t->ApplyViewportSize();
                                    } else {
                                        // Stay on the software path. This is the normal outcome for a
                                        // page without compositing layers, not a failure to report to
                                        // the user, so the button says GPU is off rather than broken.
                                        t->m_gpuPresent = false;
                                        g_directPresent.store(false);
                                        // Apotheosis 2026-09-19: this path falls back to software, and
                                        // software must run at zoom 1.0. Without the call the engine keeps
                                        // whatever the previous GPU page set -- and since the port now
                                        // re-asserts the requested zoom on every present (see
                                        // apoReassertPageZoom), a stale 2.00 would be enforced, not just
                                        // inherited. PushPageZoom reads m_gpuPresent, cleared just above,
                                        // so this is the 1.0 request.
                                        t->PushPageZoom();
                                        // Apotheosis 2026-08-29: and this is the branch that produced
                                        // the blank window. GpuPanel is collapsed just below, but the
                                        // driver kept taking its direct-present path and returning
                                        // success with the RGBA buffer untouched -- so ya.ru rendered
                                        // into a hidden surface while the visible bitmap stayed zeroed
                                        // (nonwhite=0/710656 at readyState Complete). Clearing the flag
                                        // makes the driver composite and read back into the buffer.
                                        WebEngine::instance().post("gpu-direct-off",
                                            []() { try { WebCoreSetDirectPresent(0); } catch (...) {} });
                                        t->GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                                        t->RenderImage->Visibility = Windows::UI::Xaml::Visibility::Visible;
                                        t->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::Gray);
                                        LogWriteF("EnableGpu: no presentable GPU frame (EnableCompositing=%d Composite=%d) -- staying on software present", ec, pc);
                                    }
                                }));
                        } catch (...) {}
                    });
                    s->ApplyViewportSize();
                } else {
                    s->GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                    s->GpuBtn->Content = ref new String(L"\U0001F5A5 GPU\x2717");
                    s->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::OrangeRed);
                }
            }));
        } catch (...) {}
    });
}

// ===== XAML-generated implementations =====
// Apotheosis: this file used to carry a hand-pasted copy of Generated Files\MainPage.g.hpp --
// InitializeComponent(), a ~490-line Connect() switch and GetBindingConnector(). The XAML compiler
// regenerates that switch, and the connection ids inside it, on every MainPage.xaml change; the
// frozen copy did not. Adding the diagnostics page gave DiagPage id 7 -- previously TabSwitcher,
// also a Grid, so safe_cast accepted it without a word -- and pushed ContentArea from id 76 to 88,
// an id the frozen switch has no case for. LoadComponent then built the whole tree while binding
// the fields to the wrong controls and leaving ContentArea null, and `ContentArea->Children` in
// the constructor is a raw null dereference in C++/CX (not a catchable NullReferenceException), so
// the launch died with an empty LocalState. Including the generated file keeps markup and code in
// lockstep by construction, and a handler whose signature drifts becomes a compile error instead
// of a silent mis-wiring. Two consequences worth knowing:
//   * the generated InitializeComponent() does not swallow a LoadComponent failure the way the
//     hand-written one did -- the constructor's own try/catch now records it and falls through to
//     the code-only fallback UI, which is the same containment with the cause written down;
//   * the orphaned snapshot Src\harness\MainPage.g.hpp was deleted. Its sibling .g.h was already
//     gone, which is why #include "MainPage.g.h" has always resolved to the generated one.
// History worth keeping, from the comment this replaces: LoadComponent used to be skipped
// altogether because XBF loading crashed with 0xc000027b in twinapi.appcore.dll. That had two
// causes -- the null IXamlMetadataProvider of MINIMAL_TEST builds, and the XAML compilation failing
// under SDK 10.0.19041.0 -- both since worked around, so the markup path is live. The second one was
// worked around by moving x64 to SDK 26100 while ARM stayed on 19041 (26100 ships no ARM32
// libraries), and an attempt to unify both on 19041 on 2026-08-17 failed for a verified reason: the
// harness cannot compile against 19041 at all, because 19041 puts LoadLibraryExA outside the APP
// partition. So the two architectures really do run *different* XAML compilers today, which is
// precisely why the generated file has to be included rather than frozen: one hand-pasted Connect()
// could never be correct for both. See Harness.vcxproj and Doc/UNIFICATION.md.
#include "MainPage.g.hpp"

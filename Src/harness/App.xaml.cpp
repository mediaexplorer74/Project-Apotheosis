#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "App.g.h"
#include <windows.h>
#include <cstdio>
#include <ctime>

using namespace Harness;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

// Apotheosis: startup bisection markers. The launch dies with HTTP 500 and an empty LocalState,
// which means the process never reaches App::OnLaunched (where the crash verdict is written), so
// write one file per stage to find how far the process actually gets. Remove once the bug is found.
static void WriteStageMarker(const wchar_t* name)
{
    try {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
        wcscat_s(path, L"\\stage-");
        wcscat_s(path, name);
        wcscat_s(path, L".txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"w") == 0 && f) {
            fprintf(f, "%lld\n", (long long)time(nullptr));
            fclose(f);
        }
    } catch (...) {}
}

App::App()
{
    WriteStageMarker(L"2-appctor");
    InitializeComponent();
    // Apotheosis: without this handler an exception on the UI thread terminates the process with
    // nothing written anywhere -- no log, and on this Win11 build not even a WER report. That is
    // exactly the failure mode we cannot debug on a Lumia, VS 2022 having dropped ARM32 device
    // debugging, so record the HRESULT and message to LocalState and let the process die as before.
    // SetObserved(true) is deliberately NOT called: swallowing the exception would leave a
    // half-constructed UI running and hide the bug.
    UnhandledException += ref new UnhandledExceptionEventHandler(
        [](Platform::Object^, UnhandledExceptionEventArgs^ e) {
            try {
                wchar_t path[MAX_PATH];
                wcscpy_s(path, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
                wcscat_s(path, L"\\unhandled.txt");
                FILE* f = nullptr;
                if (_wfopen_s(&f, path, L"a") == 0 && f) {
                    fprintf(f, "UnhandledException hr=0x%08X msg=%ls\n",
                            (unsigned)e->Exception.Value,
                            e->Message ? e->Message->Data() : L"(none)");
                    fclose(f);
                }
            } catch (...) {}
        });
    // Apotheosis: crash verdict, part 2. OnSuspending is not a C++/CX overridable (C3668), so
    // subscribe to the event exactly like UnhandledException above. Note the delegate lives in
    // Windows::UI::Xaml while its args come from Windows::ApplicationModel. The handler itself is
    // at the bottom of this file and writes exit-ok.txt -- see RunCrashVerdict in MainPage.xaml.cpp.
    Suspending += ref new Windows::UI::Xaml::SuspendingEventHandler(this, &App::OnSuspending);
}

[Platform::MTAThread]
int main(Platform::Array<Platform::String^>^)
{
    WriteStageMarker(L"1-main");
    ::Windows::UI::Xaml::Application::Start(ref new ::Windows::UI::Xaml::ApplicationInitializationCallback(
        [](::Windows::UI::Xaml::ApplicationInitializationCallbackParams^) {
            ref new App();
        }));
    return 0;
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    WriteStageMarker(L"3-onlaunched");
    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
    if (rootFrame == nullptr) {
        rootFrame = ref new Frame();
        Window::Current->Content = rootFrame;
    }
    if (rootFrame->Content == nullptr) {
        // Apotheosis: crash verdict, and must run before MainPage exists -- the engine thread
        // (WebEngine::instance(), created inside the MainPage constructor) runs SetupRuntimeEnvOnce
        // / LogInit, which truncates log.txt. Until this point the previous session's log.txt is
        // still intact, which is the whole point of the check. Defined in MainPage.xaml.cpp.
        RunCrashVerdict();
        // Direct MainPage creation — Frame::Navigate crashes on x64-uwp when
        // the XAML IXamlMetadataProvider returns nullptr. Manual creation is
        // simpler and avoids the broken type-resolution path entirely.
        rootFrame->Content = ref new MainPage();
    }
    Window::Current->Activate();
}

void App::OnSuspending(Platform::Object^, Windows::ApplicationModel::SuspendingEventArgs^)
{
    // Apotheosis: a crash never suspends first, so at the next launch the presence of exit-ok.txt
    // means the previous session reached the orderly teardown path (and its absence means it died).
    // Written with the synchronous FILE API on purpose: Suspending grants only a few seconds and
    // async WinRT file I/O can be cut off mid-write. Note the verdict logic in MainPage.xaml.cpp
    // deletes this file on launch, so it can never carry a stale "clean" verdict from an older,
    // clean session into one that crashed after it.
    try {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
        wcscat_s(path, L"\\exit-ok.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"w") == 0 && f) {
            fprintf(f, "clean-exit %lld\n", (long long)time(nullptr));
            fclose(f);
        }
    } catch (...) {}
}

void App::InitializeComponent()
{
    _contentLoaded = true;
    // App.xbf intentionally NOT loaded — App.xaml has zero resources (empty Application element).
    // The XBF embedding pipeline is unreliable across VS versions; since there's no content,
    // LoadComponent would be a no-op even if the XBF were embedded.
}

::Windows::UI::Xaml::Markup::IXamlType^ App::GetXamlType(::Windows::UI::Xaml::Interop::TypeName type)
{
    auto p = _AppProvider;
    return p ? p->GetXamlTypeByType(type) : nullptr;
}

::Windows::UI::Xaml::Markup::IXamlType^ App::GetXamlType(Platform::String^ fullName)
{
    auto p = _AppProvider;
    return p ? p->GetXamlTypeByName(fullName) : nullptr;
}

Platform::Array<::Windows::UI::Xaml::Markup::XmlnsDefinition>^ App::GetXmlnsDefinitions()
{
    return ref new Platform::Array<::Windows::UI::Xaml::Markup::XmlnsDefinition>(0);
}

::XamlTypeInfo::InfoProvider::XamlTypeInfoProvider^ App::_AppProvider::get()
{
#if defined(MINIMAL_TEST)
    return nullptr;
#else
    if (__provider == nullptr)
        __provider = ref new ::XamlTypeInfo::InfoProvider::XamlTypeInfoProvider();
    return __provider;
#endif
}

// MainPage.minimal.cpp — stub for MinimalTest=true build.
// Strips all WebCore/engine init; builds UI in code (no XAML loading).

#include "pch.h"
#include "MainPage.g.h"

using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Media;
using namespace Harness;

void LogWriteF(const char* fmt, ...) { (void)fmt; }
void LogWrite(const char* s) { (void)s; }

MainPage::MainPage()
{
    _contentLoaded = true;
    auto root = ref new Grid();
    root->Background = ref new SolidColorBrush(Windows::UI::Colors::Black);
    auto tb = ref new TextBlock();
    tb->Text = L"Minimal test — XAML works";
    tb->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
    tb->FontSize = 36;
    tb->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
    tb->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    root->Children->Append(tb);
    this->Content = root;
}

// Stub event handlers (required by MainPage.g.hpp/Connect() linkage)
void MainPage::OnBack(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnForward(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnMenu(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnUrlAction(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnUrlChanged(Platform::Object^, TextChangedEventArgs^) {}
void MainPage::OnUrlKeyDown(Platform::Object^, KeyRoutedEventArgs^) {}
void MainPage::OnUrlGotFocus(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnUrlLostFocus(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnPageTapped(Platform::Object^, TappedRoutedEventArgs^) {}
void MainPage::OnScrollUp(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnScrollDown(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnImageManipDelta(Platform::Object^, ManipulationDeltaRoutedEventArgs^) {}
void MainPage::OnImageManipCompleted(Platform::Object^, ManipulationCompletedRoutedEventArgs^) {}
void MainPage::OnActionScrimTap(Platform::Object^, TappedRoutedEventArgs^) {}
void MainPage::OnSheetTap(Platform::Object^, TappedRoutedEventArgs^) {}
void MainPage::OnAction(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnSettingsBack(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnSettingsBtn(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnZoomChanged(Platform::Object^, RangeBaseValueChangedEventArgs^) {}
void MainPage::OnLangChanged(Platform::Object^, SelectionChangedEventArgs^) {}
void MainPage::OnFindChanged(Platform::Object^, TextChangedEventArgs^) {}
void MainPage::OnFindKeyDown(Platform::Object^, KeyRoutedEventArgs^) {}
void MainPage::OnFindNext(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnFindPrev(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnFindClose(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnTabs(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnNewTab(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnTabSwitcherDone(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnToggleUA(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnToggleGpu(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnDrawerClose(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnTabFav(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnTabHist(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnTabDl(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnPrimaryAction(Platform::Object^, RoutedEventArgs^) {}
void MainPage::OnImeTextChanged(Platform::Object^, TextChangedEventArgs^) {}
void MainPage::OnImeKeyDown(Platform::Object^, KeyRoutedEventArgs^) {}

void MainPage::InitializeComponent()
{
    _contentLoaded = true;
}

void MainPage::Connect(int __connectionId, ::Platform::Object^ __target)
{
    __connectionId;
    __target;
}

::Windows::UI::Xaml::Markup::IComponentConnector^ MainPage::GetBindingConnector(int __connectionId, ::Platform::Object^ __target)
{
    __connectionId;
    __target;
    return nullptr;
}

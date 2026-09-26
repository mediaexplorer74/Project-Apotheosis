/*
 * PortChromeClient.cpp — out-of-line method bodies for WebCorePort::PortChromeClient.
 *
 * Bodies mirror WebCore's EmptyChromeClient (EmptyClients.cpp). The
 * popup-menu factories return nullptr (EmptyChromeClient returns a file-local
 * EmptyPopupMenu/EmptySearchPopupMenu; nullptr is a valid no-op here since the
 * headless port never shows native popups). addMessageToConsole is the one
 * deliberate departure: it reports instead of discarding.
 */

#include "config.h"
#include "PortChromeClient.h"

#include <WebCore/ColorChooser.h>
#include <WebCore/CookieConsentDecisionResult.h>
#include <WebCore/DataListSuggestionPicker.h>
#include <WebCore/DateTimeChooser.h>
#include <WebCore/Icon.h>
#include <WebCore/PopupMenu.h>
#include <WebCore/SearchPopupMenu.h>
// Apotheosis: ChromeClient.h only forward-declares MessageSource/MessageLevel, so naming the
// enumerators needs the real definition.
#include <JavaScriptCore/ConsoleTypes.h>
#include <wtf/CompletionHandler.h>
#include <wtf/text/CString.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace WebCorePort {

// Apotheosis: defined in WebCoreDriver.cpp alongside the walker it shares with the watchdog's
// runaway probe. Declared here rather than in a header because the port's own convention is a local
// declaration per translation unit -- see LoadingFrameLoaderClient.cpp's portDiagLog for the same
// shape.
void portDumpJsStack(const char* tag);

using namespace WebCore;

// Apotheosis: every marker line costs an fopen/fwrite/fclose (see gpuLogMarker), and a page stuck
// in a loop that warns each frame would otherwise grind the device's flash and bury the first,
// interesting messages under thousands of repeats. A load that has not explained itself within this
// many lines is not going to.
static constexpr unsigned kMaxConsoleLines = 400;
static unsigned g_consoleLines = 0;

static const char* consoleLevelName(JSC::MessageLevel level)
{
    switch (level) {
    case JSC::MessageLevel::Log:     return "log";
    case JSC::MessageLevel::Warning: return "warn";
    case JSC::MessageLevel::Error:   return "ERROR";
    case JSC::MessageLevel::Debug:   return "debug";
    case JSC::MessageLevel::Info:    return "info";
    }
    return "?";
}

static const char* consoleSourceName(JSC::MessageSource source)
{
    switch (source) {
    case JSC::MessageSource::XML:                    return "xml";
    case JSC::MessageSource::JS:                     return "js";
    case JSC::MessageSource::Network:                return "network";
    case JSC::MessageSource::ConsoleAPI:             return "console";
    case JSC::MessageSource::Storage:                return "storage";
    case JSC::MessageSource::Rendering:              return "rendering";
    case JSC::MessageSource::CSS:                    return "css";
    case JSC::MessageSource::Accessibility:          return "a11y";
    case JSC::MessageSource::Security:               return "security";
    case JSC::MessageSource::ContentBlocker:         return "contentblocker";
    case JSC::MessageSource::Media:                  return "media";
    case JSC::MessageSource::MediaSource:            return "mediasource";
    case JSC::MessageSource::WebRTC:                 return "webrtc";
    case JSC::MessageSource::ITPDebug:               return "itp";
    case JSC::MessageSource::PrivateClickMeasurement: return "pcm";
    case JSC::MessageSource::PaymentRequest:         return "payment";
    case JSC::MessageSource::Other:                  return "other";
    }
    return "?";
}

void PortChromeClient::addMessageToConsole(JSC::MessageSource source, JSC::MessageLevel level, const String& message, unsigned lineNumber, unsigned columnNumber, const String& sourceID)
{
    if (g_consoleLines >= kMaxConsoleLines) {
        if (g_consoleLines == kMaxConsoleLines) {
            ++g_consoleLines;
            portDiagLog("[JS] ---- console log capped, further messages dropped ----");
        }
        return;
    }
    ++g_consoleLines;

    // Only the tail of a bundle URL is useful and they are routinely hundreds of characters of
    // cache-busting hash, so keep the last path segment. Truncate the message for the same reason:
    // a stack trace spliced into one line still names the throwing function first.
    const CString msg = message.utf8();
    CString src = sourceID.utf8();
    const char* srcText = src.data() ? src.data() : "";
    if (const char* slash = std::strrchr(srcText, '/'))
        srcText = slash + 1;

    char line[1024];
    std::snprintf(line, sizeof(line), "[JS] %s/%s %s:%u:%u %.700s",
        consoleSourceName(source), consoleLevelName(level),
        srcText[0] ? srcText : "-", lineNumber, columnNumber,
        msg.data() ? msg.data() : "");
    portDiagLog(line);

    // Apotheosis: self-test for the JS-stack probe (WebCorePort::portDumpJsStack, WebCoreDriver.cpp).
    //
    // The probe that matters is the one inside the watchdog callback, and that one can only fire
    // during a runaway -- so its mechanism would stay unverified until the single run it is needed
    // for, and "no output" would be indistinguishable from "the freeze did not happen". This hook
    // reaches the same walker from a healthy page: FrameConsoleClient::addMessage calls
    // addMessageToConsole with JSExecState::currentState() on the stack, so real JavaScript frames
    // are there to be named.
    //
    // Off unless LocalState\jstack.txt exists. A file rather than a build flag because the switch
    // has to work against an already-installed build, and because Device Portal cannot write
    // LocalState on the Lumia. Fires at most three times: enough to show a stack, not enough to
    // bury the trace.
    static int s_jstackState = -1;   // -1 unchecked, 0 absent, 1 armed
    static int s_jstackFires = 0;
    if (s_jstackState < 0) {
        s_jstackState = 0;
        if (const char* f = getenv("FONTCONFIG_FILE")) {
            std::string p(f);
            auto slash = p.rfind('\\');
            if (slash != std::string::npos) {
                p = p.substr(0, slash + 1) + "jstack.txt";
                if (FILE* probe = fopen(p.c_str(), "rb")) {
                    fclose(probe);
                    s_jstackState = 1;
                    std::string banner = "jstack: armed by " + p;
                    portDiagLog(banner.c_str());
                }
            }
        }
    }
    if (s_jstackState == 1 && s_jstackFires < 3) {
        ++s_jstackFires;
        WebCorePort::portDumpJsStack("jstack");
    }
}

RefPtr<PopupMenu> PortChromeClient::createPopupMenu(PopupMenuClient&) const
{
    return nullptr;
}

RefPtr<SearchPopupMenu> PortChromeClient::createSearchPopupMenu(PopupMenuClient&) const
{
    return nullptr;
}

RefPtr<ColorChooser> PortChromeClient::createColorChooser(ColorChooserClient&, const Color&)
{
    return nullptr;
}

RefPtr<DataListSuggestionPicker> PortChromeClient::createDataListSuggestionPicker(DataListSuggestionsClient&)
{
    return nullptr;
}

RefPtr<DateTimeChooser> PortChromeClient::createDateTimeChooser(DateTimeChooserClient&)
{
    return nullptr;
}

void PortChromeClient::setTextIndicator(RefPtr<TextIndicator>&&) const
{
}

void PortChromeClient::updateTextIndicator(RefPtr<TextIndicator>&&) const
{
}

void PortChromeClient::runOpenPanel(LocalFrame&, FileChooser&)
{
}

void PortChromeClient::showShareSheet(ShareDataWithParsedURL&&, CompletionHandler<void(bool)>&&)
{
}

void PortChromeClient::requestCookieConsent(CompletionHandler<void(CookieConsentDecisionResult)>&& completion)
{
    completion(CookieConsentDecisionResult::NotSupported);
}

RefPtr<Icon> PortChromeClient::createIconForFiles(const Vector<String>& /* filenames */)
{
    return nullptr;
}

// Shape detection stubs
RefPtr<WebCore::ShapeDetection::BarcodeDetector> PortChromeClient::createBarcodeDetector(const WebCore::ShapeDetection::BarcodeDetectorOptions&) const
{
    return nullptr;
}

void PortChromeClient::getBarcodeDetectorSupportedFormats(CompletionHandler<void(Vector<WebCore::ShapeDetection::BarcodeFormat>&&)>&& completion) const
{
    completion({ });
}

RefPtr<WebCore::ShapeDetection::FaceDetector> PortChromeClient::createFaceDetector(const WebCore::ShapeDetection::FaceDetectorOptions&) const
{
    return nullptr;
}

RefPtr<WebCore::ShapeDetection::TextDetector> PortChromeClient::createTextDetector() const
{
    return nullptr;
}

} // namespace WebCorePort

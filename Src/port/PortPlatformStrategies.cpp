// ============================================================================
// PortPlatformStrategies.cpp  (compiles in port/, links against WebCore.lib)
//
// Minimal PlatformStrategies for the in-process render path. createLoaderStrategy()
// returns PortLoaderStrategy below: WebKitLegacy is not compiled for WinUWP, so we do
// not get WebResourceLoadScheduler and provide our own minimal scheduler that drives
// ResourceLoader::start() -> ResourceHandle (curl). Everything else here is the
// smallest thing that links.
//
// install once from WebCoreDriver init:  installPortPlatformStrategies();
//
// The PortBlobRegistry forwarder is a verbatim copy of WebKitLegacy's
// WebBlobRegistry (Source/WebKitLegacy/mac/WebCoreSupport/WebPlatformStrategies.mm):
// thin forwarders to an owned BlobRegistryImpl.
// ============================================================================

#include "config.h"

#include <cstring>   // Apotheosis: std::strrchr, for the leaf name in the per-request loader trace
#include <WebCore/BlobRegistry.h>
#include <WebCore/BlobRegistryImpl.h>
#include <WebCore/CachedResource.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/MediaStrategy.h>
#include <WebCore/Page.h>
#include <WebCore/PlatformStrategies.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceLoadPriority.h>
#include <WebCore/ResourceLoader.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SubresourceLoader.h>
#include <wtf/Deque.h>
#include <wtf/FastMalloc.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

#include <array>
#include <memory>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cerrno>    // Apotheosis: errno/strerror in the one-shot report on the file sink
#include <string>

using namespace WebCore;

namespace WebCorePort {

// Apotheosis: local diagnostic tracer, appending to the same LocalState\port-trace.txt the
// driver uses. WebCoreDriver.cpp has an identical WebCorePortTrace(), but it sits in an
// anonymous namespace (internal linkage) and cannot be shared across translation units.
// Duplicated here rather than hoisted into a shared PortTrace.h because recompiling
// WebCoreDriver.cpp costs minutes on the build machine and this file costs seconds; unify the
// two the next time that file has to be rebuilt anyway. Loads are the one thing we cannot
// observe from the diag string: `res:[news.css(s1)]` tells us a resource is Pending but not
// whether a SubresourceLoader was ever created for it, nor whether it later failed.

// Apotheosis 2026-09-18 (ST-5): the second, proven channel. portDiagLog is defined in
// WebCoreDriver.cpp as `{ gpuLogMarker(text); }` -- one open/write/flush/close per line into
// LocalState\gpuinit-steps.txt, the file that has survived every crash and hang this project has
// had. Declared locally rather than by including PortChromeClient.h, which pulls in the whole of
// ChromeClient.h for one function; the driver declares portLoaderMark the same way for the same
// reason. If that signature ever changes, change it in both places.
void portDiagLog(const char* utf8);

// Apotheosis 2026-09-18 (ST-5): why this function has two sinks.
//
// Measured on 0.1.9.99 and 0.1.9.100: port-trace.txt held ~1700 lines and ZERO `loader:` ones,
// while the same session's diag showed Hacker News with five subresources at status Cached and
// nonwhite=587134/710656. Every other writer of that same file was present in it (`rl:`, `settle:`,
// `pump:`, `tail:`, `spa:`), and the trace strings are demonstrably inside the installed
// Harness.exe. So the file, its path resolution and the link are all fine -- and yet the one thing
// we most need to see, the engine's own load traffic, is blank. That blank was believed once: it
// made Src/tools/analyze-loader-trace.ps1 print "the scheduler is exonerated" from an empty input.
//
// Rather than keep guessing which half is broken, the low-volume categories go to gpuinit-steps.txt
// as well. That splits the remaining possibilities apart in a single build:
//   * lines appear in gpuinit-steps.txt and not in port-trace.txt -> the call sites are reached and
//     the defect is this function's own stdio/path use;
//   * lines appear in neither -> the call sites are not reached at all, and the question becomes
//     who does serve the engine's loads.
// The per-request +/- pairs stay file-only: they are the bulk (hundreds per page) and would bury
// the marker channel that this diagnostic is trying to read.
// Apotheosis 2026-09-18 (ST-5): one report about the file sink, ever. It answers the question the
// empty trace could not: did this translation unit try to write, and what came back. Only the first
// outcome is reported -- later ones cannot differ, and the marker channel is not for bulk.
static void portLoaderSinkReportOnce(const std::string& what)
{
    static bool done = false;
    if (done)
        return;
    done = true;
    portDiagLog(what.c_str());
}

static void portLoaderTraceV(bool alsoDiag, const char* fmt, va_list args)
{
    if (!fmt)
        return;
    char buf[512];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(buf, sizeof buf, fmt, copy);
    va_end(copy);

    if (alsoDiag)
        portDiagLog(buf);

    // Apotheosis 2026-09-18 (ST-5): the first attempt at the file sink reports its own outcome,
    // because "wrote nothing" and "had nothing to write" are indistinguishable from the outside --
    // and that indistinguishability already cost this project a false verdict.
    const char* fontConfigFile = getenv("FONTCONFIG_FILE");
    if (!fontConfigFile) {
        portLoaderSinkReportOnce("PT: sink SKIPPED -- FONTCONFIG_FILE unset in this translation unit");
        return;
    }
    std::string path(fontConfigFile);
    auto pos = path.rfind('\\');
    if (pos == std::string::npos) {
        portLoaderSinkReportOnce("PT: sink SKIPPED -- no directory separator in FONTCONFIG_FILE");
        return;
    }
    path = path.substr(0, pos + 1) + "port-trace.txt";

    // Apotheosis 2026-09-18 (ST-5): mirror the driver's proven WebCorePortTrace exactly -- plain
    // fopen(.., "ab") with fflush before fclose, not fopen_s. This is not superstition: on 0.1.9.101
    // this very function printed a full FONTCONFIG_FILE through the marker channel and put 35
    // loader: lines into gpuinit-steps.txt, while port-trace.txt -- written by six other translation
    // units in the same session -- received none of them. Call sites, environment and bridge are
    // therefore all proven good, and the fault is inside the lines below. If it survives this
    // rewrite, the report below names the path and the errno instead of leaving a mystery.
    FILE* file = fopen(path.c_str(), "ab");
    if (!file) {
        portLoaderSinkReportOnce("PT: SINK FAILED path=" + path + " errno=" + std::to_string(errno)
            + " (" + std::strerror(errno) + ")");
        return;
    }
    portLoaderSinkReportOnce("PT: sink ok path=" + path);
    fputs(buf, file);
    fputc('\n', file);
    fflush(file);
    fclose(file);
}

// Bulk channel: one line per request, file only.
static void portLoaderTrace(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    portLoaderTraceV(false, fmt, args);
    va_end(args);
}

// Decisive channel: low-volume events, written to both sinks.
static void portLoaderTraceBoth(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    portLoaderTraceV(true, fmt, args);
    va_end(args);
}

// Apotheosis 2026-09-18 (ST-5): the first few arrivals get written to both sinks -- enough to
// prove the strategy is live, which is decided by the very first resource -- and the rest go to
// the file only, so that a 724-resource page cannot bury the marker channel this diagnostic is
// read from. The count is per process, not per page: the question is answered once.
static unsigned g_loaderCallTraceBudget = 5;

// Apotheosis: session generation, for telling live loader traffic from traffic that outlived the
// session it belonged to.
//
// The driver rebuilds Page/Frame/FrameView on every navigation (teardownSession then buildSession),
// but a load is not owned by the driver: the curl port parses responses on a worker thread and
// marshals the result back through the main RunLoop. Between loads nothing pumps that RunLoop --
// the engine thread is asleep on its own job queue -- so a response that arrives after
// WebCoreSessionLoad returned sits in the queue and only runs when something next cycles the loop:
// the next pumpLoop, a pumpQuick, or teardownSession's own four cycles. Whether any of it then
// refers to objects that are already gone is exactly what could not be measured, because every
// loader line in the trace looked alike.
//
// So the counter is bumped once per teardown and stamped on every +/- line. A `+` at one generation
// paired with its `-` at a later one is a request that crossed a session boundary; that pairing is
// the evidence, and it costs three characters per line.
static unsigned g_loaderGeneration = 0;

// Apotheosis: the LoaderStrategy for builds without WebKitLegacy (WinUWP).
//
// Everything here except the scheduling machinery is still the smallest thing that links.
// loadResource() is not optional though: it is the single hook WebCore's CachedResourceLoader
// uses for *every* CachedResource -- stylesheets, scripts, images, fonts, XHR/fetch, and the main
// resource of a navigation as well (DocumentLoader::loadMainResource reaches it through
// requestMainResource; corrected 2026-09-18, see the CALL line below). While its
// body was empty, the damage was invisible but total:
//
//   * CachedResourceLoader asked for news.css and got no SubresourceLoader back (the
//     completion handler was not even invoked), so the CachedResource stayed at
//     CachedResource::Pending forever;
//   * a pending *render-blocking* stylesheet keeps Document holding
//     VisualUpdatesPreventedReason::RenderBlocking;
//   * while any reason is held, RenderLayer::shouldSuppressPaintingLayer() returns true and
//     WebCore paints NOTHING -- not the text, not even the body background.
//
// That is the whole story behind "every real site shows an empty window". The main document
// loaded fine (the driver fetches it with curl itself and feeds it through DocumentWriter),
// but nothing *inside* the page could ever load, so nothing could ever be drawn. It was
// never a font, JIT or compositing problem -- diag said title=Hacker News,
// contents=720x1828, and still nonwhite=0.
//
// The first version of this class started every load the moment it was handed to us. That is
// enough for correctness and it is how Hacker News first rendered, but it is not enough to be
// usable: https://aresluna.org/the-hardest-working-font-in-manhattan/ asks for 724
// subresources, almost all of them AVIF images on one host, and starting 724 concurrent
// libcurl transfers against a single server means every one of them crawls -- including the
// stylesheet the first paint is blocked on. Navigation took minutes. So this is now a real
// scheduler, deliberately modelled on WebKitLegacy's WebResourceLoadScheduler (which exists in
// the tree at Source/WebKitLegacy/WebCoreSupport/ but is not compiled for this port):
//
//   * requests are bucketed per host and served highest-priority-first, so a render-blocking
//     stylesheet goes out before 700 images that only matter once the user scrolls;
//   * at most maxRequestsInFlightPerHost transfers per host are in flight, the rest queue --
//     this is the HTTP/1.1 connection limit every browser applies, not politeness;
//   * a saturated host stops the pass entirely (upstream returns rather than breaks): if the
//     host cannot take another connection, no lower-priority bucket can start either;
//   * VeryLow (speculative/prefetch) waits until its host is completely idle.
//
// What is intentionally *not* copied from upstream: the deferred-request restart in
// setDefersLoading(), plugin stream loads, ping loads and preconnects -- none apply here.
static constexpr unsigned maxRequestsInFlightPerHost = 6;
// Apotheosis 2026-09-24: the ceiling the scheduler did not have, and the reason the two constants
// below exist. Measured on habr.com/ru/feed/: `loader: serve started=208 inflight=6 pending=65
// hosts=1` -- six transfers in flight, sixty-five queued, one host -- and it stayed there. Nothing
// retired, so `remove()` never ran, so nothing was ever served again: not the render-blocking
// light-v2.css, not anything. The document kept `body=0`, the parser waited on a stylesheet that
// was never dispatched, and the user got the harness's empty-page notice on a popular site.
//
// Two independent holes, so two independent fixes:
//
//   * priority was only a QUEUE ORDER, and an order is worthless when nothing is dequeued. A
//     saturated host refused even a request the page cannot render without. Measured in
//     DefaultResourceLoadPriority.cpp:30 -- CSSStyleSheet / Script / JSON are High and
//     ImageResource is Low, so on habr the congestion was Low and the starved resource was High.
//     Exceeding the cap by a bounded margin for High and above costs the anti-congestion property
//     nothing (the 724-image page that motivated the cap is all Low images) and unblocks the parser
//     even while the bulk is stuck.
//   * the port had no way to make progress on its own: every recovery path ran through remove(),
//     i.e. through a transfer reaching a terminal state. `saturatedHostCeiling` removes that
//     dependency -- a host that has retired nothing for this long while at its cap with work queued
//     has one in-flight request cancelled to free the slot. It is a last resort, and it is loud.
static constexpr unsigned criticalPriorityOverflow = 2;
static constexpr Seconds saturatedHostCeiling { 20 };
// How often the stall check re-arms itself while some host is saturated. A stalled page schedules
// nothing by itself, so the check cannot live only in serve() -- see scheduleStallCheck().
static constexpr Seconds saturatedHostRecheck { 5 };

// Apotheosis: data:, blob: and about: share one bucket. They complete without touching the
// network, so the limit only exists to stop a pathological page from starting thousands of
// synchronous loads inside one run-loop turn. Upstream uses the same number.
static constexpr unsigned maxRequestsInFlightForNonHTTPProtocols = 20;

class PortLoaderStrategy final : public LoaderStrategy {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PortLoaderStrategy);
public:
    PortLoaderStrategy() = default;

private:
    // Apotheosis: per-host queues and in-flight set (upstream's HostInformation).
    class HostState {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(HostState);
    public:
        HostState(const String& name, unsigned maxRequestsInFlight)
            : m_name(name)
            , m_maxRequestsInFlight(maxRequestsInFlight)
        {
        }

        const String& name() const { return m_name; }

        void schedule(ResourceLoader& loader, ResourceLoadPriority priority)
        {
            m_pending[indexOf(priority)].append(RefPtr<ResourceLoader> { &loader });
        }

        void addLoadInProgress(ResourceLoader& loader)
        {
            m_loading.add(RefPtr<ResourceLoader> { &loader });
        }

        bool remove(ResourceLoader& loader)
        {
            RefPtr<ResourceLoader> key { &loader };
            if (m_loading.remove(key)) {
                // Apotheosis 2026-09-24: the only place a slot is freed, so it is the only honest
                // place to record that this host made progress. The stall ceiling reads it.
                m_lastRetirement = MonotonicTime::now();
                return true;
            }
            // Cancelled before it ever started: it is still sitting in one of the queues.
            for (auto& queue : m_pending) {
                for (auto it = queue.begin(), end = queue.end(); it != end; ++it) {
                    if (*it == key) {
                        queue.remove(it);
                        return false;
                    }
                }
            }
            return false;
        }

        bool hasRequests() const
        {
            if (!m_loading.isEmpty())
                return true;
            for (auto& queue : m_pending) {
                if (!queue.isEmpty())
                    return true;
            }
            return false;
        }

        bool atLimit(ResourceLoadPriority priority) const
        {
            // Apotheosis: VeryLow means speculative -- a prefetch, or an image far below the
            // fold. It must never compete with something the page needs now, so it only goes
            // out when the host is completely idle. Same rule as upstream.
            if (priority == ResourceLoadPriority::VeryLow && !m_loading.isEmpty())
                return true;
            // Apotheosis 2026-09-24: High and above may exceed the cap by a bounded margin, so a
            // render-blocking stylesheet or a parser-blocking script still goes out while the
            // host is saturated with stuck Low-priority transfers. See the note above
            // criticalPriorityOverflow. The margin is what keeps this bounded: the loop in
            // serveHost() stops as soon as the (raised) limit is reached, so a page cannot
            // accelerate into unbounded concurrency on the strength of its priorities.
            unsigned limit = m_maxRequestsInFlight;
            if (priority >= ResourceLoadPriority::High)
                limit += criticalPriorityOverflow;
            return m_loading.size() >= limit;
        }

        // Apotheosis 2026-09-24: the precondition for the stall ceiling -- at the cap AND with
        // something actually waiting behind it. A host at the cap with an empty queue is just a
        // busy host, and cancelling a transfer to help nothing would be worse than waiting.
        bool atCapWithWorkPending() const
        {
            if (m_loading.size() < m_maxRequestsInFlight)
                return false;
            for (auto& queue : m_pending) {
                if (!queue.isEmpty())
                    return true;
            }
            return false;
        }

        // Apotheosis: when this host last freed an in-flight slot. The stall ceiling is measured
        // against it, so that a host which is genuinely making progress -- however slowly, however
        // many transfers it has -- is never touched.
        MonotonicTime lastRetirement() const { return m_lastRetirement; }
        void noteRetirement() { m_lastRetirement = MonotonicTime::now(); }

        const HashSet<RefPtr<ResourceLoader>>& loading() const { return m_loading; }

        Deque<RefPtr<ResourceLoader>>& pending(ResourceLoadPriority priority) { return m_pending[indexOf(priority)]; }
        unsigned inFlight() const { return m_loading.size(); }

        unsigned pendingCount() const
        {
            unsigned count = 0;
            for (auto& queue : m_pending)
                count += queue.size();
            return count;
        }

    private:
        // ResourceLoadPriority runs VeryLow=0 .. VeryHigh=4, so the cast is the identity
        // upstream's switch spells out longhand.
        static unsigned indexOf(ResourceLoadPriority priority) { return static_cast<unsigned>(priority); }

        std::array<Deque<RefPtr<ResourceLoader>>, resourceLoadPriorityCount> m_pending;
        HashSet<RefPtr<ResourceLoader>> m_loading;
        // Apotheosis 2026-09-24: starts at construction so a host that has never retired anything
        // is not immediately "overdue" -- the ceiling is a period of no progress, and a brand-new
        // host has not had a chance to make any.
        MonotonicTime m_lastRetirement { MonotonicTime::now() };
        const String m_name;
        const unsigned m_maxRequestsInFlight;
    };

    HostState& nonHTTPHost()
    {
        if (!m_nonHTTPHost)
            m_nonHTTPHost = makeUnique<HostState>(String { }, maxRequestsInFlightForNonHTTPProtocols);
        return *m_nonHTTPHost;
    }

    HostState* hostFor(const URL& url, bool createIfNotFound)
    {
        if (!url.protocolIsInHTTPFamily())
            return &nonHTTPHost();

        String hostName = url.host().toString();
        auto it = m_hosts.find(hostName);
        if (it != m_hosts.end())
            return it->value.get();
        if (!createIfNotFound)
            return nullptr;
        return m_hosts.add(hostName, makeUnique<HostState>(hostName, maxRequestsInFlightPerHost)).iterator->value.get();
    }

    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request, const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completionHandler) final
    {
        CString urlForTrace = request.url().string().utf8();
        // Apotheosis 2026-09-18 (ST-6): type and main= are the whole reason this line changed.
        //
        // CachedResource::load is the only asynchronous caller of LoaderStrategy::loadResource, and
        // upstream's WebResourceLoadScheduler -- which this class mirrors -- answers it with
        // SubresourceLoader::create for *every* CachedResource, the main one included. So this hook is
        // not "the subresource path": DocumentLoader::loadMainResource reaches it through
        // CachedResourceLoader::requestMainResource, which means a page-initiated navigation and an
        // <img> on that page arrive here looking exactly alike. Reading this trace while believing it
        // covered subresources only is how a navigation gets mistaken for a subresource and the
        // search goes to the wrong layer.
        //
        // Type is a CachedResource::Type, and the number is printed rather than a name so this
        // cannot drift out of step with the enum -- which is exactly what the hand-written list
        // that used to sit here did. That list was one slot out from CachedResource.h for
        // everything from ImageResource onward (it named ImageResource=2 / CSSStyleSheet=4 /
        // Script=5), and it was wrong for a whole investigation: measured 2026-09-24, `.css`
        // arrives as type=3 and `.js` as type=4. Read the enum at
        // WebKit\Source\WebCore\loader\cache\CachedResource.h -- in this tree it is
        // MainResource=0, ImageResource=1, JSON=2, CSSStyleSheet=3, Script=4, FontResource=5,
        // SVGFontResource=6, MediaResource=7, then EnvironmentMapResource/ModelResource under
        // ENABLE(MODEL_ELEMENT) (which is on here) ahead of RawResource. main= is
        // frame.isMainFrame(). The paired `+` trace line prints the URL's leaf, so an extension
        // is the cheap cross-check that needs no header at all.
        int resourceType = static_cast<int>(resource.type());
        int isMainFrame = frame.isMainFrame() ? 1 : 0;
        // Apotheosis 2026-09-18 (ST-5): proof of arrival, before create() is even consulted.
        const char* callUrl = urlForTrace.data() ? urlForTrace.data() : "(null)";
        if (g_loaderCallTraceBudget) {
            --g_loaderCallTraceBudget;
            portLoaderTraceBoth("loader: CALL type=%d main=%d %.90s", resourceType, isMainFrame, callUrl);
        } else
            portLoaderTrace("loader: CALL type=%d main=%d %.90s", resourceType, isMainFrame, callUrl);
        SubresourceLoader::create(frame, resource, WTF::move(request), options,
            [this, urlForTrace, completionHandler = WTF::move(completionHandler)](RefPtr<SubresourceLoader>&& loader) mutable {
                if (loader)
                    scheduleLoad(*loader);
                else {
                    // Apotheosis: create() declined -- CSP, mixed content, a bad URL. Traced
                    // per-URL because it is an anomaly; successful loads are only counted (see
                    // serve()), since tracing 724 of them per page is itself a slowdown.
                    portLoaderTraceBoth("loader: DECLINED %s", urlForTrace.data());
                }
                // Apotheosis: call this even when create() declined. CachedResourceLoader waits
                // on it, and a dropped CompletionHandler is exactly how a resource ends up
                // Pending forever.
                completionHandler(WTF::move(loader));
            });
    }

    void scheduleLoad(ResourceLoader& loader)
    {
        HostState* host = hostFor(loader.url(), true);
        host->schedule(loader, loader.request().priority());
        scheduleServe();
    }

    // Apotheosis: never start a load synchronously from inside loadResource(). We are called
    // from deep inside CachedResourceLoader, and ResourceLoader::start() can *finish* a load
    // before returning (data: URLs, immediate failures) and re-enter CachedResourceLoader
    // before it has finished registering the resource being loaded. Deferring by one run-loop
    // turn is what WebResourceLoadScheduler's request timer buys upstream. The engine thread
    // pumps this run loop (see pumpLoop in WebCoreDriver.cpp) and `this` is a
    // process-lifetime singleton, so the captured pointer cannot dangle.
    void scheduleServe()
    {
        if (m_serveScheduled)
            return;
        m_serveScheduled = true;
        RunLoop::mainSingleton().dispatch([this] {
            m_serveScheduled = false;
            serve(ResourceLoadPriority::Lowest);
        });
    }

    void serve(ResourceLoadPriority minimumPriority)
    {
        if (m_suspendCount)
            return;

        // Apotheosis: ResourceLoader::start() can complete a load synchronously and re-enter
        // through remove() -> scheduleServe() -> serve(). Letting that recurse would mutate the
        // host map underneath the pass that is walking it, so a re-entrant call only asks for
        // another pass on the next run-loop turn. This flag is also what keeps HostState
        // pointers alive across start(): hosts are only erased by the loop below.
        if (m_serving) {
            scheduleServe();
            return;
        }
        m_serving = true;

        // Apotheosis 2026-09-24: before anything is served, give a host that has been making no
        // progress at all a way out. This runs HERE, at the top of the pass, and not inside
        // serveHost() while it walks -- cancel() re-enters through remove() -> scheduleServe(), and
        // mutating a container being iterated is exactly the class of bug the m_serving flag
        // already exists to prevent.
        enforceStallCeiling();

        serveHost(nonHTTPHost(), minimumPriority);

        // Copy the key list first: serveHost() -> start() can add hosts (a redirect to another
        // origin, an @import from a CDN) and rehash the map.
        for (auto& hostName : copyToVector(m_hosts.keys())) {
            auto it = m_hosts.find(hostName);
            if (it == m_hosts.end())
                continue;
            if (it->value->hasRequests())
                serveHost(*it->value, minimumPriority);
            else
                m_hosts.remove(it);
        }

        m_serving = false;

        // One line per pass rather than per load: a pass happens at most once per run-loop turn,
        // so this stays bounded on a 724-resource page while still showing the pipeline moving.
        unsigned inFlight = nonHTTPHost().inFlight();
        unsigned pending = nonHTTPHost().pendingCount();
        for (auto& host : m_hosts.values()) {
            inFlight += host->inFlight();
            pending += host->pendingCount();
        }
        if (inFlight != m_lastTracedInFlight || pending != m_lastTracedPending) {
            m_lastTracedInFlight = inFlight;
            m_lastTracedPending = pending;
            portLoaderTraceBoth("loader: serve started=%u inflight=%u pending=%u hosts=%u", m_startedCount, inFlight, pending, m_hosts.size());
        }

        // Apotheosis 2026-09-24: a page whose host is saturated schedules nothing by itself -- that
        // is what being stalled means -- so a check that only ever ran at the end of a pass would
        // never run again. While some host is at its cap with work queued, keep a wake-up armed.
        if (anyHostSaturated())
            scheduleStallCheck();
    }

    // Apotheosis 2026-09-24: the last resort of item 0n. A host that is at its cap, has something
    // waiting, and has retired NOTHING for saturatedHostCeiling has stopped making progress by
    // every measure the port can take. One of its in-flight requests is cancelled to free a slot,
    // which is the only way the queue can advance when no transfer reaches a terminal state on its
    // own. The longest-held slot goes first, because it is the one most likely to be the stuck one.
    //
    // Deliberately a last resort: all three conditions must hold at once, and a single retirement
    // anywhere on the host resets the clock. A slow-but-moving host is never touched.
    void enforceStallCeiling()
    {
        MonotonicTime now = MonotonicTime::now();
        Vector<RefPtr<ResourceLoader>> doomed;

        auto consider = [&](HostState& host) {
            if (!host.atCapWithWorkPending())
                return;
            if (now - host.lastRetirement() < saturatedHostCeiling)
                return;
            RefPtr<ResourceLoader> oldest;
            MonotonicTime oldestAt = now;
            for (auto& loader : host.loading()) {
                auto it = m_startInfo.find(static_cast<void*>(loader.get()));
                if (it == m_startInfo.end())
                    continue;
                if (!oldest || it->value.startedAt < oldestAt) {
                    oldest = loader;
                    oldestAt = it->value.startedAt;
                }
            }
            if (oldest)
                doomed.append(oldest);
        };

        if (m_nonHTTPHost)
            consider(*m_nonHTTPHost);
        for (auto& host : m_hosts.values())
            consider(*host);

        for (auto& loader : doomed) {
            // Apotheosis: loud on purpose. A ceiling that fires must never be silent -- silence is
            // what let the GPU probe hide a dead context for a day, and what let this stall be
            // mistaken for a lost load. `age` is how long the slot had been held.
            auto it = m_startInfo.find(static_cast<void*>(loader.get()));
            double age = it == m_startInfo.end() ? -1 : (now - it->value.startedAt).seconds();
            portLoaderTraceBoth("loader: STALL-CEILING cancelling %p age=%.1fs host-held-no-progress=%us",
                static_cast<void*>(loader.get()), age, static_cast<unsigned>(saturatedHostCeiling.seconds()));
            loader->cancel();
        }
    }

    bool anyHostSaturated() const
    {
        if (m_nonHTTPHost && m_nonHTTPHost->atCapWithWorkPending())
            return true;
        for (auto& host : m_hosts.values()) {
            if (host->atCapWithWorkPending())
                return true;
        }
        return false;
    }

    void scheduleStallCheck()
    {
        if (m_stallCheckScheduled)
            return;
        m_stallCheckScheduled = true;
        // Apotheosis: the returned timer must be kept. WTF cancels a DispatchTimer when its last
        // reference goes away, so `RunLoop::mainSingleton().dispatchAfter(...)` with the result
        // discarded arms a wake-up that never fires. Capturing `this` is safe for the same reason
        // scheduleServe()'s capture is: this strategy is a process-lifetime singleton.
        //
        // Apotheosis 2026-09-24: a timer is the one part of this mechanism that cannot be verified by
        // reading it, because `RunLoop::mainSingleton()` is whatever thread first reached
        // WTF::initializeMainThread -- here the engine thread, whose run loop is pumped from inside a
        // load job (WebCoreDriver.cpp's pumpLoop, `RunLoop::run()`). Whether it is pumped at all once
        // that job has ended is the open question, and this stall is precisely the case where the job
        // HAS ended: [STAGE] after-load arrives 3.25 s in while the queue is still stuck. So the pair
        // of lines below is the measurement, not decoration. `armed` with no `fired` says the run
        // loop stopped being pumped and the ceiling needs a different wake-up source -- and, because
        // m_stallCheckScheduled stays true in that case, it also says why only one `armed` line will
        // ever appear per stall.
        portLoaderTraceBoth("loader: STALL-WATCH armed in %us", static_cast<unsigned>(saturatedHostRecheck.seconds()));
        m_stallTimer = RunLoop::mainSingleton().dispatchAfter(saturatedHostRecheck, [this] {
            m_stallCheckScheduled = false;
            portLoaderTraceBoth("loader: STALL-WATCH fired");
            // A full pass, so the freed slot is filled in the same turn the ceiling fires in.
            serve(ResourceLoadPriority::Lowest);
        });
    }

    void serveHost(HostState& host, ResourceLoadPriority minimumPriority)
    {
        auto priority = ResourceLoadPriority::Highest;
        while (true) {
            auto& pending = host.pending(priority);
            while (!pending.isEmpty()) {
                // Upstream returns rather than breaks, and that is the point: a host that
                // cannot take another connection cannot take a lower-priority one either.
                if (host.atLimit(priority))
                    return;
                RefPtr<ResourceLoader> loader = pending.takeFirst();
                if (!loader)
                    continue;
                host.addLoadInProgress(*loader);
                ++m_startedCount;
                // Apotheosis: a paired start/finish trace, because the aggregate counters cannot
                // answer the question that matters. On hh.ru the scheduler reports
                // inflight=0 pending=0 -- itself completely idle -- while WebCore still holds seven
                // resources at CachedResource::Pending, one of them the parser-blocking
                // vendors...js, and the document sits at readyState Loading with no body. No
                // DECLINED lines, no curl errors. So a request either never reached the scheduler or
                // reached it and never came back, and only per-request identity distinguishes those.
                // The loader pointer is the identity: it is unique and stable for the load's
                // lifetime, and `+` without a matching `-` is exactly the stuck request. One short
                // line each keeps this affordable -- roughly 300 lines for hh.ru's ~150 requests --
                // where tracing every successful load with its full URL would not be.
                {
                    CString u = loader->url().string().utf8();
                    const char* full = u.data() ? u.data() : "";
                    const char* slash = std::strrchr(full, '/');
                    const char* leaf = (slash && slash[1]) ? slash + 1 : (full[0] ? full : "(empty)");
                    // Apotheosis: g<n> is the session generation this request was started in.
                    // Compare it against the g<n> on the matching '-' line: if they differ, the
                    // response came back after the session that asked for it had been torn down.
                    // The start time goes in the same entry: it is what the stall ceiling uses to
                    // pick the transfer that has been holding a slot longest.
                    m_startInfo.set(static_cast<void*>(loader.get()), LoadStart { g_loaderGeneration, MonotonicTime::now() });
                    portLoaderTrace("loader: +%p g%u %.44s",
                        static_cast<void*>(loader.get()), g_loaderGeneration, leaf);
                }
                loader->start();
            }
            if (priority == minimumPriority)
                return;
            priority = static_cast<ResourceLoadPriority>(static_cast<uint8_t>(priority) - 1);
        }
    }

    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest&, ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError&, ResourceResponse&, Vector<uint8_t>&) final { }
    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }

    // Apotheosis: called when a load is cancelled or reaches its terminal state. Dropping our
    // reference is what frees the host's in-flight slot, so this is also what advances the
    // queue -- without the scheduleServe() here, a 724-image page would start six images and
    // stop forever.
    void remove(ResourceLoader* loader) final
    {
        if (!loader)
            return;
        // Apotheosis: the closing half of the +/- pair started in serveHost. Also records whether
        // the host lookup succeeded: remove() finds the host by the loader's *current* URL, so a
        // load that crossed origins after crossOriginRedirectReceived moved its registration, or one
        // whose host was already dropped from the map, would silently skip the slot release. h=0 in
        // this trace is that case, and it would show up as a host that never serves again.
        HostState* host = hostFor(loader->url(), false);
        // Apotheosis: g<started>/g<now>. Reported as a pair rather than left to offline pairing
        // because loader addresses are recycled, so a later '+' at the same address would be
        // indistinguishable from the earlier one in a text pairing. The map entry is taken here,
        // which keeps one live entry per address at a time and makes recycling harmless.
        unsigned startedIn = g_loaderGeneration;
        if (auto it = m_startInfo.find(static_cast<void*>(loader)); it != m_startInfo.end()) {
            startedIn = it->value.generation;
            m_startInfo.remove(it);
        }
        if (startedIn != g_loaderGeneration) {
            portLoaderTrace("loader: -%p g%u h=%d STALE started-in-g%u",
                static_cast<void*>(loader), g_loaderGeneration, host ? 1 : 0, startedIn);
        } else
            portLoaderTrace("loader: -%p g%u h=%d", static_cast<void*>(loader), g_loaderGeneration, host ? 1 : 0);
        if (host)
            host->remove(*loader);
        scheduleServe();
    }

    // Apotheosis: deliberately a no-op. Upstream restarts a deferred request here, but nothing
    // in this port ever defers: Page::setDefersLoading() is never called and the curl bridge in
    // ResourceHandle.cpp has no suspend for a transfer already in flight. ResourceLoader::
    // setDefersLoading() also calls straight back into this hook, so doing work here risks a
    // loop for no gain.
    void setDefersLoading(ResourceLoader&, bool) final { }

    // Apotheosis: the curl bridge follows redirects itself (CURLOPT_FOLLOWLOCATION is never
    // set, so WebCore sees every hop). When a hop crosses to another host the in-flight
    // registration has to move with it, or the old host leaks a slot forever and the new host
    // never counts the connection it is actually holding.
    void crossOriginRedirectReceived(ResourceLoader* loader, const URL& redirectURL) final
    {
        if (!loader)
            return;
        HostState* oldHost = hostFor(loader->url(), false);
        HostState* newHost = hostFor(redirectURL, true);
        if (!oldHost || !newHost || oldHost == newHost)
            return;
        newHost->addLoadInProgress(*loader);
        oldHost->remove(*loader);
    }

    void servePendingRequests(ResourceLoadPriority minimumPriority) final { serve(minimumPriority); }

    void suspendPendingRequests() final { ++m_suspendCount; }

    void resumePendingRequests() final
    {
        ASSERT(m_suspendCount);
        if (m_suspendCount)
            --m_suspendCount;
        if (!m_suspendCount)
            scheduleServe();
    }

    void startPingLoad(LocalFrame&, ResourceRequest&, const HTTPHeaderMap&, const FetchOptions&, ContentSecurityPolicyImposition, PingLoadCompletionHandler&&) final { }
    void preconnectTo(FrameLoader&, ResourceRequest&&, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&&) final { }
    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }
    bool isOnLine() const final { return true; }
    void addOnlineStateChangeListener(Function<void(bool)>&&) final { }

    // Apotheosis: a load is finished once its host bucket is gone -- the same test upstream
    // uses. Hosts are dropped by serve() as soon as they have no queued or in-flight requests.
    void isResourceLoadFinished(CachedResource& resource, CompletionHandler<void(bool)>&& callback) final
    {
        auto loader = resource.loader();
        if (!loader) {
            callback(true);
            return;
        }
        callback(!hostFor(loader->url(), false));
    }

    ResourceError cancelledError(const ResourceRequest&) const final { return { }; }
    ResourceError blockedError(const ResourceRequest&) const final { return { }; }
    ResourceError blockedByContentBlockerError(const ResourceRequest&) const final { return { }; }
    ResourceError cannotShowURLError(const ResourceRequest&) const final { return { }; }
    ResourceError interruptedForPolicyChangeError(const ResourceRequest&) const final { return { }; }
#if ENABLE(CONTENT_FILTERING)
    ResourceError blockedByContentFilterError(const ResourceRequest&) const final { return { }; }
#endif
    ResourceError cannotShowMIMETypeError(const ResourceResponse&) const final { return { }; }
    ResourceError fileDoesNotExistError(const ResourceResponse&) const final { return { }; }
    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest&) const final { return { }; }
    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest&) const final { return { }; }
    ResourceError pluginWillHandleLoadError(const ResourceResponse&) const final { return { }; }

public:
    // Apotheosis: exact counters at an arbitrary moment, for the marks the driver writes around
    // teardown. serve() computes the same numbers but only traces them when they change and only
    // at the end of a pass, so it cannot answer "how much was still in flight when the session
    // died" -- which is the number the stale-callback question turns on.
    void snapshot(unsigned& started, unsigned& inFlight, unsigned& pending, unsigned& hosts)
    {
        started = m_startedCount;
        inFlight = m_nonHTTPHost ? m_nonHTTPHost->inFlight() : 0;
        pending = m_nonHTTPHost ? m_nonHTTPHost->pendingCount() : 0;
        for (auto& host : m_hosts.values()) {
            inFlight += host->inFlight();
            pending += host->pendingCount();
        }
        hosts = m_hosts.size();
    }

private:
    // Apotheosis 2026-09-24: one entry per live load, keyed by the loader address -- the same
    // identity the +/- trace uses, and stable for the load's lifetime. The generation answers "did
    // this outlive its session"; the start time answers "which of these has been holding a slot
    // longest", which is what the stall ceiling cancels. One map rather than two so the two facts
    // cannot drift apart, and so remove() cannot free one and leak the other.
    struct LoadStart {
        unsigned generation { 0 };
        MonotonicTime startedAt;
    };

    HashMap<String, std::unique_ptr<HostState>> m_hosts;
    // Apotheosis: loader address -> how and when it started. Entries are taken in remove(), so the
    // map holds only live loads and a recycled address cannot be confused with its previous owner.
    HashMap<void*, LoadStart> m_startInfo;
    std::unique_ptr<HostState> m_nonHTTPHost;
    // Apotheosis 2026-09-24: the stall ceiling's wake-up. Held rather than dropped because a
    // RunLoop::DispatchTimer is cancelled when its last reference goes -- letting this return value
    // fall on the floor would make the check fire at most once.
    RefPtr<RunLoop::DispatchTimer> m_stallTimer;
    unsigned m_suspendCount { 0 };
    unsigned m_startedCount { 0 };
    unsigned m_lastTracedInFlight { 0 };
    unsigned m_lastTracedPending { 0 };
    bool m_serveScheduled { false };
    bool m_stallCheckScheduled { false };
    bool m_serving { false };
};

// Apotheosis: the live strategy, set in createLoaderStrategy below. Only ever read for tracing.
static PortLoaderStrategy* g_liveLoaderStrategy = nullptr;

// Apotheosis: called by the driver (WebCoreDriver.cpp) around teardownSession. Two entry points
// rather than one because the order matters: every mark must be written while the generation is
// still the old one, and the bump comes last, once the session is really gone. Both are named in
// the WebCorePort namespace and declared at the driver's call site -- there is no shared port
// header yet, and adding one would mean recompiling every port translation unit.
void portLoaderMark(const char* tag)
{
    if (!tag)
        return;
    unsigned started = 0, inFlight = 0, pending = 0, hosts = 0;
    if (g_liveLoaderStrategy)
        g_liveLoaderStrategy->snapshot(started, inFlight, pending, hosts);
    portLoaderTraceBoth("loader: MARK %s gen=%u started=%u inflight=%u pending=%u hosts=%u",
        tag, g_loaderGeneration, started, inFlight, pending, hosts);
}

void portLoaderNextGeneration()
{
    ++g_loaderGeneration;
}

// --- BlobRegistry forwarder (copied from WebKitLegacy WebBlobRegistry) -------
class PortBlobRegistry final : public BlobRegistry {
public:
    PortBlobRegistry() = default;

private:
    void registerInternalFileBlobURL(const URL& url, Ref<BlobDataFileReference>&& reference, const String&, const String& contentType) final { m_blobRegistry.registerInternalFileBlobURL(url, WTF::move(reference), contentType); }
    void registerInternalBlobURL(const URL& url, Vector<BlobPart>&& parts, const String& contentType) final { m_blobRegistry.registerInternalBlobURL(url, WTF::move(parts), contentType); }
    void registerBlobURL(const URL& url, const URL& srcURL, const PolicyContainer& policyContainer, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURL(url, srcURL, policyContainer, topOrigin); }
    void registerInternalBlobURLOptionallyFileBacked(const URL& url, const URL& srcURL, RefPtr<BlobDataFileReference>&& reference, const String& contentType) final { m_blobRegistry.registerInternalBlobURLOptionallyFileBacked(url, srcURL, WTF::move(reference), contentType, { }); }
    void registerInternalBlobURLForSlice(const URL& url, const URL& srcURL, long long start, long long end, const String& contentType) final { m_blobRegistry.registerInternalBlobURLForSlice(url, srcURL, start, end, contentType); }
    void unregisterBlobURL(const URL& url, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURL(url, topOrigin); }
    String blobType(const URL& url) final { return m_blobRegistry.blobType(url); }
    unsigned long long blobSize(const URL& url) final { return m_blobRegistry.blobSize(url); }
    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>& blobURLs, CompletionHandler<void(Vector<String>&& filePaths)>&& completionHandler) final { m_blobRegistry.writeBlobsToTemporaryFilesForIndexedDB(blobURLs, WTF::move(completionHandler)); }
    void registerBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURLHandle(url, topOrigin); }
    void unregisterBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURLHandle(url, topOrigin); }

    BlobRegistryImpl* blobRegistryImpl() final { return &m_blobRegistry; }

    BlobRegistryImpl m_blobRegistry;
};

class PortPlatformStrategies final : public PlatformStrategies {
public:
    PortPlatformStrategies() = default;

private:
    LoaderStrategy* createLoaderStrategy() final
    {
        // Apotheosis: a real loader (see PortLoaderStrategy) -- not the old empty stub.
        // Apotheosis 2026-09-18 (ST-5): WebCore asks for a loader strategy exactly once per
        // process, lazily on the first load. This line appearing proves the singleton we installed
        // is the one the engine actually uses; its absence beside a successful install would mean
        // the strategies object the engine consults belongs to someone else.
        portLoaderTraceBoth("PS: createLoaderStrategy");
        PortLoaderStrategy* strategy = new PortLoaderStrategy;
        // Apotheosis: remembered so the driver's teardown marks can read the live counters.
        // WebCore calls this exactly once per process and never destroys the result, so the
        // pointer cannot go stale; it is deliberately not an owning reference.
        g_liveLoaderStrategy = strategy;
        return strategy;
    }

    PasteboardStrategy* createPasteboardStrategy() final
    {
        // The render path never touches the pasteboard. pasteboardStrategy()
        // returns a CheckedPtr, so a null here is tolerated as long as nothing
        // dereferences it during a render (it does not).
        return nullptr;
    }

    MediaStrategy* createMediaStrategy() final
    {
        class PortMediaStrategy final : public MediaStrategy {
            bool enableWebMMediaPlayer() const final { return false; }
        };
        return new PortMediaStrategy;
    }

    BlobRegistry* createBlobRegistry() final
    {
        return new PortBlobRegistry;
    }
};

} // namespace WebCorePort

void installPortPlatformStrategies()
{
    static WTF::NeverDestroyed<WebCorePort::PortPlatformStrategies> strategies;
    const bool alreadyInstalled = WebCore::hasPlatformStrategies();
    if (!alreadyInstalled)
        WebCore::setPlatformStrategies(&strategies.get());
    // Apotheosis 2026-09-18 (ST-5): report the decision, and the environment it was made in.
    //
    // hasPlatformStrategies() here resolves to this port's own stub in webcore-internal-stubs.cpp
    // (it returns a hard false) rather than to WebCore.dll's real one, because an explicit object
    // definition beats an import-library member at link time -- webcore-exports.def lists the
    // symbol, so the intent was to import it. The consequence is benign today: false means we
    // always install, which is what we want on the first call. It is logged anyway, because the
    // day that stub stops being linked is the day this number changes.
    //
    // fcc is here because "FONTCONFIG_FILE is unset" is the one environmental reason portLoaderTrace
    // would silently write nothing, and this is the earliest point in the process where it can be
    // observed from this translation unit -- the same getenv the tracer uses.
    const char* fcc = getenv("FONTCONFIG_FILE");
    WebCorePort::portLoaderTraceBoth("PS: install has=%d didInstall=%d fcc=%.80s",
        (int)alreadyInstalled, (int)!alreadyInstalled, fcc ? fcc : "(null)");
}

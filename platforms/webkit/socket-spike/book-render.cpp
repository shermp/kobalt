#include "book-render.h"
#include "perf-trace.h"
#include "../renderer-frame.h"
#include "../retained/reader-style.h"

#include <cstring>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

using namespace spike;

namespace {
void refuse(WebKitURISchemeRequest *request)
{
    g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                                  "Publication resource unavailable");
    webkit_uri_scheme_request_finish_error(request, error);
}
} // namespace

BookRender::Resource::Resource(uint64_t token, WebKitURISchemeRequest *value)
    : id(token), request(WEBKIT_URI_SCHEME_REQUEST(g_object_ref(value)))
{
}

BookRender::Resource::~Resource()
{
    g_object_unref(request);
}

BookRender::BookRender(std::array<unsigned char *, 2> mapped, uint32_t bytes, Reply send,
                       int traceFd)
    : maps(mapped), capacity(bytes), reply(std::move(send)), measure(traceFd >= 0), trace(traceFd)
{
}

BookRender::~BookRender()
{
#ifdef KOBALT_LAZY_BOOK_PROBE
    previous.reset();
#endif
    view.reset();
    g_clear_object(&context);
}

void BookRender::reject(uint64_t id, RenderError error)
{
    std::string data;
    put(data, uint32_t(error), 4);
    reply(ErrorReply, id, data);
}

bool BookRender::receive(const Message &message)
{
    if (shutdownId)
        return false;
    if (!generation) {
        if (message.type != PoolHello || message.payload.size() != 12
            || read(message.payload.data() + 8, 4) != capacity)
            return false;
        generation = read(message.payload.data(), 8);
        if (!generation)
            return false;
        reply(Ready, message.id, message.payload);
        return true;
    }
    if (message.type == Shutdown) {
        if (!message.payload.empty())
            return false;
        shutdownId = message.id;
        if (renderId)
            cancel();
        else {
            refuseResources();
            if (!queryId)
                reply(Bye, shutdownId, {});
        }
        return true;
    }
    if (message.type == Cancel) {
        if (message.payload.size() != 8)
            return false;
        const bool current = renderId && read(message.payload.data(), 8) == renderId;
        reply(CancelAck, message.id, std::string(1, current ? '\1' : '\0'));
        if (current)
            cancel();
        return true;
    }
    if (message.type == ResourceData)
        return resourceData(message.payload);
    if (message.type == BookWordAt || message.type == BookRangeRects)
        return query(message);
    if (message.type != spike::RenderBookPage)
        return false;
    const auto received = measure ? g_get_monotonic_time() : 0;
    if (renderId) {
        reject(message.id, RenderError::Busy);
        return true;
    }
    BookPage request;
    if (!request.decode(message.payload) || request.frame.generation != generation
        || !validGeometry(request.frame.width, request.frame.height, capacity)) {
        reject(message.id, RenderError::Invalid);
        return true;
    }
    if (committed.document
        && (request.revision.document != committed.document
            || request.revision.layout < committed.layout
            || request.revision.frame <= committed.frame)) {
        reject(message.id, RenderError::Stale);
        return true;
    }
    render(message.id, std::move(request), received);
    return true;
}

bool BookRender::query(const Message &message)
{
    const bool word = message.type == BookWordAt;
    const auto type = word ? BookWordReply : BookRangeReply;
    if (queryId || (word ? message.payload.size() != 28 : message.payload.size() < 40))
        return false;
    const auto *data = message.payload.data();
    const auto revision = Revision::decode(data);
    if (!view || renderId || !committed.frame || !(revision == committed)) {
        reply(type, message.id, {});
        return true;
    }
    queryId = message.id;
    if (word) {
        view->wordAt(unsigned(read(data + 24, 2)), unsigned(read(data + 26, 2)),
                     [this, revision, type](retained::Word word) {
                         std::string payload;
                         if (!word.text.empty()) {
                             payload = BookWord{ { word.start.path, word.start.offset,
                                                   word.start.after },
                                                 { word.end.path, word.end.offset, word.end.after },
                                                 std::move(word.text),
                                                 word.x,
                                                 word.y,
                                                 word.width,
                                                 word.height }
                                               .encode();
                         }
                         finishQuery(type, revision, payload);
                     });
        return true;
    }
    const auto size = size_t(read(data + 24, 2));
    const auto second = 26 + size;
    BookAnchor first, last;
    if (second > message.payload.size() || !first.decode(message.payload.substr(26, size), 0)
        || !last.decode(message.payload.substr(second), 0) || first.offset < 0 || last.offset < 0) {
        queryId = 0;
        return false;
    }
    view->rangeRects(retained::Location{ 1, {}, std::move(first.path), first.offset, first.after },
                     retained::Location{ 1, {}, std::move(last.path), last.offset, last.after },
                     [this, revision, type](const retained::RangeGeometry &geometry) {
                         std::string payload;
                         for (const auto &edge : { geometry.start, geometry.end }) {
                             put(payload, edge.fixed, 2);
                             put(payload, edge.first, 2);
                             put(payload, edge.last, 2);
                             put(payload, edge.direction, 1);
                         }
                         for (const auto &rect : geometry.rects) {
                             put(payload, rect.x, 2);
                             put(payload, rect.y, 2);
                             put(payload, rect.width, 2);
                             put(payload, rect.height, 2);
                         }
                         finishQuery(type, revision, payload);
                     });
    return true;
}

void BookRender::finishQuery(Type type, Revision revision, const std::string &payload)
{
    const auto id = std::exchange(queryId, 0);
    if (drained) {
        if (!renderId)
            drainHelpers();
        return;
    }
    if (shutdownId) {
        if (!renderId)
            reply(Bye, shutdownId, {});
        return;
    }
    reply(type, id, !renderId && revision == committed ? payload : std::string());
}

void BookRender::enqueue(WebKitURISchemeRequest *request)
{
    const char *method = webkit_uri_scheme_request_get_http_method(request);
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    if (shutdownId || drained || !uri || !method || std::strcmp(method, "GET")
        || std::strlen(uri) > maxBookHref || resources.size() >= 64) {
        refuse(request);
        return;
    }
    resources.emplace_back(std::make_unique<Resource>(++nextResource, request));
    if (measure) {
        auto &resource = *resources.back();
        resource.queuedUs = g_get_monotonic_time();
        resource.queueDepth = unsigned(resources.size());
        resource.render = renderId;
    }
    if (resources.size() == 1)
        requestResource();
}

void BookRender::requestResource()
{
    if (resources.empty())
        return;
    auto &resource = *resources.front();
    std::string data;
    put(data, resource.bytes.size(), 4);
    if (resource.bytes.empty())
        data += webkit_uri_scheme_request_get_uri(resource.request);
    if (measure) {
        if (!resource.chunks)
            resource.requestedUs = g_get_monotonic_time();
        ++resource.chunks;
    }
    reply(ResourceRequest, resource.id, data);
}

bool BookRender::resourceData(const std::string &data)
{
    if (data.size() < 17)
        return false;
    const auto *p = data.data();
    const auto id = read(p, 8);
    if (resources.empty() || resources.front()->id != id)
        return id && id <= nextResource; // The transfer was cancelled.
    auto &resource = *resources.front();
    const auto offset = read(p + 8, 4), size = read(p + 12, 4), mediaSize = read(p + 16, 1);
    if (offset != resource.bytes.size() || size > maxResourceBytes || offset > size
        || 17 + mediaSize > data.size())
        return false;
    const auto count = data.size() - 17 - mediaSize;
    if (count > resourceChunk || count > size - offset || (size && !count)
        || (!offset && size && !mediaSize) || (offset && (mediaSize || resource.length != size)))
        return false;
    if (size) {
        if (!offset) {
            resource.media = data.substr(17, mediaSize);
            if (resource.media.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789/+-")
                != std::string::npos)
                return false;
            resource.length = size;
        }
        resource.bytes.append(data, 17 + mediaSize, count);
        if (resource.bytes.size() < size) {
            requestResource();
            return true;
        }
    } else if (count || mediaSize) {
        return false;
    }
    auto complete = std::move(resources.front());
    resources.pop_front();
    if (!size) {
        refuse(complete->request);
    } else {
        g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data(
                g_memdup2(complete->bytes.data(), size), size, g_free);
        webkit_uri_scheme_request_finish(complete->request, stream, size, complete->media.c_str());
    }
    if (measure)
        perfTrace(trace,
                  "wpe-perf-resource pid=%d request=%llu resource=%llu bytes=%llu "
                  "chunks=%u depth=%u queued_us=%lld requested_us=%lld finished_us=%lld\n",
                  int(getpid()), static_cast<unsigned long long>(complete->render),
                  static_cast<unsigned long long>(id), static_cast<unsigned long long>(size),
                  complete->chunks, complete->queueDepth, complete->queuedUs, complete->requestedUs,
                  static_cast<long long>(g_get_monotonic_time()));
    requestResource();
    return true;
}

void BookRender::refuseResources()
{
    while (!resources.empty()) {
        auto resource = std::move(resources.front());
        resources.pop_front();
        refuse(resource->request);
    }
}

void BookRender::render(uint64_t id, BookPage request, long long received)
{
    if (measure) {
        receivedUs = received;
        viewTiming = {};
        pixelStartUs = pixelEndUs = copyStartUs = copyEndUs = 0;
    }
    if (!context) {
        context = webkit_web_context_new();
        webkit_web_context_register_uri_scheme(
                context, "kobalt-epub",
                +[](WebKitURISchemeRequest *resource, gpointer data) {
                    static_cast<BookRender *>(data)->enqueue(resource);
                },
                this, nullptr);
    }
    if (!view || !view->usable()) {
#ifdef KOBALT_LAZY_BOOK_PROBE
        previous.reset();
        previousUri.clear();
        currentUri.clear();
#endif
        view = std::make_unique<retained::View>(context, retained::readerStyle, false,
                                                retained::policy, measure ? &viewTiming : nullptr);
    }

#ifdef KOBALT_LAZY_BOOK_PROBE
    // This target is a foreground-only experiment using the real reader wire.
    // Swap ownership back after previews and failed requests.
    selectedPrevious = false;
    if (!currentUri.empty() && request.uri != currentUri) {
        const bool warm = previous && previous->usable() && request.uri == previousUri;
        if (warm || !request.preview) {
            if (!warm) {
                previous.reset();
                previousUri.clear();
                previous = std::make_unique<retained::View>(
                        context, retained::readerStyle, false, retained::policy,
                        measure ? &viewTiming : nullptr, view.get());
            }
            std::swap(view, previous);
            currentUri.swap(previousUri);
            selectedPrevious = true;
            g_message("RV0b lazy request=%llu warm=%d preview=%d",
                      static_cast<unsigned long long>(id), warm, request.preview);
        }
    }
#endif
    page = std::move(request);
    renderId = id;
    cancellable = g_cancellable_new();
    retained::Page candidate{ page.uri,    page.frame.width, page.frame.height,
                              page.column, page.fontPixels,  page.href };
    candidate.fragment = page.fragment;
    if (page.restore)
        candidate.location =
                retained::Location{ 1,
                                    page.href,
                                    { page.anchor.path.begin(), page.anchor.path.end() },
                                    page.anchor.offset,
                                    page.anchor.after };
    view->show(
            std::move(candidate), cancellable,
            [this](WebKitImage *image, const char *error) {
                if (error && !g_cancellable_is_cancelled(cancellable))
                    g_warning("WebKit page: %s", error);
                snapshot(image);
            },
            page.preview);
}

void BookRender::snapshot(WebKitImage *image)
{
    if (image && view->sawExternalResource())
        g_warning("WebKit tried to load a resource outside the publication scheme");
    if (!image || view->sawExternalResource() || g_cancellable_is_cancelled(cancellable))
        return finish(false);
    const auto frame = page.frame;
    if (measure)
        pixelStartUs = g_get_monotonic_time();
    auto *pixels = fixturePixels(image, frame.width, frame.height, 0);
    if (measure)
        pixelEndUs = g_get_monotonic_time();
    if (!pixels)
        return finish(false);

    struct Copy
    {
        std::unique_ptr<FixturePixels> pixels;
        unsigned char *map;
        bool measure;
        long long startUs = 0, endUs = 0;
    };

    g_autoptr(GTask) task = g_task_new(
            nullptr, cancellable,
            +[](GObject *, GAsyncResult *result, gpointer data) {
                auto &renderer = *static_cast<BookRender *>(data);
                auto *task = G_TASK(result);
                if (renderer.measure) {
                    const auto &copy = *static_cast<Copy *>(g_task_get_task_data(task));
                    renderer.copyStartUs = copy.startUs;
                    renderer.copyEndUs = copy.endUs;
                }
                renderer.finish(g_task_propagate_boolean(task, nullptr));
            },
            this);
    g_task_set_task_data(
            task, new Copy{ std::unique_ptr<FixturePixels>(pixels), maps[frame.slot], measure },
            +[](gpointer data) { delete static_cast<Copy *>(data); });
    g_task_run_in_thread(
            task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancel) {
                auto &copy = *static_cast<Copy *>(data);
                const auto &frame = *copy.pixels;
                const auto *src =
                        static_cast<const unsigned char *>(g_bytes_get_data(frame.bytes, nullptr));
                if (copy.measure)
                    copy.startUs = g_get_monotonic_time();
                for (guint row = 0; row < frame.height; ++row) {
                    if (g_cancellable_is_cancelled(cancel))
                        return g_task_return_boolean(task, FALSE);
                    std::memcpy(copy.map + size_t(row) * frame.width * 4,
                                src + size_t(row) * frame.stride, frame.width * 4);
                }
                if (copy.measure)
                    copy.endUs = g_get_monotonic_time();
                g_task_return_boolean(task, TRUE);
            });
}

void BookRender::finish(bool success)
{
    const bool cancelled = g_cancellable_is_cancelled(cancellable);
    g_clear_object(&cancellable);
    const auto id = std::exchange(renderId, 0);
    if (measure)
        logTiming(id, success && !cancelled);
#ifdef KOBALT_LAZY_BOOK_PROBE
    const auto restoreSelection = [this] {
        if (selectedPrevious) {
            std::swap(view, previous);
            currentUri.swap(previousUri);
        }
        selectedPrevious = false;
    };
    if (cancelled || !success || drained)
        restoreSelection();
#endif
    if (drained) {
        if (!queryId)
            drainHelpers();
        return;
    }
    if (cancelled)
        reject(id, RenderError::Cancelled);
    else if (!success)
        reject(id, RenderError::Failed);
    else {
        const auto info = *(page.preview ? view->previewPageInfo() : view->pageInfo());
        if (!page.preview)
            committed = page.revision;
#ifdef KOBALT_LAZY_BOOK_PROBE
        if (page.preview)
            restoreSelection();
        else
            currentUri = page.uri;
#endif
        BookFrame frame{ page.revision,
                         info.column,
                         info.chapterPages,
                         unsigned(info.precision),
                         unsigned(info.restoration),
                         { info.anchor.path, info.anchor.offset, info.anchor.after } };
        for (const auto &link : info.links)
            frame.links.push_back({ link.uri, link.x, link.y, link.width, link.height, link.note });
        frame.pageEnd = { info.pageEnd.path, info.pageEnd.offset, info.pageEnd.after };
        reply(BookFrameReady, id, frame.encode(page.frame));
    }
    if (shutdownId && !queryId)
        reply(Bye, shutdownId, {});
}

void BookRender::logTiming(uint64_t id, bool success) const
{
    const auto request = static_cast<unsigned long long>(id);
    perfTrace(trace,
              "wpe-perf-render pid=%d request=%llu document=%llu layout=%llu frame=%llu "
              "operation=%s success=%d received_us=%lld pixel_start_us=%lld pixel_end_us=%lld "
              "copy_start_us=%lld copy_end_us=%lld finished_us=%lld\n",
              int(getpid()), request, static_cast<unsigned long long>(page.revision.document),
              static_cast<unsigned long long>(page.revision.layout),
              static_cast<unsigned long long>(page.revision.frame), viewTiming.operation, success,
              receivedUs, pixelStartUs, pixelEndUs, copyStartUs, copyEndUs,
              static_cast<long long>(g_get_monotonic_time()));
    const auto logLayout = [this, request](const char *phase,
                                           const retained::View::LayoutTiming &t) {
        if (!t.applyUs)
            return;
        if (t.loadStartUs)
            perfTrace(trace,
                      "wpe-perf-navigation pid=%d request=%llu phase=%s load_start_us=%lld "
                      "load_commit_us=%lld load_finish_us=%lld early_font_px=%u\n",
                      int(getpid()), request, phase, t.loadStartUs, t.loadCommitUs, t.loadFinishUs,
                      t.earlyFontSize);
        const auto &[viewport, style, font, images, columns, scroll, source, links, end] = t.domUs;
        perfTrace(
                trace,
                "wpe-perf-dom pid=%d request=%llu phase=%s apply_us=%lld dom_start_us=%lld "
                "dom_end_us=%lld snapshot_start_us=%lld snapshot_end_us=%lld viewport_us=%d "
                "style_us=%d fonts_us=%d images_us=%d columns_us=%d scroll_us=%d "
                "source_start_us=%d links_us=%d source_end_us=%d reused=%d visible_decode_us=%d\n",
                int(getpid()), request, phase, t.applyUs, t.domStartUs, t.domEndUs,
                t.snapshotStartUs, t.snapshotEndUs, viewport, style, font, images, columns, scroll,
                source, links, end, int(t.reused), t.visibleDecodeUs);
        const auto &[firstNodes, firstRects, firstBatches, lastNodes, lastRects, lastBatches,
                     linkNodes, linkRects, linksFound] = t.sourceWork;
        perfTrace(
                trace,
                "wpe-perf-geometry pid=%d request=%llu phase=%s first_nodes=%d first_rects=%d first_batches=%d "
                "last_nodes=%d last_rects=%d last_batches=%d link_nodes=%d link_rects=%d links=%d\n",
                int(getpid()), request, phase, firstNodes, firstRects, firstBatches, lastNodes,
                lastRects, lastBatches, linkNodes, linkRects, linksFound);
    };
    logLayout("candidate", viewTiming.candidate);
    logLayout("rollback", viewTiming.rollback);
}

void BookRender::cancel()
{
    g_cancellable_cancel(cancellable);
    if (shutdownId || drained)
        refuseResources();
}

void BookRender::disconnect(std::function<void()> done)
{
    drained = std::move(done);
    if (renderId)
        cancel();
    else {
        refuseResources();
        if (!queryId)
            drainHelpers();
    }
}

void BookRender::drainHelpers()
{
    // Release the view after its completion returns. GLib's child watchers
    // can then reap WebKit processes while this main context still runs.
    g_timeout_add(
            10,
            +[](gpointer data) -> gboolean {
                auto &self = *static_cast<BookRender *>(data);
#ifdef KOBALT_LAZY_BOOK_PROBE
                self.previous.reset();
#endif
                self.view.reset();
                g_clear_object(&self.context);
                siginfo_t info{};
                if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) < 0 && errno == ECHILD) {
                    self.drained();
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            },
            this);
}

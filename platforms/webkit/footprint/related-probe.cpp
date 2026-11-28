// Standalone RV0 workload. Each configuration uses the production retained layout
// and snapshot path, one context and two output slots.
#include "related-memory.h"
#include "../renderer-frame.h"
#include "../retained/reader-style.h"
#include "../retained/view.h"

#include <glib-unix.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using related_probe::Checkpoint;
constexpr unsigned width = 1440, height = 1920, frameBytes = width * height * 4;
constexpr std::string_view origin = "kobalt-epub://book-rv0/";
constexpr int warmTurns = 20, idleMs = 15000;

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "RV0: %s\n", message);
        std::exit(1);
    }
}

struct Resource
{
    const char *mime;
    std::shared_ptr<const std::string> bytes;
};

std::shared_ptr<const std::string> readFixture(const std::string &directory, const char *name)
{
    std::ifstream file(directory + '/' + name, std::ios::binary);
    std::string bytes{ std::istreambuf_iterator<char>(file), {} };
    require(!bytes.empty() && bytes.size() <= 16 * 1024 * 1024, name);
    return std::make_shared<const std::string>(std::move(bytes));
}

struct Publication
{
    std::map<std::string, Resource, std::less<>> resources;
    unsigned refused = 0;
    std::function<bool(WebKitURISchemeRequest *, const Resource &)> onResource;

    Publication(std::string_view workload, const std::string &directory)
    {
        const bool fonts = workload == "resources";
        const bool images = workload == "images";
        if (fonts) {
            resources.emplace("styles.css",
                              Resource{ "text/css",
                                        std::make_shared<const std::string>(
                                                "@font-face { font-family: ReaderFixture; "
                                                "src: url(font.ttf); } "
                                                "body { font-family: ReaderFixture, serif; }") });
            resources.emplace("font.ttf",
                              Resource{ "font/ttf", readFixture(directory, "font.ttf") });
        }
        constexpr const char *pictures[] = { "cover.png", "ornament.png", "reporter.png" };
        std::array<std::shared_ptr<const std::string>, std::size(pictures)> pictureData;
        if (fonts || images) {
            for (unsigned i = 0; i < pictureData.size(); ++i)
                pictureData[i] = readFixture(directory, pictures[i]);
        }

        // Three spine items and one alternate document for forced reloads.
        for (unsigned spine = 0; spine < 4; ++spine) {
            const auto title = "Chapter " + std::to_string(spine + 1);
            const int paragraphs = workload == "long" ? 512 : 24;
            std::string document =
                    "<?xml version='1.0'?><html xmlns='http://www.w3.org/1999/xhtml'>"
                    "<head><title>"
                    + title + "</title>";
            if (fonts)
                document += "<link rel='stylesheet' href='styles.css'/>";
            document += "</head><body>";
            for (unsigned column = 0; column < (images ? 8u : 3u); ++column) {
                if (column)
                    document += "<section style='break-before:column'>";
                else
                    document += "<section>";
                document += "<h1>" + title + " / " + std::to_string(column + 1) + "</h1>";
                if (fonts || images) {
                    const auto variant = (spine + column * unsigned(images)) % pictureData.size();
                    // Give full-page illustrations separate URLs to exercise decoding and memory
                    // for distinct EPUB resources, while sharing the small input byte buffers.
                    const auto image = images ? "image-" + std::to_string(spine) + "-"
                                    + std::to_string(column) + ".png"
                                              : pictures[variant];
                    resources.try_emplace(image, Resource{ "image/png", pictureData[variant] });
                    document += "<img src='" + image + "' alt='Fixture illustration'/>";
                }
                if (!images)
                    for (int row = 0; row < paragraphs; ++row)
                        document +=
                                "<p>" + title + ". Reading on the Forma with retained columns.</p>";
                document += "</section>";
            }
            document += "</body></html>";
            resources.emplace(href(spine),
                              Resource{ "application/xhtml+xml",
                                        std::make_shared<const std::string>(std::move(document)) });
        }
    }

    static std::string href(unsigned spine) { return "chapter" + std::to_string(spine) + ".xhtml"; }

    static void serve(WebKitURISchemeRequest *request, gpointer data)
    {
        auto &book = *static_cast<Publication *>(data);
        const auto *method = webkit_uri_scheme_request_get_http_method(request);
        const auto *rawUri = webkit_uri_scheme_request_get_uri(request);
        const auto uri = std::string_view(rawUri ? rawUri : "");
        const auto resource = uri.starts_with(origin)
                ? book.resources.find(uri.substr(origin.size()))
                : book.resources.end();
        if (!method || std::strcmp(method, "GET") || resource == book.resources.end()) {
            ++book.refused;
            g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                                          "Publication resource unavailable");
            webkit_uri_scheme_request_finish_error(request, error);
            return;
        }
        const auto &file = resource->second;
        if (!book.onResource || !book.onResource(request, file))
            deliver(request, file);
    }

    static void deliver(WebKitURISchemeRequest *request, const Resource &file)
    {
        g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data(
                file.bytes->data(), file.bytes->size(), nullptr);
        webkit_uri_scheme_request_finish(request, stream, file.bytes->size(), file.mime);
    }
};

struct Frames
{
    std::array<unsigned char *, 2> slots{};

    Frames()
    {
        for (unsigned i = 0; i < slots.size(); ++i) {
            const auto name = "/kobalt-rv0-" + std::to_string(getpid()) + "-" + std::to_string(i);
            const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
            require(fd >= 0 && shm_unlink(name.c_str()) == 0 && ftruncate(fd, frameBytes) == 0,
                    "Create shared frame");
            const auto map = mmap(nullptr, frameBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            require(map != MAP_FAILED, "Map shared frame");
            slots[i] = static_cast<unsigned char *>(map);
        }
    }

    ~Frames()
    {
        for (auto *slot : slots)
            munmap(slot, frameBytes);
    }

    const unsigned char *write(WebKitImage *image)
    {
        const std::unique_ptr<FixturePixels> pixels(fixturePixels(image, width, height, 0));
        require(bool(pixels), "Read snapshot pixels");
        const auto *source =
                static_cast<const unsigned char *>(g_bytes_get_data(pixels->bytes, nullptr));
        auto *target = slots[next];
        for (unsigned row = 0; row < height; ++row)
            std::memcpy(target + size_t(row) * width * 4, source + size_t(row) * pixels->stride,
                        width * 4);
        next = 1 - next;
        return target;
    }

private:
    unsigned next = 0;
};

void idle(unsigned milliseconds)
{
    bool done = false;
    g_timeout_add(
            milliseconds,
            +[](gpointer data) -> gboolean {
                *static_cast<bool *>(data) = true;
                return G_SOURCE_REMOVE;
            },
            &done);
    while (!done)
        g_main_context_iteration(nullptr, TRUE);
}

struct Run
{
    int channel;
    std::string_view workload;
    Publication publication;
    Frames frames;
    std::array<std::unique_ptr<retained::View>, 3> views;
    std::array<unsigned, 3> terminations{};
    std::vector<double> quiet, competing;
    std::map<std::pair<unsigned, unsigned>, std::string> fingerprints;
    GCancellable *cancel = nullptr;

    Run(int socket, std::string_view input, const char *directory)
        : channel(socket), workload(input), publication(input, directory)
    {
    }

    void mark(Checkpoint point) const { related_probe::checkpoint(channel, point); }

    retained::Page page(unsigned spine, unsigned column) const
    {
        const auto href = Publication::href(spine);
        return { std::string(origin) + href, width, height, column, 24, href };
    }

    void checkPage(unsigned view, unsigned spine, unsigned column)
    {
        require(!views[view]->sawExternalResource() && !publication.refused,
                "Publication resource policy rejected a request");
        const auto info = views[view]->pageInfo();
        require(info && info->anchor.href == Publication::href(spine) && info->column == column
                        && info->chapterPages >= 3,
                "Unexpected retained page details");
    }

    double render(unsigned view, unsigned spine, unsigned column, const char *phase,
                  const gint64 *loadFinished = nullptr, gint64 *delivered = nullptr)
    {
        const auto start = g_get_monotonic_time();
        gint64 end = 0;
        const unsigned char *pixels = nullptr;
        views[view]->show(page(spine, column), cancel, [&](WebKitImage *image, const char *error) {
            require(image && !error, error ? error : "Snapshot failed");
            pixels = frames.write(image);
            end = g_get_monotonic_time();
        });
        while (!end)
            g_main_context_iteration(nullptr, TRUE);
        if (delivered)
            *delivered = end;
        checkPage(view, spine, column);
        // Hash each page on its first use and during the alternating navigation
        // check. Keep verification work out of subsequent timed foreground turns.
        const auto key = std::pair{ spine, column };
        if (!fingerprints.contains(key) || std::string_view(phase) == "navigation"
            || std::string_view(phase) == "lazy-warm") {
            g_autofree gchar *sum =
                    g_compute_checksum_for_data(G_CHECKSUM_SHA256, pixels, frameBytes);
            const auto [found, newPage] = fingerprints.try_emplace(key, sum);
            require(newPage || found->second == sum, "Page fingerprint changed");
            if (newPage) {
                for (const auto &[other, fingerprint] : fingerprints) {
                    if (other != key)
                        require(fingerprint != sum,
                                "Distinct spine pages returned identical pixels");
                }
                std::printf("page spine=%u column=%u sha256=%s\n", spine, column, sum);
            }
        }
        const auto ms = (end - start) / 1000.0;
        std::printf("frame phase=%s view=%u spine=%u column=%u start_us=%lld end_us=%lld "
                    "ms=%.3f overlap_ms=%.3f\n",
                    phase, view, spine, column, static_cast<long long>(start),
                    static_cast<long long>(end), ms,
                    loadFinished ? (*loadFinished ? std::max<gint64>(0, *loadFinished - start)
                                                  : end - start)
                                    / 1000.0
                                 : 0.0);
        return ms;
    }

    void populate(unsigned view, unsigned spine, unsigned &foreground)
    {
        // There is always a completed foreground snapshot before this experiment.
        // Only load and layout the neighbour during the contention window.
        mark(Checkpoint::competingBegin);
        const auto start = g_get_monotonic_time();
        gint64 finished = 0;
        views[view]->prepare(page(spine, 0), cancel, [&](const char *error) {
            require(!error, error ? error : "Adjacent layout failed");
            finished = g_get_monotonic_time();
        });
        while (!finished) {
            foreground = 1 - foreground;
            competing.push_back(render(0, 0, foreground, "competing", &finished));
        }
        mark(Checkpoint::competingEnd);
        checkPage(view, spine, 0);
        std::printf("load view=%u spine=%u start_us=%lld end_us=%lld ms=%.3f\n", view, spine,
                    static_cast<long long>(start), static_cast<long long>(finished),
                    (finished - start) / 1000.0);
    }

    void summarize(const char *phase, std::vector<double> times)
    {
        if (times.empty())
            return;
        std::sort(times.begin(), times.end());
        const auto count = times.size();
        std::printf("latency phase=%s samples=%zu median_ms=%.3f p95_ms=%.3f\n", phase, count,
                    (times[(count - 1) / 2] + times[count / 2]) / 2,
                    times[(count * 95 + 99) / 100 - 1]);
    }

    void preempt(const std::function<void(unsigned)> &create, std::string_view action,
                 unsigned trials)
    {
        constexpr std::string_view phases[] = { "quiet", "before-resource", "resource", "parsing",
                                                "style", "javascript",      "snapshot" };
        unsigned foreground = 1;
        for (unsigned trial = 0; trial < trials; ++trial) {
            for (const auto phase : phases) {
                const bool adjacent = phase != "quiet";
                if (adjacent)
                    create(1);
                g_autoptr(GCancellable) neighbourCancel = g_cancellable_new();
                gint64 issued = 0, received = 0, triggered = 0, completed = 0, cancelled = 0;
                bool held = false, succeeded = false, discarded = false;
                std::string result;
                auto stage = [&](std::string_view name) {
                    const auto now = g_get_monotonic_time();
                    std::printf("preempt-stage trial=%u target=%.*s name=%.*s t_us=%lld\n", trial,
                                int(phase.size()), phase.data(), int(name.size()), name.data(),
                                static_cast<long long>(now));
                    if (phase == name && !triggered) {
                        triggered = now;
                        related_probe::requestForeground(channel);
                    }
                };

                struct Receive
                {
                    gint64 &issued, &received;
                } ready{ issued, received };

                g_unix_fd_add_full(
                        G_PRIORITY_HIGH, channel, G_IO_IN,
                        +[](gint fd, GIOCondition, gpointer opaque) -> gboolean {
                            auto &request = *static_cast<Receive *>(opaque);
                            request.issued = related_probe::receiveForeground(fd);
                            request.received = g_get_monotonic_time();
                            return G_SOURCE_REMOVE;
                        },
                        &ready, nullptr);
                mark(Checkpoint::competingBegin);

                if (adjacent) {
                    views[1]->observeProbeStages(stage);
                    std::function<void(std::string_view)> loadEvent = stage;
                    auto *background = views[1]->webView();
                    g_signal_connect(
                            background, "load-changed",
                            G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent event, gpointer data) {
                                auto &signal =
                                        *static_cast<std::function<void(std::string_view)> *>(data);
                                if (event == WEBKIT_LOAD_COMMITTED)
                                    signal("parsing");
                                if (event == WEBKIT_LOAD_FINISHED)
                                    signal("load-finished");
                            }),
                            &loadEvent);
                    publication.onResource = [&](WebKitURISchemeRequest *request,
                                                 const Resource &file) {
                        if (std::string_view(webkit_uri_scheme_request_get_uri(request))
                            != page(1, 0).uri)
                            return false;
                        stage("resource-request");
                        if (phase == "resource") {
                            struct Transfer
                            {
                                WebKitURISchemeRequest *request;
                                const Resource &file;
                                bool &held;
                            };
                            held = true;
                            g_timeout_add(
                                    120,
                                    +[](gpointer data) -> gboolean {
                                        std::unique_ptr<Transfer> pending(
                                                static_cast<Transfer *>(data));
                                        Publication::deliver(pending->request, pending->file);
                                        g_object_unref(pending->request);
                                        pending->held = false;
                                        return G_SOURCE_REMOVE;
                                    },
                                    new Transfer{ WEBKIT_URI_SCHEME_REQUEST(g_object_ref(request)),
                                                  file, held });
                            stage("resource");
                            return true;
                        }
                        // The payload hand-off brackets the start of parsing with load-committed.
                        Publication::deliver(request, file);
                        stage("response-released");
                        return true;
                    };

                    auto finish = [&](const char *error) {
                        completed = g_get_monotonic_time();
                        succeeded = !error;
                        if (error)
                            result = error;
                    };
                    if (phase == "snapshot") {
                        views[1]->show(page(1, 0), neighbourCancel,
                                       [&](WebKitImage *, const char *error) { finish(error); });
                    } else {
                        views[1]->prepare(page(1, 0), neighbourCancel, finish);
                    }
                    stage("before-resource");

                    while (!received && (!completed || triggered))
                        g_main_context_iteration(nullptr, TRUE);
                    require(received, "Target preemption stage was not reached");
                    if (action != "none" && !completed) {
                        cancelled = g_get_monotonic_time();
                        g_cancellable_cancel(neighbourCancel);
                        if (action == "stop")
                            webkit_web_view_stop_loading(background);
                    }
                    foreground = 1 - foreground;
                    const auto started = g_get_monotonic_time();
                    gint64 delivered = 0;
                    render(0, 0, foreground, "preemption", nullptr, &delivered);
                    if (phase == "before-resource" && action == "stop" && !completed) {
                        idle(500);
                        if (!completed) {
                            discarded = true;
                            views[1]->abandonProbeLoad();
                        }
                    }
                    while (!completed || held)
                        g_main_context_iteration(nullptr, TRUE);
                    g_signal_handlers_disconnect_by_data(background, &loadEvent);
                    publication.onResource = {};
                    views[1].reset();
                    std::printf("preempt trial=%u target=%.*s triggered_us=%lld issue_us=%lld "
                                "received_us=%lld cancel_us=%lld observed_us=%lld start_us=%lld "
                                "frame_us=%lld success=%d discarded=%d result=%s\n",
                                trial, int(phase.size()), phase.data(),
                                static_cast<long long>(triggered), static_cast<long long>(issued),
                                static_cast<long long>(received), static_cast<long long>(cancelled),
                                static_cast<long long>(discarded ? 0 : completed),
                                static_cast<long long>(started), static_cast<long long>(delivered),
                                succeeded, discarded, result.c_str());
                } else {
                    stage("quiet");
                    while (!received)
                        g_main_context_iteration(nullptr, TRUE);
                    foreground = 1 - foreground;
                    const auto started = g_get_monotonic_time();
                    gint64 delivered = 0;
                    render(0, 0, foreground, "preemption", nullptr, &delivered);
                    std::printf("preempt trial=%u target=quiet triggered_us=%lld issue_us=%lld "
                                "received_us=%lld cancel_us=0 observed_us=0 start_us=%lld "
                                "frame_us=%lld success=1 result=\n",
                                trial, static_cast<long long>(triggered),
                                static_cast<long long>(issued), static_cast<long long>(received),
                                static_cast<long long>(started), static_cast<long long>(delivered));
                }
                mark(Checkpoint::competingEnd);
            }
        }
    }

    void lazy(const std::function<void(unsigned, int)> &create, bool retain)
    {
        // Only a successfully delivered navigation changes the selected view.
        std::array<int, 2> spines{ 0, -1 };
        unsigned active = 0, activeColumn = 1;
        auto turn = [&](unsigned spine, unsigned column) {
            const auto requested = g_get_monotonic_time();
            unsigned slot = active;
            const bool same = spines[active] == int(spine);
            bool warm = !same && retain && spines[1 - active] == int(spine);
            if (!same && retain) {
                slot = 1 - active;
                if (!warm) {
                    views[slot].reset();
                    create(slot, active);
                }
            }
            gint64 delivered = 0;
            render(slot, spine, column,
                   same           ? "lazy-turn"
                           : warm ? "lazy-warm"
                                  : "lazy-cold",
                   nullptr, &delivered);
            std::printf("lazy-request spine=%u crossing=%d warm=%d ms=%.3f\n", spine, !same, warm,
                        (delivered - requested) / 1000.0);
            spines[slot] = spine;
            active = slot;
            activeColumn = column;
        };
        auto preview = [&](unsigned spine, unsigned column) {
            const auto slot = retain && spines[1 - active] == int(spine) ? 1 - active : active;
            const auto original = views[slot]->pageInfo();
            gint64 delivered = 0;
            const auto start = g_get_monotonic_time();
            views[slot]->show(
                    page(spine, column), cancel,
                    [&](WebKitImage *image, const char *error) {
                        require(image && !error, error ? error : "Lazy preview failed");
                        const auto info = views[slot]->previewPageInfo();
                        require(info && info->column == column
                                        && info->anchor.href == Publication::href(spine),
                                "Preview page belongs to another request");
                        const auto *pixels = frames.write(image);
                        delivered = g_get_monotonic_time();
                        g_autofree gchar *sum =
                                g_compute_checksum_for_data(G_CHECKSUM_SHA256, pixels, frameBytes);
                        require(fingerprints.at({ spine, column }) == sum,
                                "Lazy preview pixels differ from confirmed navigation");
                    },
                    true);
            while (!delivered)
                g_main_context_iteration(nullptr, TRUE);
            const auto after = views[slot]->pageInfo();
            require(after && original && after->column == original->column
                            && after->anchor == original->anchor,
                    "Preview changed a retained reading position");
            checkPage(active, unsigned(spines[active]), activeColumn);
            std::printf("lazy-preview spine=%u slot=%u retained=%d ms=%.3f\n", spine, slot,
                        slot != active, (delivered - start) / 1000.0);
        };

        // Seed both page fingerprints via real requests. Revisit both chapters
        // and preview from both directions while keeping confirmed positions.
        turn(1, 0);
        mark(Checkpoint::secondLoaded);
        turn(0, 0);
        preview(1, 0);
        preview(1, 0);
        turn(1, 0);
        preview(0, 0);
        for (unsigned spine : { 2, 1, 2, 3, 2, 3, 0, 3, 0 })
            turn(spine, 0);
        mark(Checkpoint::navigated);
        idle(15000);
        mark(Checkpoint::idle);

        // A failed or cancelled crossing must leave the active view intact.
        // Run these after the timing sequence to avoid changing the warm set.
        if (retain) {
            const auto spare = 1 - active;
            for (const bool cancelRequest : { false, true }) {
                views[spare].reset();
                create(spare, active);
                g_autoptr(GCancellable) operation = g_cancellable_new();
                if (cancelRequest)
                    views[spare]->observeProbeStages([&](std::string_view stage) {
                        if (stage == "snapshot")
                            g_cancellable_cancel(operation);
                    });
                bool done = false;
                views[spare]->show(page(2, cancelRequest ? 0 : 65535), operation,
                                   [&](WebKitImage *image, const char *error) {
                                       require(!image && error,
                                               "Failed crossing delivered a candidate");
                                       done = true;
                                       std::printf("lazy-failure test=%s message=%s\n",
                                                   cancelRequest ? "cancel" : "column", error);
                                   });
                while (!done)
                    g_main_context_iteration(nullptr, TRUE);
                checkPage(active, unsigned(spines[active]), activeColumn);
                render(active, unsigned(spines[active]), activeColumn, "navigation");
            }
            views[spare].reset();
        }
        mark(Checkpoint::released);
        idle(15000);
        mark(Checkpoint::settled);
    }

    void experiment(unsigned count, bool related, bool documentViewer, bool verifyFailure,
                    std::string_view action, unsigned trials, bool runLazy)
    {
        g_autoptr(WebKitWebContext) context = webkit_web_context_new();
        g_autoptr(GCancellable) operation = g_cancellable_new();
        cancel = operation;
        if (documentViewer)
            webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
        webkit_web_context_register_uri_scheme(context, "kobalt-epub", Publication::serve,
                                               &publication, nullptr);

        auto create = [&](unsigned index, int peer = -1) {
            if (peer < 0 && index)
                peer = 0;
            const auto *opener = related && peer >= 0 ? views.at(peer).get() : nullptr;
            views[index] = std::make_unique<retained::View>(context, retained::readerStyle,
                                                            workload == "resources",
                                                            retained::policy, nullptr, opener);
            auto *native = views[index]->webView();
            auto *session = webkit_web_view_get_network_session(native);
            const bool shared =
                    opener && session == webkit_web_view_get_network_session(opener->webView());
            require(webkit_web_view_get_context(native) == context && shared == bool(opener)
                            && webkit_network_session_is_ephemeral(session),
                    "Unexpected WebKit context or network session");
            g_signal_connect(
                    native, "web-process-terminated",
                    G_CALLBACK(+[](WebKitWebView *view, WebKitWebProcessTerminationReason reason,
                                   gpointer data) {
                        ++*static_cast<unsigned *>(data);
                        std::printf("termination page_id=%llu reason=%d\n",
                                    (unsigned long long)webkit_web_view_get_page_id(view), reason);
                        require(reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API,
                                "Unexpected WebProcess termination");
                    }),
                    &terminations[index]);
            std::printf("view index=%u page_id=%llu shared_network=%d\n", index,
                        (unsigned long long)webkit_web_view_get_page_id(native), shared);
        };

        create(0);
        mark(Checkpoint::startup);
        render(0, 0, 0, "open");
        mark(Checkpoint::firstLoaded);
        render(0, 0, 1, "warmup");
        if (trials) {
            preempt([&](unsigned index) { create(index); }, action, trials);
        } else if (runLazy) {
            lazy(create, related);
        } else {
            unsigned foreground = 1;

            mark(Checkpoint::quietBegin);
            for (int i = 0; i < warmTurns; ++i) {
                foreground = 1 - foreground;
                quiet.push_back(render(0, 0, foreground, "quiet"));
            }
            mark(Checkpoint::quietEnd);

            for (unsigned index = 1; index < count; ++index) {
                create(index);
                populate(index, index, foreground);
                mark(index == 1 ? Checkpoint::secondLoaded : Checkpoint::thirdLoaded);
            }
            for (unsigned index = 0; index < count; ++index)
                render(index, index, 0, "capture");
            mark(Checkpoint::captured);
            for (unsigned turn = 0; turn <= 6; ++turn) {
                for (unsigned step = 0; step < count; ++step) {
                    const unsigned index = turn % 2 ? count - step - 1 : step;
                    render(index, index, turn % 2, "navigation");
                }
            }
            foreground = 0;
            mark(Checkpoint::navigated);
            if (count == 1) {
                for (const unsigned spine : { 1, 2, 1, 0, 1, 0 })
                    render(0, spine, 0, "cold-crossing");
            }
            // Restore the same spine set after each forced reload so the final idle
            // sample can expose memory retained by caches or previous documents.
            for (unsigned index = 1; index < count; ++index) {
                populate(index, 3, foreground);
                populate(index, index, foreground);
                render(index, index, 0, "reload-capture");
            }
            mark(Checkpoint::reloaded);
            summarize("quiet", quiet);
            summarize("competing", competing);

            if (verifyFailure) {
                webkit_web_view_terminate_web_process(views[0]->webView());
                idle(2000);
                bool expected = true;
                for (unsigned index = 0; index < count; ++index) {
                    std::printf("failure-scope view=%u terminations=%u\n", index,
                                terminations[index]);
                    expected &= terminations[index] == unsigned(index == 0 || related);
                }
                // Report the observed scope even if related views moved to another process.
                std::printf("failure-scope matches_expected=%d\n", expected);
            }
            idle(idleMs);
            mark(Checkpoint::idle);
            for (unsigned index = count; index > 1; --index)
                views[index - 1].reset();
            mark(Checkpoint::released);
            idle(idleMs);
            mark(Checkpoint::settled);
        }
        for (auto &view : views)
            view.reset();
        g_clear_object(&context);
        // Give GLib's child watchers a chance to reap the helpers.
        for (;;) {
            siginfo_t child{};
            if (waitid(P_ALL, 0, &child, WEXITED | WNOHANG | WNOWAIT) < 0 && errno == ECHILD)
                break;
            idle(10);
        }
    }
};
} // namespace

int main(int argc, char **argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "";
    const std::string_view workload = argc > 2 ? argv[2] : "";
    const unsigned count = mode == "one" ? 1 : mode == "three-related" ? 3 : 2;
    const bool related = mode == "two-related" || mode == "three-related";
    bool documentViewer = false, verifyFailure = false, runLazy = false;
    std::string_view action;
    unsigned trials = 0;
    for (int i = 4; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--document-viewer")
            documentViewer = true;
        else if (option == "--verify-failure")
            verifyFailure = true;
        else if (option == "--lazy")
            runLazy = true;
        else if (option == "--preempt" && i + 2 < argc && !trials) {
            action = argv[++i];
            char *end = nullptr;
            const auto count = std::strtol(argv[++i], &end, 10);
            if (*end || count < 1 || count > 10
                || (action != "none" && action != "cancel" && action != "stop"))
                return 2;
            trials = count;
        } else {
            return 2;
        }
    }
    if (argc < 4 || (!related && mode != "one" && mode != "two-independent")
        || (trials && (count != 2 || documentViewer || verifyFailure || runLazy))
        || (runLazy
            && ((mode != "one" && mode != "two-related") || documentViewer || verifyFailure))
        || (workload != "short" && workload != "long" && workload != "resources"
            && workload != "images")) {
        std::fprintf(
                stderr,
                "usage: %s one|two-independent|two-related|three-related "
                "short|long|resources|images FIXTURE_DIR "
                "[--document-viewer] [--verify-failure] [--preempt none|cancel|stop TRIALS] [--lazy]\n",
                argv[0]);
        return 2;
    }

    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("run mode=%s workload=%s cache=%s fault=%d\n", argv[1], argv[2],
                documentViewer ? "document-viewer" : "default", verifyFailure);
    if (trials)
        std::printf("preemption action=%.*s trials=%u\n", int(action.size()), action.data(),
                    trials);
    try {
        return related_probe::monitor([&](int socket) {
            Run run(socket, workload, argv[3]);
            run.experiment(count, related, documentViewer, verifyFailure, action, trials, runLazy);
        });
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

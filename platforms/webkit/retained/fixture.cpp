#include "view.h"

#include <array>
#include <cstring>
#include <map>
#include <memory>

namespace {
constexpr char origin[] = "kobalt-epub://book-retained/";
constexpr char secondOrigin[] = "kobalt-epub://book-figure/";
constexpr char style[] = R"CSS(
    @font-face {
        font-family: ReaderFixture;
        src: url(font.ttf) format("truetype");
    }
    html { margin: 0 !important; overflow: hidden !important; background: white !important; }
    body {
        box-sizing: border-box !important;
        margin: 0 !important;
        height: 100vh !important;
        width: 100vw !important;
        column-width: 100vw !important;
        column-gap: 0 !important;
        column-fill: auto !important;
        font: var(--reader-font-size, 18px) ReaderFixture !important;
    }
    .fixture-page { height: 100vh; break-inside: avoid; overflow: hidden; color: black; }
    .fixture-page + .fixture-page { break-before: column; }
    .fixture-page p { margin: 0; }
    .fixture-marker { position: absolute; left: 0; top: 80px; width: 15px; height: 15px; }
    .red .swatch { background: red; }
    .blue .swatch { background: blue; }
    .swatch { width: 32px; height: 32px; }
)CSS";
constexpr char hebrewStyle[] = R"CSS(
    @font-face {
        font-family: ReaderFixture;
        src: url(hebrew.ttf) format("truetype");
        unicode-range: U+0590-05FF;
    }
)CSS";
constexpr char fixture[] = R"HTML(<!doctype html><html><head>
<script>document.documentElement.setAttribute('data-book-script', 'ran')</script>
<style>@import url(https://unexpected.invalid/blocked.css);</style>
</head><body>
<section class="fixture-page red"><div class="swatch"></div><p>WPE type</p><picture><source media="(min-width: 200px)" srcset="wide-marker.png"><img class="fixture-marker" src="marker.png" alt=""></picture></section>
<section class="fixture-page blue"><div class="swatch"></div><p>WPE type</p></section>
</body></html>)HTML";
constexpr char rtlFixture[] = R"HTML(<!doctype html><html dir="rtl"><head></head>
<body><p>שלום A  <span>é עולם</span></p><p>עוד טקסט לקריאה</p></body></html>)HTML";
constexpr char policy[] =
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
        "style-src 'unsafe-inline' kobalt-epub:; img-src kobalt-epub:; "
        "font-src kobalt-epub:; base-uri 'none'; form-action 'none'\"/>";

struct Resource
{
    std::string bytes;
    const char *type;
    unsigned requests = 0;
    bool delay = false;
};

struct Pending
{
    WebKitURISchemeRequest *request;
    Resource *resource;
    ~Pending() { g_object_unref(request); }
};

void deliver(WebKitURISchemeRequest *request, const Resource &resource)
{
    g_autoptr(GBytes) bytes = g_bytes_new(resource.bytes.data(), resource.bytes.size());
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream, resource.bytes.size(), resource.type);
}

void addPolicy(std::string &html)
{
    const auto head = html.find("<head>");
    if (head != std::string::npos)
        html.insert(head + 6, policy);
}

class Fixture
{
public:
    explicit Fixture(const char *directory);
    int run();

private:
    bool add(const char *name, const char *type, bool delayed = false, const char *base = origin);
    void serve(WebKitURISchemeRequest *);
    void submit();
    void previewCheck(unsigned);
    void result(WebKitImage *, const char *);
    bool pixels(WebKitImage *, bool blue, bool controlled, std::string &hash);
    void fail(const char *);

    const char *directory;
    std::map<std::string, Resource> resources;
    GMainLoop *loop = nullptr;
    std::unique_ptr<retained::View> view;
    retained::View::Timing timing;
    unsigned step = 0, denied = 0;
    bool success = false, secondBook = false, hebrewFont = false;
    std::string firstHash, reflowHash, chapterHash, turnedHash;
    retained::Location chapterAnchor, previewWord;
    std::string previewFirst, previewNext;
    std::optional<retained::PageInfo> beforeFailure;
};

Fixture::Fixture(const char *directory) : directory(directory) {}

bool Fixture::add(const char *name, const char *type, bool delayed, const char *base)
{
    g_autofree gchar *path = g_build_filename(directory, name, nullptr);
    g_autofree gchar *data = nullptr;
    gsize size = 0;
    if (!g_file_get_contents(path, &data, &size, nullptr) || !size || size > 2 * 1024 * 1024)
        return false;
    Resource entry{std::string(data, size), type, 0, delayed};
    if (g_str_equal(type, "application/xhtml+xml"))
        addPolicy(entry.bytes);
    const auto *leaf = std::strrchr(name, '/');
    resources.emplace(std::string(base) + (leaf ? leaf + 1 : name), std::move(entry));
    return true;
}

void Fixture::serve(WebKitURISchemeRequest *request)
{
    auto found = resources.find(webkit_uri_scheme_request_get_uri(request));
    if (found == resources.end() || found->second.requests >= 16) {
        ++denied;
        g_autoptr(GError) error =
                g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Fixture URI not admitted");
        webkit_uri_scheme_request_finish_error(request, error);
        return;
    }
    auto &entry = found->second;
    ++entry.requests;
    if (!entry.delay || entry.requests != 1)
        return deliver(request, entry);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 40, +[](gpointer data) -> gboolean {
        const auto &pending = *static_cast<Pending *>(data);
        deliver(pending.request, *pending.resource);
        return G_SOURCE_REMOVE;
    }, new Pending{WEBKIT_URI_SCHEME_REQUEST(g_object_ref(request)), &entry},
        +[](gpointer data) { delete static_cast<Pending *>(data); });
}

int Fixture::run()
{
    std::string test = fixture;
    addPolicy(test);
    resources.emplace(std::string(origin) + "fixture.html", Resource{test, "text/html"});
    std::string animated = test;
    const auto head = animated.find("</head>");
    animated.insert(head,
                    "<style>.swatch { animation: stable-swatch 1s linear infinite paused; }"
                    "@keyframes stable-swatch { from { opacity: 1; } to { opacity: 1; } }"
                    "</style>");
    resources.emplace(std::string(origin) + "animated.html", Resource{ animated, "text/html" });
    std::string rtl = rtlFixture;
    addPolicy(rtl);
    resources.emplace(std::string(origin) + "rtl.html", Resource{rtl, "text/html"});
    // The delayed markers must appear in the first snapshot of each viewport.
    const auto addMarker = [this](const char *name, const char *encoded) {
        gsize size = 0;
        g_autofree guchar *bytes = g_base64_decode(encoded, &size);
        resources.emplace(std::string(origin) + name,
                          Resource{ std::string(reinterpret_cast<const char *>(bytes), size),
                                    "image/png", 0, true });
    };
    addMarker(
            "marker.png",
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNg+M8AAAICAQB7CYF4AAAAAElFTkSuQmCC");
    addMarker(
            "wide-marker.png",
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNg+P//PwAF/wL+Xg47rQAAAABJRU5ErkJggg==");
    g_autofree gchar *figure = g_build_filename(directory, "figure", nullptr);
    secondBook = g_file_test(figure, G_FILE_TEST_IS_DIR);
    if (!add("chapter1.xhtml", "application/xhtml+xml") || !add("chapter2.xhtml", "application/xhtml+xml")
        || !add("styles.css", "text/css") || !add("ornament.png", "image/png", true)
        || !add("font.ttf", "font/ttf", true)
        || (secondBook && (!add("font.ttf", "font/ttf", false, secondOrigin)
                           || !add("figure/chapter.xhtml", "application/xhtml+xml", false, secondOrigin)
                           || !add("figure/bridge.png", "image/png", false, secondOrigin)))) {
        g_printerr("Missing fixture inputs\n");
        return 1;
    }

    g_autofree gchar *hebrew = g_build_filename(directory, "hebrew.ttf", nullptr);
    hebrewFont = g_file_test(hebrew, G_FILE_TEST_IS_REGULAR);
    if (hebrewFont && !add("hebrew.ttf", "font/ttf", true))
        return 1;
    const auto styleSheet = std::string(style) + (hebrewFont ? hebrewStyle : "");

    g_autoptr(WebKitWebContext) context = webkit_web_context_new();
    auto *security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, "kobalt-epub");
    webkit_web_context_register_uri_scheme(
            context, "kobalt-epub", +[](WebKitURISchemeRequest *request, gpointer data) {
        static_cast<Fixture *>(data)->serve(request);
    }, this, nullptr);
    g_autoptr(GMainLoop) owner = g_main_loop_new(nullptr, FALSE);
    loop = owner;
    view = std::make_unique<retained::View>(context, styleSheet.c_str(), true, nullptr, &timing);
    // A watchdog is a failure bound only. It never decides a frame is fresh.
    const auto timeout = g_timeout_add_seconds(40, +[](gpointer) -> gboolean {
        g_error("Retained view fixture timed out");
        return G_SOURCE_REMOVE;
    }, nullptr);
    g_idle_add(+[](gpointer data) -> gboolean {
        static_cast<Fixture *>(data)->submit();
        return G_SOURCE_REMOVE;
    }, this);
    g_main_loop_run(loop);
    g_source_remove(timeout);
    view.reset();
    return success ? 0 : 1;
}

void Fixture::submit()
{
    const char *name = step < 9 ? "fixture.html" : step < 11 ? "chapter1.xhtml"
            : step == 11 ? "chapter2.xhtml" : step < 16 ? "chapter1.xhtml"
            : step < 18 ? "rtl.html" : "chapter.xhtml";
    const bool resized = step >= 7 && step < 13;
    retained::Page page{std::string(step < 18 ? origin : secondOrigin) + name,
                        resized ? 208u : 160u, resized ? 136u : 120u,
                        0, step >= 3 && step < 13 ? 26u : 18u};
    page.href = step >= 9 && step < 16 ? std::string("OEBPS/") + name : name;
    if (step == 1 || step == 5 || step == 8)
        page.column = 1;
    if (step == 10)
        page.column = 2;
    if (step == 4)
        page.column = 8;
    if (step == 12)
        page.location = chapterAnchor;
    if (step == 14 || step == 15) {
        beforeFailure = view->pageInfo();
        if (!beforeFailure)
            return fail("Missing source checkpoint");
        page.location = chapterAnchor;
        if (step == 14)
            page.location->path = {9999};
        else
            page.location->offset = 2 * 1024 * 1024;
    }
    if (step == 16 || step == 17) {
        page.location = retained::Location{1, "rtl.html", {0}, 2, true};
        if (step == 17)
            page.fontSize = 26;
    }
    if (step == 19) {
        beforeFailure = view->pageInfo();
        page.location = chapterAnchor; // chapter1 of the other publication
    }
    g_autoptr(GCancellable) operation = g_cancellable_new();
    timing = {};
    view->show(page, operation, [this](WebKitImage *image, const char *error) { result(image, error); });
    if (step == 5)
        g_cancellable_cancel(operation);
}

bool Fixture::pixels(WebKitImage *image, bool blue, bool controlled, std::string &hash)
{
    const int width = webkit_image_get_width(image), height = webkit_image_get_height(image);
    const guint stride = webkit_image_get_stride(image);
    gsize size = 0;
    auto *bytes = webkit_image_as_bytes(image);
    if (!bytes || width <= 32 || height <= 32 || stride < guint(width) * 4
        || guint64(stride) * height > g_bytes_get_size(bytes))
        return false;
    const auto *data = static_cast<const guint8 *>(g_bytes_get_data(bytes, &size));
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    unsigned ink = 0;
    bool markerVisible = false;
    for (int y = 0; y < height; ++y) {
        const auto *row = data + size_t(y) * stride;
        g_checksum_update(checksum, row, width * 4);
        for (int x = 0; x < width; ++x) {
            const auto *pixel = row + x * 4;
            if (controlled && x < 32 && y < 32
                && (pixel[0] != (blue ? 255 : 0) || pixel[1]
                    || pixel[2] != (blue ? 0 : 255) || pixel[3] != 255))
                return false;
            if (controlled && !blue && x == 4 && y == 84)
                markerVisible = pixel[0] == (width >= 200 ? 255 : 0) && pixel[1] == 255
                        && pixel[2] == 0 && pixel[3] == 255;
            if ((!controlled || (y >= 32 && y < 68)) && pixel[0] < 128
                && pixel[1] < 128 && pixel[2] < 128)
                ++ink;
        }
    }
    hash = g_checksum_get_string(checksum);
    g_print("retained step=%u size=%dx%d ink=%u sha256=%s\n", step, width, height, ink, hash.c_str());
    return ink > 20 && (!controlled || blue || markerVisible);
}

void Fixture::result(WebKitImage *image, const char *error)
{
    if (((step == 1 || step == 2) && !timing.candidate.reused)
        || ((step == 0 || step == 3 || step == 6 || step == 7 || step == 11 || step == 12)
            && timing.candidate.reused)
        || timing.rollback.reused)
        return fail("Unexpected retained layout reuse");
    if (step == 4 || step == 5 || step == 14 || step == 19) {
        if (image || !error || !view->usable())
            return fail("Failed or cancelled operation did not restore the active view");
        if (step == 14 || step == 19) {
            const auto after = view->pageInfo();
            if (!after || !beforeFailure || after->anchor != beforeFailure->anchor
                || after->column != beforeFailure->column)
                return fail("Unresolved restore changed the confirmed page");
        }
        g_print("retained step=%u rolled back: %s\n", step, error);
    } else {
        if (!image || error)
            return fail(error ? error : "Missing frame");
        std::string hash;
        if (!pixels(image, step == 1 || step == 8, step < 9, hash))
            return fail("Snapshot pixels do not match the requested column or expected text");
        if (step == 0)
            firstHash = hash;
        if (step == 2 && hash != firstHash)
            return fail("Return to first column did not reproduce the original pixels");
        if (step == 3) {
            if (hash == firstHash)
                return fail("Appearance reflow did not change text pixels");
            reflowHash = hash;
        }
        if (step == 6 && hash != reflowHash)
            return fail("Recovery did not reproduce the frame preceding the failed operations");
        if (step == 9)
            chapterHash = hash;
        const auto info = view->pageInfo();
        if (!info || !info->chapterPages || info->column >= info->chapterPages)
            return fail("Missing committed chapter page information");
        if ((step == 1 || step == 2)
            && (timing.candidate.sourceWork[4] < 1 || info->pageEnd.offset != 7
                || info->pageEnd.path != std::vector<unsigned>{ unsigned(step == 1), 1 }))
            return fail("Batched trailing source position differs from the visible text");
        g_print("location step=%u chapter=%s column=%u/%u offset=%d precision=%d restore=%d\n",
                step, info->anchor.href.c_str(), info->column, info->chapterPages, info->anchor.offset,
                int(info->precision), int(info->restoration));
        if (step == 10) {
            if (hash == chapterHash || info->chapterPages < 2 || info->column != 2
                || info->anchor.href != "OEBPS/chapter1.xhtml" || info->anchor.offset < 0)
                return fail("Real chapter turn did not produce a source anchor");
            chapterAnchor = info->anchor;
            turnedHash = hash;
        }
        if (step == 12 && (hash != turnedHash || info->column != 2
                           || info->restoration != retained::Restoration::Exact))
            return fail("Reopening a chapter did not restore its original column");
        if (step == 13 && (info->restoration != retained::Restoration::Exact
                           || info->anchor.href != chapterAnchor.href))
            return fail("Reflow did not resolve the previous source anchor");
        if (step == 15 && info->restoration != retained::Restoration::Approximate)
            return fail("An out-of-range text offset did not report approximate restoration");
        if ((step == 16 || step == 17) && (info->restoration != retained::Restoration::Exact
                                         || info->anchor.href != "rtl.html"))
            return fail("RTL source restoration failed");
        if (step == 18 && info->anchor.href != "chapter.xhtml")
            return fail("The second publication returned the wrong chapter anchor");
    }
    if (resources.at(std::string(origin) + "fixture.html").requests != 1
        || resources.at(std::string(origin) + "wide-marker.png").requests != unsigned(step >= 7)
        || resources.at(std::string(origin) + "chapter1.xhtml").requests
                != unsigned(step >= 9) + unsigned(step >= 12)
        || resources.at(std::string(origin) + "chapter2.xhtml").requests != unsigned(step >= 11))
        return fail("Column operation reloaded the active document or fetched a later chapter");
    if (++step == (secondBook ? 20u : 18u)) {
        success = resources.at(std::string(origin) + "font.ttf").requests > 0
                && resources.at(std::string(origin) + "marker.png").requests == 1
                && resources.at(std::string(origin) + "ornament.png").requests > 0 && !denied
                && !view->sawExternalResource()
                && (!hebrewFont || resources.at(std::string(origin) + "hebrew.ttf").requests > 0);
        if (!success)
            return fail("Expected resource delivery or offline restriction failed");
        g_print("retained fixture passed: delayed resources, same-view rollback, and lazy chapters\n");
        success = false;
        previewCheck(0);
        return;
    }
    g_idle_add(+[](gpointer data) -> gboolean {
        static_cast<Fixture *>(data)->submit();
        return G_SOURCE_REMOVE;
    }, this);
}

void Fixture::previewCheck(unsigned phase)
{
    const auto base = std::string(origin);
    const bool preview = (phase >= 1 && phase <= 6) || phase == 11;
    const bool resize = phase == 3 || phase == 9;
    const char *href = phase == 4 || phase == 6 ? "chapter2.xhtml"
            : phase >= 10                       ? "animated.html"
                                                : "fixture.html";
    const bool nextColumn = (preview && phase != 4) || phase == 7;
    retained::Page page{ base + href, resize ? 208u : 160u, resize ? 136u : 120u,
                         phase == 6 ? 65535u : unsigned(nextColumn),
                         resize || phase == 4 || phase == 6 ? 26u : 18u };
    const auto before = view->pageInfo();
    g_autoptr(GCancellable) operation = g_cancellable_new();
    timing = {};
    view->show(
            page, operation,
            [this, phase, preview, before](WebKitImage *image, const char *error) {
                const auto info = view->pageInfo();
                const auto thumb = view->previewPageInfo();
                const bool rejected = phase == 5 || phase == 6;
                if (!info || (rejected ? image || !error : !image || error) || !view->usable())
                    return fail("Unexpected preview or rollback result");
                if (preview) {
                    const bool fast = phase == 1 || phase == 2;
                    if (!before || before->anchor != info->anchor || before->column != info->column
                        || before->pageEnd != info->pageEnd
                        || before->chapterPages != info->chapterPages || !timing.rollback.domEndUs
                        || timing.rollback.reused != fast)
                        return fail(
                                "Preview changed confirmed geometry or took the wrong rollback path");
                    if (fast
                        && timing.rollback.sourceWork != decltype(timing.rollback.sourceWork){})
                        return fail("Scroll-only rollback ran visible geometry scans");
                    // A viewport change resolves the confirmed anchor instead of the requested column.
                    const unsigned expected = (phase == 3 || phase == 4) ? 0 : 1;
                    if (!rejected
                        && (!thumb || thumb->column != expected
                            || (phase == 4 && thumb->anchor.href != "chapter2.xhtml")))
                        return fail("Preview reported unexpected target geometry");
                }
                if (!rejected && phase != 4) {
                    std::string hash;
                    if (!pixels(image, (preview && phase != 3) || phase == 7, true, hash))
                        return fail("Preview or subsequent turn returned stale pixels");
                    if (phase == 0)
                        previewFirst = hash;
                    if (phase == 1)
                        previewNext = hash;
                    if ((phase == 2 || phase == 7) && hash != previewNext)
                        return fail("Repeated preview differs from the foreground turn");
                    if (phase == 8 && hash != previewFirst)
                        return fail("Foreground return changed the original frame");
                    if (phase == 9 && hash == previewFirst)
                        return fail("Reflow after a preview did not change the frame");
                }
                g_print("preview phase=%u rollback=%d reused=%d\n", phase,
                        timing.rollback.domEndUs != 0, timing.rollback.reused);
                // Read selection geometry in the actual restored view before the next render.
                // Both fixture columns have the same text, so the location path identifies the column.
                if (phase <= 6 || phase >= 10)
                    return view->wordAt(8, 40, [this, phase](retained::Word word) {
                        const bool first = phase == 0 || phase == 10;
                        if (word.text.empty() || (!first && word.start != previewWord))
                            return fail("Preview rollback did not restore live hit testing");
                        if (first)
                            previewWord = word.start;
                        if (phase == 11) {
                            success = true;
                            g_main_loop_quit(loop);
                            return;
                        }
                        previewCheck(phase + 1);
                    });
                previewCheck(phase + 1);
            },
            preview);
    if (phase == 5)
        g_cancellable_cancel(operation);
}

void Fixture::fail(const char *reason)
{
    g_printerr("retained step %u: %s\n", step, reason);
    g_main_loop_quit(loop);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        g_printerr("usage: %s AUTHORIZED_FIXTURE_DIRECTORY\n", argv[0]);
        return 2;
    }
    return Fixture(argv[1]).run();
}

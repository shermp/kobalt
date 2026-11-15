#include "view.h"

#include <array>
#include <cstring>
#include <map>
#include <memory>

namespace {
constexpr char origin[] = "kobalt-epub://book-retained/";
constexpr char style[] = R"CSS(
    @font-face {
        font-family: ReaderFixture;
        src: url(kobalt-epub://book-retained/font.ttf) format("truetype");
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
constexpr char fixture[] = R"HTML(<!doctype html><html><head>
<script>document.documentElement.setAttribute('data-book-script', 'ran')</script>
<style>@import url(https://unexpected.invalid/blocked.css);</style>
</head><body>
<section class="fixture-page red"><div class="swatch"></div><p>WPE type</p><img class="fixture-marker" src="marker.png" alt=""></section>
<section class="fixture-page blue"><div class="swatch"></div><p>WPE type</p></section>
</body></html>)HTML";
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
    bool add(const char *name, const char *type, bool delayed = false);
    void serve(WebKitURISchemeRequest *);
    void submit();
    void result(WebKitImage *, const char *);
    bool pixels(WebKitImage *, bool blue, bool controlled, std::string &hash);
    void fail(const char *);

    const char *directory;
    std::map<std::string, Resource> resources;
    GMainLoop *loop = nullptr;
    std::unique_ptr<retained::View> view;
    unsigned step = 0, denied = 0;
    bool success = false;
    std::string firstHash, reflowHash, chapterHash;
};

Fixture::Fixture(const char *directory) : directory(directory) {}

bool Fixture::add(const char *name, const char *type, bool delayed)
{
    g_autofree gchar *path = g_build_filename(directory, name, nullptr);
    g_autofree gchar *data = nullptr;
    gsize size = 0;
    if (!g_file_get_contents(path, &data, &size, nullptr) || !size || size > 2 * 1024 * 1024)
        return false;
    Resource entry{std::string(data, size), type, 0, delayed};
    if (g_str_equal(type, "application/xhtml+xml"))
        addPolicy(entry.bytes);
    resources.emplace(std::string(origin) + name, std::move(entry));
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
    // One fully opaque green pixel. A delayed response must be visible in
    // the first snapshot, rather than passing on image.decode() alone.
    gsize markerSize = 0;
    g_autofree guchar *marker = g_base64_decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNg+M8AAAICAQB7CYF4AAAAAElFTkSuQmCC", &markerSize);
    resources.emplace(std::string(origin) + "marker.png",
                      Resource{std::string(reinterpret_cast<const char *>(marker), markerSize),
                               "image/png", 0, true});
    if (!add("chapter1.xhtml", "application/xhtml+xml") || !add("chapter2.xhtml", "application/xhtml+xml")
        || !add("styles.css", "text/css") || !add("ornament.png", "image/png", true)
        || !add("font.ttf", "font/ttf", true)) {
        g_printerr("Missing fixture inputs\n");
        return 1;
    }

    g_autoptr(WebKitWebContext) context = webkit_web_context_new();
    auto *security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, "kobalt-epub");
    webkit_web_context_register_uri_scheme(
            context, "kobalt-epub", +[](WebKitURISchemeRequest *request, gpointer data) {
        static_cast<Fixture *>(data)->serve(request);
    }, this, nullptr);
    g_autoptr(GMainLoop) owner = g_main_loop_new(nullptr, FALSE);
    loop = owner;
    view = std::make_unique<retained::View>(context, style);
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
    const char *name = step < 9 ? "fixture.html" : step < 11 ? "chapter1.xhtml" : "chapter2.xhtml";
    const bool resized = step >= 7;
    retained::Page page{std::string(origin) + name, resized ? 208u : 160u,
                        resized ? 136u : 120u, 0, step >= 3 ? 26u : 18u};
    if (step == 1 || step == 5 || step == 8 || step == 10)
        page.column = 1;
    if (step == 4)
        page.column = 8; // fails after applying the candidate layout
    g_autoptr(GCancellable) operation = g_cancellable_new();
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
                markerVisible = pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255;
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
    if (step == 4 || step == 5) {
        if (image || !error || !view->usable())
            return fail("Failed or cancelled operation did not restore the active view");
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
        if (step == 10 && hash == chapterHash)
            return fail("Real chapter turn did not change pixels");
    }
    if (resources.at(std::string(origin) + "fixture.html").requests != 1
        || resources.at(std::string(origin) + "chapter1.xhtml").requests != unsigned(step >= 9)
        || resources.at(std::string(origin) + "chapter2.xhtml").requests != unsigned(step >= 11))
        return fail("Column operation reloaded the active document or fetched a later chapter");
    if (++step == 12) {
        success = resources.at(std::string(origin) + "font.ttf").requests > 0
                && resources.at(std::string(origin) + "marker.png").requests == 1
                && resources.at(std::string(origin) + "ornament.png").requests > 0 && !denied
                && !view->sawExternalResource();
        if (!success)
            return fail("Expected resource delivery or offline restriction failed");
        g_print("retained fixture passed: delayed resources, same-view rollback, and lazy chapters\n");
        g_main_loop_quit(loop);
        return;
    }
    g_idle_add(+[](gpointer data) -> gboolean {
        static_cast<Fixture *>(data)->submit();
        return G_SOURCE_REMOVE;
    }, this);
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

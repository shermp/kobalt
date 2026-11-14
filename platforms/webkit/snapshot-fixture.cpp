#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using Pixel = std::array<unsigned char, 4>;
constexpr Pixel red{ 0, 0, 255, 255 };
constexpr Pixel white{ 255, 255, 255, 255 };
constexpr Pixel blue{ 255, 0, 0, 255 };
constexpr int initialWidth = 160;
constexpr int initialHeight = 120;
constexpr int resizedWidth = 208;
constexpr int resizedHeight = 136;

enum class Step { Initial, Blue, Red, Resize, Mutation, Cancel, Recovery };

struct Fixture : std::enable_shared_from_this<Fixture>
{
    explicit Fixture(WebKitWebView *view) : view(view) { }

    ~Fixture()
    {
        g_object_unref(cancel);
        g_clear_pointer(&initial, g_bytes_unref);
        g_main_loop_unref(loop);
    }

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    WebKitWebView *view;
    GCancellable *cancel = g_cancellable_new();
    Step step = Step::Initial;
    int width = initialWidth, height = initialHeight;
    bool passed = false;
    bool stopped = false;
    GBytes *initial = nullptr;
};

using Pending = std::unique_ptr<std::shared_ptr<Fixture>>;

void stop(Fixture &fixture, const char *message)
{
    fixture.stopped = true;
    g_printerr("%s\n", message);
    g_main_loop_quit(fixture.loop);
}

bool disableAcceleration(WebKitSettings *settings)
{
    const char *required[] = { "HardwareAcceleration", "AcceleratedCompositing",
                               "ForceCompositingMode" };
    unsigned disabled = 0;
    auto *features = webkit_settings_get_all_features();
    for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
        auto *feature = webkit_feature_list_get(features, i);
        for (const auto *identifier : required) {
            if (g_str_equal(webkit_feature_get_identifier(feature), identifier)) {
                webkit_settings_set_feature_enabled(settings, feature, FALSE);
                disabled += !webkit_settings_get_feature_enabled(settings, feature);
            }
        }
    }
    webkit_feature_list_unref(features);
    return disabled == G_N_ELEMENTS(required);
}

void loadPage(Fixture &fixture, bool bluePage)
{
    const char *html = bluePage
            ? "<!doctype html><style>html{background:blue}body{margin:0}</style>"
              "<div style='width:32px;height:32px;background:white'></div>"
            : "<!doctype html><style>html{background:red}body{margin:0}</style>"
              "<div style='width:32px;height:32px;background:white'></div>";
    webkit_web_view_load_html(fixture.view, html, nullptr);
}

void requestSnapshot(Fixture &fixture);

void waitForLayout(Fixture &fixture, const char *script)
{
    webkit_web_view_call_async_javascript_function(
            fixture.view, script, -1, nullptr, nullptr, nullptr, nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                Pending pending(static_cast<std::shared_ptr<Fixture> *>(data));
                auto &fixture = **pending;
                GError *error = nullptr;
                auto *value = webkit_web_view_call_async_javascript_function_finish(
                        WEBKIT_WEB_VIEW(source), result, &error);
                if (!fixture.stopped) {
                    if (value)
                        requestSnapshot(fixture);
                    else
                        stop(fixture, error ? error->message : "Layout check failed");
                }
                g_clear_object(&value);
                g_clear_error(&error);
            },
            new std::shared_ptr<Fixture>(fixture.shared_from_this()));
}

bool checkPixels(Fixture &fixture, WebKitImage *image)
{
    const int width = webkit_image_get_width(image);
    const int height = webkit_image_get_height(image);
    const auto stride = webkit_image_get_stride(image);
    gsize size = 0;
    const auto *pixels = static_cast<const unsigned char *>(
            g_bytes_get_data(webkit_image_as_bytes(image), &size));
    const auto rowBytes = static_cast<gsize>(fixture.width) * 4;
    if (width != fixture.width || height != fixture.height || stride < rowBytes
        || stride > size / static_cast<gsize>(fixture.height))
        return false;

    // Own tightly packed pixels and omit any WebKit row padding from the hash.
    std::vector<unsigned char> frame(rowBytes * height);
    for (int y = 0; y < height; ++y)
        std::memcpy(frame.data() + y * rowBytes, pixels + gsize(y) * stride, rowBytes);

    const auto &paper = fixture.step == Step::Blue || fixture.step == Step::Mutation
                    || fixture.step == Step::Recovery
            ? blue
            : red;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto &expected = x < 32 && y < 32 ? white : paper;
            if (std::memcmp(frame.data() + y * rowBytes + x * 4, expected.data(), 4) != 0) {
                g_printerr("Unexpected pixel at %d,%d in step %d\n", x, y, int(fixture.step));
                return false;
            }
        }
    }

    auto *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, frame.data(), frame.size());
    g_print("step=%d width=%d height=%d stride=%u format=BGRA8888 sha256=%s\n", int(fixture.step),
            width, height, stride, hash);
    g_free(hash);

    if (fixture.step == Step::Initial)
        fixture.initial = g_bytes_new(frame.data(), frame.size());
    if (fixture.step == Step::Red)
        return std::memcmp(frame.data(), g_bytes_get_data(fixture.initial, nullptr), frame.size())
                == 0;
    return true;
}

void advance(Fixture &fixture)
{
    switch (fixture.step) {
    case Step::Initial:
        fixture.step = Step::Blue;
        loadPage(fixture, true);
        break;
    case Step::Blue:
        fixture.step = Step::Red;
        loadPage(fixture, false);
        break;
    case Step::Red:
        fixture.step = Step::Resize;
        fixture.width = resizedWidth;
        fixture.height = resizedHeight;
        wpe_toplevel_resize(wpe_view_get_toplevel(webkit_web_view_get_wpe_view(fixture.view)),
                            fixture.width, fixture.height);
        waitForLayout(fixture, R"JS(
            await new Promise(resolve => {
                const ready = () => {
                    if (innerWidth === 208 && innerHeight === 136) {
                        removeEventListener('resize', ready);
                        resolve();
                    }
                };
                addEventListener('resize', ready);
                ready();
            });
            return innerWidth;
        )JS");
        break;
    case Step::Resize:
        fixture.step = Step::Mutation;
        waitForLayout(fixture, R"JS(
            await new Promise(resolve => requestAnimationFrame(() => {
                document.documentElement.style.background = 'blue';
                resolve();
            }));
            return getComputedStyle(document.documentElement).backgroundColor;
        )JS");
        break;
    case Step::Mutation:
        fixture.step = Step::Cancel;
        requestSnapshot(fixture);
        g_cancellable_cancel(fixture.cancel);
        break;
    case Step::Cancel:
        fixture.step = Step::Recovery;
        requestSnapshot(fixture);
        break;
    case Step::Recovery:
        fixture.passed = true;
        g_main_loop_quit(fixture.loop);
        break;
    }
}

void requestSnapshot(Fixture &fixture)
{
    webkit_web_view_get_snapshot(
            fixture.view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
            fixture.step == Step::Cancel ? fixture.cancel : nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                Pending pending(static_cast<std::shared_ptr<Fixture> *>(data));
                auto &fixture = **pending;
                GError *error = nullptr;
                auto *image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result,
                                                                  &error);
                if (fixture.stopped) {
                    g_clear_object(&image);
                    g_clear_error(&error);
                    return;
                }
                bool passed;
                if (fixture.step == Step::Cancel) {
                    passed = !image && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
                    if (passed)
                        g_print("snapshot cancelled\n");
                } else
                    passed = image && checkPixels(fixture, image);
                if (passed)
                    advance(fixture);
                else
                    stop(fixture, error ? error->message : "Snapshot assertion failed");
                g_clear_object(&image);
                g_clear_error(&error);
            },
            new std::shared_ptr<Fixture>(fixture.shared_from_this()));
}
} // namespace

int main()
{
    auto *settings = webkit_settings_new();
    if (!disableAcceleration(settings)) {
        g_printerr("Software rendering preferences are unavailable\n");
        g_object_unref(settings);
        return 1;
    }
    auto *display = wpe_display_headless_new();
    auto *network = webkit_network_session_new_ephemeral();
    auto *view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "settings",
                                              settings, "network-session", network, nullptr));
    auto fixture = std::make_shared<Fixture>(view);
    wpe_toplevel_resize(wpe_view_get_toplevel(webkit_web_view_get_wpe_view(view)), fixture->width,
                        fixture->height);
    g_signal_connect(view, "load-changed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent event, gpointer data) {
                         if (event == WEBKIT_LOAD_FINISHED)
                             requestSnapshot(*static_cast<Fixture *>(data));
                     }),
                     fixture.get());
    g_signal_connect(
            view, "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer data) {
                stop(*static_cast<Fixture *>(data), "Web process terminated");
            }),
            fixture.get());
    auto *deadline = g_timeout_source_new_seconds(30);
    g_source_set_callback(
            deadline,
            +[](gpointer data) -> gboolean {
                stop(*static_cast<Fixture *>(data), "Timed out waiting for the fixture");
                return G_SOURCE_REMOVE;
            },
            fixture.get(), nullptr);
    g_source_attach(deadline, nullptr);

    loadPage(*fixture, false);
    g_main_loop_run(fixture->loop);

    g_source_destroy(deadline);
    g_source_unref(deadline);
    g_signal_handlers_disconnect_by_data(view, fixture.get());
    g_object_unref(view);
    g_object_unref(network);
    g_object_unref(display);
    g_object_unref(settings);
    return fixture->passed ? 0 : 1;
}

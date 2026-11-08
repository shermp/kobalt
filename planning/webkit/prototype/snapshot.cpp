#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>
#include <png.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Isolated M0 probe. This never changes the sandbox configuration itself.
struct Probe
{
    GMainLoop *loop = nullptr;
    GCancellable *cancellable = nullptr;
    WebKitWebView *view = nullptr;
    const char *output = nullptr;
    int width = 0, height = 0, result = 1;
    guint deadline = 0, idleSource = 0, idleMilliseconds = 0;
    unsigned captures = 0;
    bool updateOnce = false;
    gint64 updateStarted = 0;
    gint64 started = 0;
    unsigned shmFrames = 0, otherFrames = 0;
    bool finished = false;
};

static void finish(Probe &probe, int result, const char *message)
{
    if (probe.finished)
        return;
    probe.finished = true;
    probe.result = result;
    if (probe.idleSource) {
        g_source_remove(probe.idleSource);
        probe.idleSource = 0;
    }
    if (probe.cancellable)
        g_cancellable_cancel(probe.cancellable);
    g_printerr("%s\n", message);
    g_main_loop_quit(probe.loop);
}

struct FixtureResources
{
    gchar *document;
    gchar *directory;

    ~FixtureResources()
    {
        g_free(document);
        g_free(directory);
    }
};

// Fixed local resources with explicit MIME types; no desktop MIME database.
static void loadFixtureResource(WebKitURISchemeRequest *request, gpointer data)
{
    auto &resources = *static_cast<FixtureResources *>(data);
    const char *path = webkit_uri_scheme_request_get_path(request);
    const char *mime = nullptr;
    gchar *filename = nullptr;
    if (g_str_equal(path, "/document.xhtml")) {
        mime = "application/xhtml+xml";
        filename = g_strdup(resources.document);
    } else if (g_str_equal(path, "/font.ttf")) {
        mime = "font/ttf";
        filename = g_build_filename(resources.directory, "font.ttf", nullptr);
    } else if (g_str_equal(path, "/raster.png")) {
        mime = "image/png";
        filename = g_build_filename(resources.directory, "raster.png", nullptr);
    } else if (g_str_equal(path, "/illustration.svg")) {
        mime = "image/svg+xml";
        filename = g_build_filename(resources.directory, "illustration.svg", nullptr);
    }
    GError *error = nullptr;
    gchar *contents = nullptr;
    gsize length = 0;
    if (!filename)
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown fixture resource");
    else
        g_file_get_contents(filename, &contents, &length, &error);
    g_free(filename);
    if (error) {
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    GInputStream *stream = g_memory_input_stream_new_from_data(contents, length, g_free);
    g_print("resource=%s mime=%s bytes=%" G_GSIZE_FORMAT "\n", path, mime, length);
    webkit_uri_scheme_request_finish(request, stream, length, mime);
    g_object_unref(stream);
}

static bool writeRaster(const char *directory)
{
    std::vector<unsigned char> pixels(64 * 64 * 3);
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            const bool blue = ((x / 8) + (y / 8)) % 2;
            const unsigned offset = (y * 64 + x) * 3;
            pixels[offset] = blue ? 32 : 220;
            pixels[offset + 1] = blue ? 90 : 235;
            pixels[offset + 2] = blue ? 185 : 250;
        }
    }
    png_image image = {};
    image.version = PNG_IMAGE_VERSION;
    image.width = 64;
    image.height = 64;
    image.format = PNG_FORMAT_RGB;
    gchar *path = g_build_filename(directory, "raster.png", nullptr);
    const bool written = png_image_write_to_file(&image, path, 0, pixels.data(), 0, nullptr);
    if (!written)
        g_printerr("Fixture PNG: %s\n", image.message);
    g_free(path);
    png_image_free(&image);
    return written;
}

static void scriptReady(GObject *, GAsyncResult *, gpointer);

static void snapshotReady(GObject *object, GAsyncResult *result, gpointer data)
{
    auto &probe = *static_cast<Probe *>(data);
    GError *error = nullptr;
    WebKitImage *image =
            webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (probe.finished) {
        g_clear_object(&image);
        g_clear_error(&error);
        return;
    }
    if (!image) {
        finish(probe, 1, error ? error->message : "Snapshot failed");
        g_clear_error(&error);
        return;
    }
    const int width = webkit_image_get_width(image), height = webkit_image_get_height(image);
    const guint stride = webkit_image_get_stride(image);
    gsize size = 0;
    const auto *pixels = static_cast<const unsigned char *>(
            g_bytes_get_data(webkit_image_as_bytes(image), &size));
    if (width != probe.width || height != probe.height || stride < guint(width) * 4
        || size < gsize(stride) * gsize(height)) {
        g_object_unref(image);
        finish(probe, 1, "Invalid snapshot dimensions or byte span");
        return;
    }
    unsigned nonwhite = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto *pixel = pixels + gsize(y) * stride + gsize(x) * 4;
            nonwhite += pixel[0] < 240 || pixel[1] < 240 || pixel[2] < 240;
        }
    }
    ++probe.captures;
    const gint64 requestStart = probe.captures == 1 ? probe.started : probe.updateStarted;
    g_print("snapshot_index=%u snapshot_ready_us=%" G_GINT64_FORMAT "\n", probe.captures,
            g_get_monotonic_time() - requestStart);
    const gint64 writeStarted = g_get_monotonic_time();
    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    png.width = width;
    png.height = height;
    png.format = PNG_FORMAT_BGRA;
    gchar *output = probe.captures == 1 ? g_strdup(probe.output)
                                        : g_strconcat(probe.output, ".second.png", nullptr);
    const bool written = png_image_write_to_file(&png, output, 0, pixels, stride, nullptr);
    g_free(output);
    g_print("snapshot=%dx%d stride=%u nonwhite=%u shm_frames=%u other_frames=%u\n", width, height,
            stride, nonwhite, probe.shmFrames, probe.otherFrames);
    if (!written)
        g_printerr("PNG: %s\n", png.message);
    g_print("png_write_us=%" G_GINT64_FORMAT "\n", g_get_monotonic_time() - writeStarted);
    png_image_free(&png);
    g_object_unref(image);
    if (!written || nonwhite <= 1000) {
        finish(probe, 1, "Invalid or empty image");
        return;
    }
    if (probe.updateOnce && probe.captures == 1) {
        probe.updateStarted = g_get_monotonic_time();
        constexpr const char *update =
                "document.getElementById('js-status').textContent = 'Second trusted update.'; await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))); return 'TRUSTED_JS_OK|updated';";
        webkit_web_view_call_async_javascript_function(probe.view, update, -1, nullptr, "kobalt-m0",
                                                       "kobalt-m0://trusted-update",
                                                       probe.cancellable, scriptReady, &probe);
        return;
    }
    if (probe.idleMilliseconds) {
        g_print("IDLE_READY\n");
        std::fflush(stdout);
        probe.idleSource = g_timeout_add(
                probe.idleMilliseconds,
                +[](gpointer data) -> gboolean {
                    auto &probe = *static_cast<Probe *>(data);
                    probe.idleSource = 0;
                    finish(probe, 0, "SNAPSHOT_OK");
                    return G_SOURCE_REMOVE;
                },
                &probe);
        return;
    }
    finish(probe, 0, "SNAPSHOT_OK");
}

static void scriptReady(GObject *object, GAsyncResult *result, gpointer data)
{
    auto &probe = *static_cast<Probe *>(data);
    GError *error = nullptr;
    JSCValue *value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(object),
                                                                            result, &error);
    if (probe.finished) {
        g_clear_object(&value);
        g_clear_error(&error);
        return;
    }
    if (!value) {
        finish(probe, 1, error ? error->message : "Trusted script failed");
        g_clear_error(&error);
        return;
    }
    gchar *text = jsc_value_to_string(value);
    const bool valid = text && g_str_has_prefix(text, "TRUSTED_JS_OK|");
    g_print("%s\n", text ? text : "No script result");
    g_free(text);
    g_object_unref(value);
    if (!valid) {
        finish(probe, 1, "Unexpected trusted script result");
        return;
    }
    webkit_web_view_get_snapshot(probe.view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, probe.cancellable, snapshotReady,
                                 &probe);
}

static bool disableAcceleration(WebKitSettings *settings)
{
    const char *required[] = { "HardwareAcceleration", "AcceleratedCompositing",
                               "ForceCompositingMode" };
    unsigned found = 0;
    WebKitFeatureList *features = webkit_settings_get_all_features();
    for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
        WebKitFeature *feature = webkit_feature_list_get(features, i);
        for (const char *identifier : required) {
            if (g_str_equal(webkit_feature_get_identifier(feature), identifier)) {
                webkit_settings_set_feature_enabled(settings, feature, FALSE);
                found += !webkit_settings_get_feature_enabled(settings, feature);
                g_print("disabled_feature=%s\n", identifier);
            }
        }
    }
    webkit_feature_list_unref(features);
    return found == 3;
}

int main(int argc, char **argv)
{
    const bool experiment = argc == 6 && !std::strcmp(argv[5], "--allow-unsandboxed-experiment");
    if (argc != 5 && !experiment) {
        g_printerr(
                "Usage: %s fixture.xhtml output.png width height [--allow-unsandboxed-experiment]\n",
                argv[0]);
        return 2;
    }
    const char *overrideValue = std::getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    const bool overrideEnabled = overrideValue && std::strcmp(overrideValue, "0");
    if (overrideEnabled != experiment) {
        g_printerr(
                "Sandbox override requires both the explicit experiment argument and per-run environment variable\n");
        return 2;
    }
    if (!g_setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", TRUE))
        return 1;
    g_print("skia_cpu_rendering=1\n");
    Probe probe;
    probe.started = g_get_monotonic_time();
    probe.updateOnce = g_strcmp0(g_getenv("KOBALT_M0_UPDATE_ONCE"), "1") == 0;
    const char *idle = g_getenv("KOBALT_M0_IDLE_MILLISECONDS");
    const int idleMilliseconds = idle ? std::atoi(idle) : 0;
    if (idleMilliseconds < 0 || idleMilliseconds > 10000)
        return 2;
    probe.idleMilliseconds = idleMilliseconds;
    probe.output = argv[2];
    probe.width = std::atoi(argv[3]);
    probe.height = std::atoi(argv[4]);
    if (probe.width < 100 || probe.width > 4096 || probe.height < 100 || probe.height > 4096)
        return 2;
    gchar *fixture = g_canonicalize_filename(argv[1], nullptr);
    gchar *directory = g_path_get_dirname(fixture);
    if (!writeRaster(directory))
        return 1;
    gchar *uri = g_strdup("kobalt-m0://fixture/document.xhtml");
    g_print("sandbox_override=%s\n", experiment ? "explicit-experiment" : "none");
    auto *display = wpe_display_headless_new();
    auto *settings = webkit_settings_new();
    if (!disableAcceleration(settings)) {
        g_printerr("Required software rendering preferences are unavailable\n");
        g_free(directory);
        g_free(fixture);
        g_free(uri);
        return 1;
    }
    auto *context = webkit_web_context_new();
    auto *resources = new FixtureResources{ fixture, directory };
    webkit_web_context_register_uri_scheme(
            context, "kobalt-m0", loadFixtureResource, resources,
            +[](gpointer data) { delete static_cast<FixtureResources *>(data); });
    auto *security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_local(security, "kobalt-m0");
    webkit_security_manager_register_uri_scheme_as_secure(security, "kobalt-m0");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "kobalt-m0");
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    auto *network = webkit_network_session_new_ephemeral();
    probe.loop = g_main_loop_new(nullptr, FALSE);
    probe.cancellable = g_cancellable_new();
    probe.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "settings",
                                              settings, "web-context", context, "network-session",
                                              network, nullptr));
    auto *view = webkit_web_view_get_wpe_view(probe.view);
    wpe_toplevel_resize(wpe_view_get_toplevel(view), probe.width, probe.height);
    g_signal_connect(view, "buffer-rendered",
                     G_CALLBACK(+[](WPEView *, WPEBuffer *buffer, gpointer data) {
                         auto &probe = *static_cast<Probe *>(data);
                         if (WPE_IS_BUFFER_SHM(buffer))
                             ++probe.shmFrames;
                         else
                             ++probe.otherFrames;
                     }),
                     &probe);
    g_signal_connect(probe.view, "load-changed",
                     G_CALLBACK(+[](WebKitWebView *view, WebKitLoadEvent event, gpointer data) {
                         if (event != WEBKIT_LOAD_FINISHED)
                             return;
                         constexpr const char *body = R"JS(
            const faces = await document.fonts.load('24px "M0 Serif"');
            await document.fonts.ready;
            await Promise.all(Array.from(document.images, image => image.decode()));
            if (!faces.length || !document.fonts.check('24px "M0 Serif"')) throw new Error('Embedded font did not load: ' + JSON.stringify({url: document.URL, type: document.contentType, text: document.documentElement.outerHTML.slice(0,500), size: document.fonts.size, family: getComputedStyle(document.body).fontFamily, rules: Array.from(document.styleSheets, sheet => Array.from(sheet.cssRules, rule => rule.cssText))}));
            const raster = document.getElementById('raster');
            if (raster.naturalWidth !== 64) throw new Error('Raster image did not decode');
            const math = document.getElementById('math').getBoundingClientRect();
            if (!math.width || !math.height) throw new Error('MathML has no layout');
            if (typeof WebGLRenderingContext !== 'undefined' || navigator.gpu !== undefined) throw new Error('Unexpected GPU API');
            document.getElementById('js-status').textContent = 'Trusted JavaScript executed.';
            await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
            return 'TRUSTED_JS_OK|' + JSON.stringify({font: faces[0].family, images: document.images.length,
                viewport: [innerWidth, innerHeight], mathWidth: document.getElementById('math').getBoundingClientRect().width});
        )JS";
                         webkit_web_view_call_async_javascript_function(
                                 view, body, -1, nullptr, "kobalt-m0", "kobalt-m0://trusted-script",
                                 static_cast<Probe *>(data)->cancellable, scriptReady, data);
                     }),
                     &probe);
    g_signal_connect(probe.view, "load-failed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, gchar *, GError *error,
                                    gpointer data) -> gboolean {
                         finish(*static_cast<Probe *>(data), 1, error->message);
                         return TRUE;
                     }),
                     &probe);
    g_signal_connect(
            probe.view, "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer data) {
                finish(*static_cast<Probe *>(data), 1, "Web process terminated before completion");
            }),
            &probe);
    probe.deadline = g_timeout_add_seconds(
            60,
            +[](gpointer data) -> gboolean {
                auto &probe = *static_cast<Probe *>(data);
                probe.deadline = 0;
                finish(probe, 1, "Render deadline expired");
                return G_SOURCE_REMOVE;
            },
            &probe);
    webkit_web_view_load_uri(probe.view, uri);
    g_free(uri);
    if (!probe.finished)
        g_main_loop_run(probe.loop);
    if (probe.deadline)
        g_source_remove(probe.deadline);
    g_object_unref(probe.view);
    g_object_unref(network);
    g_object_unref(context);
    g_object_unref(settings);
    g_object_unref(display);
    while (g_main_context_pending(nullptr))
        g_main_context_iteration(nullptr, FALSE);
    g_object_unref(probe.cancellable);
    g_main_loop_unref(probe.loop);
    return probe.result;
}

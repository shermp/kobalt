#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>
#include <glib-unix.h>
#include <png.h>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

struct Renderer
{
    GMainLoop *loop = nullptr;
    WebKitWebView *view = nullptr;
    GCancellable *cancellable = nullptr;
    std::string bundle, runtime, input;
    std::deque<std::string> output;
    size_t queued = 0, written = 0;
    guint inputSource = 0, outputSource = 0, deadline = 0, drainSource = 0;
    unsigned lastId = 0, request = 0;
    int width = 0, height = 0, result = 0;
    bool stopping = false;
};

static void stop(Renderer &renderer, int result)
{
    renderer.result = result;
    renderer.stopping = true;
    g_cancellable_cancel(renderer.cancellable);
    if (renderer.deadline) {
        g_source_remove(renderer.deadline);
        renderer.deadline = 0;
    }
    if (renderer.output.empty())
        g_main_loop_quit(renderer.loop);
    else if (!renderer.drainSource)
        renderer.drainSource = g_timeout_add_seconds(
                2,
                +[](gpointer data) -> gboolean {
                    auto &renderer = *static_cast<Renderer *>(data);
                    renderer.drainSource = 0;
                    renderer.result = 1;
                    g_main_loop_quit(renderer.loop);
                    return G_SOURCE_REMOVE;
                },
                &renderer);
}

static gboolean writeOutput(gint fd, GIOCondition condition, gpointer data)
{
    auto &renderer = *static_cast<Renderer *>(data);
    while (!(condition & (G_IO_ERR | G_IO_HUP)) && !renderer.output.empty()) {
        const auto &line = renderer.output.front();
        ssize_t size = write(fd, line.data() + renderer.written, line.size() - renderer.written);
        if (size < 0 && (errno == EAGAIN || errno == EINTR))
            return G_SOURCE_CONTINUE;
        if (size <= 0)
            break;
        renderer.written += size;
        renderer.queued -= size;
        if (renderer.written == line.size()) {
            renderer.output.pop_front();
            renderer.written = 0;
        }
    }
    if (!renderer.output.empty()) {
        renderer.output.clear();
        renderer.queued = renderer.written = 0;
        stop(renderer, 1);
    }
    renderer.outputSource = 0;
    if (renderer.stopping)
        g_main_loop_quit(renderer.loop);
    return G_SOURCE_REMOVE;
}

static void reply(Renderer &renderer, unsigned id, const std::string &body)
{
    std::string line = "KWRP/1 " + std::to_string(id) + " " + body + "\n";
    if (renderer.queued + line.size() > 65536) {
        renderer.output.clear();
        renderer.queued = renderer.written = 0;
        stop(renderer, 1);
        return;
    }
    renderer.queued += line.size();
    renderer.output.push_back(std::move(line));
    if (!renderer.outputSource)
        renderer.outputSource =
                g_unix_fd_add(STDOUT_FILENO, GIOCondition(G_IO_OUT | G_IO_ERR | G_IO_HUP),
                              writeOutput, &renderer);
}

static void renderError(Renderer &renderer, const char *code, GError *error = nullptr)
{
    if (error)
        g_printerr("kobalt-webkit: %s: %s\n", code, error->message);
    if (renderer.deadline) {
        g_source_remove(renderer.deadline);
        renderer.deadline = 0;
    }
    reply(renderer, renderer.request, std::string("ERROR ") + code);
    // A failed render ends this worker; a fresh process is the M1 recovery boundary.
    stop(renderer, 1);
}

static void snapshotReady(GObject *object, GAsyncResult *result, gpointer data)
{
    auto &renderer = *static_cast<Renderer *>(data);
    GError *error = nullptr;
    WebKitImage *image =
            webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (renderer.stopping) {
        g_clear_object(&image);
        g_clear_error(&error);
        return;
    }
    if (!image) {
        renderError(renderer, "snapshot-failed", error);
        g_clear_error(&error);
        return;
    }
    int width = webkit_image_get_width(image), height = webkit_image_get_height(image);
    guint stride = webkit_image_get_stride(image);
    gsize size = 0;
    const void *pixels = g_bytes_get_data(webkit_image_as_bytes(image), &size);
    if (width != renderer.width || height != renderer.height || stride < guint(width) * 4
        || size / stride < gsize(height)) {
        g_object_unref(image);
        renderError(renderer, "invalid-image");
        return;
    }
    // M1 uses a run-owned PNG. Owned pixel buffers and EPUB loading belong to M2.
    std::string path = renderer.runtime + "/snapshot.png";
    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    png.width = width;
    png.height = height;
    png.format = PNG_FORMAT_BGRA;
    bool written = png_image_write_to_file(&png, path.c_str(), 0, pixels, stride, nullptr);
    if (!written)
        g_printerr("kobalt-webkit: PNG: %s\n", png.message);
    png_image_free(&png);
    g_object_unref(image);
    if (!written) {
        renderError(renderer, "image-write-failed");
        return;
    }
    g_source_remove(renderer.deadline);
    renderer.deadline = 0;
    reply(renderer, renderer.request,
          "OK SNAPSHOT " + std::to_string(width) + " " + std::to_string(height) + " " + path);
    renderer.request = 0;
}

static void scriptReady(GObject *object, GAsyncResult *result, gpointer data)
{
    auto &renderer = *static_cast<Renderer *>(data);
    GError *error = nullptr;
    JSCValue *value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(object),
                                                                            result, &error);
    if (renderer.stopping) {
        g_clear_object(&value);
        g_clear_error(&error);
        return;
    }
    if (!value || !jsc_value_is_boolean(value) || !jsc_value_to_boolean(value)) {
        g_clear_object(&value);
        renderError(renderer, "fixture-not-ready", error);
        g_clear_error(&error);
        return;
    }
    g_object_unref(value);
    webkit_web_view_get_snapshot(renderer.view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, renderer.cancellable, snapshotReady,
                                 &renderer);
}

static void resource(WebKitURISchemeRequest *request, gpointer data)
{
    auto &renderer = *static_cast<Renderer *>(data);
    const char *path = webkit_uri_scheme_request_get_path(request);
    const char *mime = nullptr;
    if (!g_strcmp0(path, "/document.xhtml"))
        mime = "application/xhtml+xml";
    else if (!g_strcmp0(path, "/font.ttf"))
        mime = "font/ttf";
    else if (!g_strcmp0(path, "/illustration.svg"))
        mime = "image/svg+xml";
    else if (!g_strcmp0(path, "/raster.png"))
        mime = "image/png";
    if (!mime) {
        GError *error =
                g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown fixture resource");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    std::string filename = !std::strcmp(path, "/raster.png") ? renderer.runtime + path
                                                             : renderer.bundle + "/share/fixture"
                    + (!std::strcmp(path, "/document.xhtml") ? "/fixture.xhtml" : path);
    GFile *file = g_file_new_for_path(filename.c_str());
    GError *error = nullptr;
    GFileInputStream *stream = g_file_read(file, nullptr, &error);
    if (stream) {
        webkit_uri_scheme_request_finish(request, G_INPUT_STREAM(stream), -1, mime);
        g_object_unref(stream);
    } else {
        webkit_uri_scheme_request_finish_error(request, error);
        g_clear_error(&error);
    }
    g_object_unref(file);
}

static bool number(const std::string &text, unsigned &value)
{
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc() && result.ptr == text.data() + text.size();
}

static void command(Renderer &renderer, const std::string &line)
{
    std::istringstream stream(line);
    std::string version, identity, action, extra;
    unsigned id = 0;
    stream >> version >> identity >> action;
    if (version != "KWRP/1" || !number(identity, id) || !id || id <= renderer.lastId) {
        reply(renderer, 0, "ERROR invalid-request");
        stop(renderer, 2);
        return;
    }
    renderer.lastId = id;
    if (action == "RENDER") {
        std::string widthText, heightText;
        unsigned width = 0, height = 0;
        stream >> widthText >> heightText;
        if (!number(widthText, width) || !number(heightText, height) || width < 100 || width > 2048
            || height < 100 || height > 2048 || (stream >> extra)) {
            reply(renderer, id, "ERROR invalid-viewport");
            return;
        }
        if (renderer.request) {
            reply(renderer, id, "ERROR busy");
            return;
        }
        renderer.request = id;
        renderer.width = width;
        renderer.height = height;
        WPEView *view = webkit_web_view_get_wpe_view(renderer.view);
        wpe_toplevel_resize(wpe_view_get_toplevel(view), width, height);
        renderer.deadline = g_timeout_add_seconds(
                60,
                +[](gpointer data) -> gboolean {
                    auto &renderer = *static_cast<Renderer *>(data);
                    renderer.deadline = 0;
                    renderError(renderer, "render-timeout");
                    return G_SOURCE_REMOVE;
                },
                &renderer);
        webkit_web_view_load_uri(renderer.view, "kobalt-runtime://fixture/document.xhtml");
    } else if (stream >> extra)
        reply(renderer, id, "ERROR invalid-request");
    else if (action == "PING")
        reply(renderer, id, "OK PONG");
    else if (action == "SHUTDOWN") {
        if (renderer.request)
            reply(renderer, renderer.request, "ERROR cancelled");
        reply(renderer, id, "OK BYE");
        stop(renderer, 0);
    } else
        reply(renderer, id, "ERROR unknown-command");
}

static gboolean readInput(gint fd, GIOCondition condition, gpointer data)
{
    auto &renderer = *static_cast<Renderer *>(data);
    char buffer[4096];
    ssize_t size = read(fd, buffer, sizeof(buffer));
    if (size < 0 && (errno == EAGAIN || errno == EINTR) && !(condition & (G_IO_HUP | G_IO_ERR)))
        return G_SOURCE_CONTINUE;
    if (size <= 0) {
        renderer.inputSource = 0;
        stop(renderer, size == 0 ? 0 : 1);
        return G_SOURCE_REMOVE;
    }
    renderer.input.append(buffer, size);
    while (!renderer.stopping) {
        auto newline = renderer.input.find('\n');
        if (newline == std::string::npos)
            break;
        if (newline > 256)
            break;
        command(renderer, renderer.input.substr(0, newline));
        renderer.input.erase(0, newline + 1);
    }
    if (renderer.input.size() > 256 && !renderer.stopping) {
        reply(renderer, 0, "ERROR oversized-request");
        stop(renderer, 2);
    }
    if (renderer.stopping) {
        renderer.inputSource = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static bool softwareSettings(WebKitSettings *settings)
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
            }
        }
    }
    webkit_feature_list_unref(features);
    return found == 3;
}

int main()
{
    const char *bundle = g_getenv("KOBALT_WEBKIT_BUNDLE"),
               *runtime = g_getenv("KOBALT_WEBKIT_RUNTIME");
    if (!bundle || !runtime || bundle[0] != '/' || runtime[0] != '/')
        return 2;
    const char *mode = g_getenv("KOBALT_WEBKIT_SANDBOX_MODE");
    bool experiment = !g_strcmp0(mode, "experiment");
    bool deviceUnsandboxed = !g_strcmp0(mode, "device-unsandboxed");
    if (!mode || (!experiment && !deviceUnsandboxed && std::strcmp(mode, "required")))
        return 2;
    const char *overrideValue = g_getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    if (experiment || deviceUnsandboxed) {
        if (g_strcmp0(overrideValue, "1"))
            return 2;
    } else if (overrideValue && std::strcmp(overrideValue, "0"))
        return 2;
    signal(SIGPIPE, SIG_IGN);
    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) || fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK))
        return 2;
    Renderer renderer;
    renderer.bundle = bundle;
    renderer.runtime = runtime;
    renderer.loop = g_main_loop_new(nullptr, FALSE);
    renderer.cancellable = g_cancellable_new();
    // Fixed generated resource stays in the run directory, never in the installed package.
    std::vector<unsigned char> pixels(64 * 64 * 3);
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            bool blue = ((x / 8) + (y / 8)) % 2;
            unsigned offset = (y * 64 + x) * 3;
            pixels[offset] = blue ? 32 : 220;
            pixels[offset + 1] = blue ? 90 : 235;
            pixels[offset + 2] = blue ? 185 : 250;
        }
    }
    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    png.width = png.height = 64;
    png.format = PNG_FORMAT_RGB;
    bool rasterReady = png_image_write_to_file(&png, (renderer.runtime + "/raster.png").c_str(), 0,
                                               pixels.data(), 0, nullptr);
    png_image_free(&png);
    if (!rasterReady)
        return 1;
    auto *display = wpe_display_headless_new();
    auto *settings = webkit_settings_new();
    if (!softwareSettings(settings))
        return 1;
    auto *context = webkit_web_context_new();
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    webkit_web_context_add_path_to_sandbox(context, bundle, TRUE);
    webkit_web_context_register_uri_scheme(context, "kobalt-runtime", resource, &renderer, nullptr);
    auto *security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_local(security, "kobalt-runtime");
    webkit_security_manager_register_uri_scheme_as_secure(security, "kobalt-runtime");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "kobalt-runtime");
    auto *network = webkit_network_session_new_ephemeral();
    renderer.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
                                                 "settings", settings, "web-context", context,
                                                 "network-session", network, nullptr));
    g_signal_connect(
            renderer.view, "load-changed",
            G_CALLBACK(+[](WebKitWebView *view, WebKitLoadEvent event, gpointer data) {
                auto &renderer = *static_cast<Renderer *>(data);
                if (event != WEBKIT_LOAD_FINISHED || renderer.stopping || !renderer.request)
                    return;
                constexpr const char *body = R"JS(
            const faces = await document.fonts.load('24px "M0 Serif"');
            await document.fonts.ready;
            await Promise.all(Array.from(document.images, image => image.decode()));
            if (!faces.length || document.getElementById('raster').naturalWidth !== 64) return false;
            const math = document.getElementById('math').getBoundingClientRect();
            if (!math.width || !math.height || typeof WebGLRenderingContext !== 'undefined' || navigator.gpu !== undefined) return false;
            document.getElementById('js-status').textContent = 'Trusted JavaScript executed.';
            await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
            return true;
        )JS";
                webkit_web_view_call_async_javascript_function(
                        view, body, -1, nullptr, "kobalt-runtime", "kobalt-runtime://trusted",
                        renderer.cancellable, scriptReady, &renderer);
            }),
            &renderer);
    g_signal_connect(renderer.view, "load-failed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, gchar *, GError *error,
                                    gpointer data) -> gboolean {
                         auto &renderer = *static_cast<Renderer *>(data);
                         if (!renderer.stopping)
                             renderError(renderer, "load-failed", error);
                         return TRUE;
                     }),
                     &renderer);
    g_signal_connect(
            renderer.view, "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer data) {
                auto &renderer = *static_cast<Renderer *>(data);
                if (!renderer.stopping)
                    renderError(renderer, "web-process-terminated");
            }),
            &renderer);
    renderer.inputSource = g_unix_fd_add(STDIN_FILENO, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                         readInput, &renderer);
    reply(renderer, 0,
          experiment                  ? "READY experiment"
                  : deviceUnsandboxed ? "READY device-unsandboxed"
                                      : "READY sandbox-required");
    g_main_loop_run(renderer.loop);
    if (renderer.inputSource)
        g_source_remove(renderer.inputSource);
    if (renderer.outputSource)
        g_source_remove(renderer.outputSource);
    if (renderer.deadline)
        g_source_remove(renderer.deadline);
    if (renderer.drainSource)
        g_source_remove(renderer.drainSource);
    g_cancellable_cancel(renderer.cancellable);
    g_object_unref(renderer.view);
    g_object_unref(network);
    g_object_unref(context);
    g_object_unref(settings);
    g_object_unref(display);
    for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i)
        g_main_context_iteration(nullptr, FALSE);
    g_object_unref(renderer.cancellable);
    g_main_loop_unref(renderer.loop);
    return renderer.result;
}

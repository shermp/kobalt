#include <wpe/webkit.h>
#include "epub.h"
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
#include <set>
#include <algorithm>
#include <fontconfig/fontconfig.h>

using kobalt::webkit::Epub;
using kobalt::webkit::jsonString;

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
    Epub book;
    bool epub = false;
    unsigned spine = 0, page = 0, pages = 0, anchor = 0;
    int targetPage = 0;
    bool preserveAnchor = false;
    bool pageScriptPending = false;
    unsigned linkSpine = 0, linkPage = 0;
    unsigned fontSize = 24, lineHeight = 150, margin = 24, letterSpacing = 0, wordSpacing = 0;
    std::string family = "publisher", fragment, bridge, metadata;
    std::string elementAnchor = "null", pendingError, sourcePosition, query;
    bool dark = false;
    std::set<unsigned> frames;
};

static void paginate(Renderer &renderer);
static std::string expectedUri(const Renderer &renderer);
static void chapter(Renderer &renderer);
static void query(Renderer &renderer);

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
    if (renderer.queued + line.size() > 1024 * 1024) {
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
    renderer.pageScriptPending = false;
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
    // EPUB frames are immutable until RELEASE; the caller owns its copied pixels.
    // Bound outstanding frames independently of the protocol output queue.
    std::string stem = renderer.runtime + "/frame-" + std::to_string(renderer.request);
    std::string path = renderer.epub ? stem + ".png" : renderer.runtime + "/snapshot.png";
    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    png.width = width;
    png.height = height;
    png.format = PNG_FORMAT_BGRA;
    const bool memory = renderer.epub && g_getenv("KOBALT_WEBKIT_MEMORY_FRAMES");
    bool written =
            memory || png_image_write_to_file(&png, path.c_str(), 0, pixels, stride, nullptr);
    if (!written)
        g_printerr("kobalt-webkit: PNG: %s\n", png.message);
    png_image_free(&png);
    if (written && renderer.epub) {
        written = g_file_set_contents((stem + ".bgra").c_str(), static_cast<const char *>(pixels),
                                      gsize(stride) * height, &error);
        std::string metadata = "{\"version\":1,\"frame\":" + std::to_string(renderer.request)
                + ",\"spine\":" + std::to_string(renderer.spine)
                + ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height)
                + ",\"stride\":" + std::to_string(stride)
                + ",\"format\":\"BGRA8-premultiplied\",\"pixels\":" + jsonString(stem + ".bgra")
                + ",\"png\":" + jsonString(memory ? "" : path) + ",\"document\":"
                + renderer.book.description() + ",\"layout\":" + renderer.metadata + "}\n";
        if (written)
            written = g_file_set_contents((stem + ".json").c_str(), metadata.data(),
                                          metadata.size(), &error);
    }
    g_object_unref(image);
    if (!written) {
        renderError(renderer, "image-write-failed", error);
        g_clear_error(&error);
        return;
    }
    g_source_remove(renderer.deadline);
    renderer.deadline = 0;
    if (renderer.epub) {
        renderer.frames.insert(renderer.request);
        reply(renderer, renderer.request, "OK PAGE " + stem + ".json");
    } else
        reply(renderer, renderer.request,
              "OK SNAPSHOT " + std::to_string(width) + " " + std::to_string(height) + " " + path);
    renderer.request = 0;
}

static void pageReady(GObject *object, GAsyncResult *result, gpointer data)
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
    if (!value || !jsc_value_is_object(value)) {
        g_clear_object(&value);
        renderError(renderer, "pagination-failed", error);
        g_clear_error(&error);
        return;
    }
    JSCValue *source = jsc_value_object_get_property(value, "source");
    char *uri = jsc_value_to_string(source);
    bool current = uri && expectedUri(renderer) == uri;
    g_free(uri);
    g_object_unref(source);
    if (!current) {
        // A superseded about:blank load can finish after OPEN has changed the
        // view URI. Wait for the actual document, never publish its stale DOM.
        g_object_unref(value);
        renderer.pageScriptPending = false;
        if (!webkit_web_view_is_loading(renderer.view))
            paginate(renderer);
        return;
    }
    JSCValue *failure = jsc_value_object_get_property(value, "error");
    char *failureText = jsc_value_is_string(failure) ? jsc_value_to_string(failure) : nullptr;
    const std::string navigationError = failureText ? failureText : "";
    g_free(failureText);
    g_object_unref(failure);
    if (!navigationError.empty()) {
        g_object_unref(value);
        if (!renderer.pendingError.empty()
            || (navigationError != "missing-link-target"
                && navigationError != "invalid-source-position"
                && navigationError != "unresolved-source-position")) {
            renderError(renderer, "navigation-restore-failed");
            return;
        }
        renderer.pendingError = navigationError;
        bool reload = renderer.spine != renderer.linkSpine;
        renderer.spine = renderer.linkSpine;
        renderer.targetPage = renderer.linkPage;
        renderer.fragment.clear();
        renderer.sourcePosition.clear();
        renderer.preserveAnchor = false;
        renderer.pageScriptPending = false;
        if (reload)
            chapter(renderer);
        else
            paginate(renderer);
        return;
    }
    auto integer = [](JSCValue *object, const char *key) {
        JSCValue *property = jsc_value_object_get_property(object, key);
        unsigned result = jsc_value_to_int32(property);
        g_object_unref(property);
        return result;
    };
    renderer.page = integer(value, "page");
    renderer.pages = integer(value, "pages");
    JSCValue *location = jsc_value_object_get_property(value, "location");
    renderer.anchor = integer(location, "start");
    JSCValue *element = jsc_value_object_get_property(location, "element");
    renderer.elementAnchor = "null";
    if (renderer.anchor == integer(location, "end") && jsc_value_is_array(element)) {
        char *path = jsc_value_to_json(element, 0);
        if (path) {
            renderer.elementAnchor = path;
            g_free(path);
        }
    }
    g_object_unref(element);
    g_object_unref(location);
    char *json = jsc_value_to_json(value, 0);
    if (!json || !renderer.pages || renderer.page >= renderer.pages) {
        g_free(json);
        g_object_unref(value);
        renderError(renderer, "invalid-pagination");
        return;
    }
    renderer.metadata = json;
    g_free(json);
    g_object_unref(value);
    if (!renderer.query.empty()) {
        renderer.pageScriptPending = false;
        query(renderer);
        return;
    }
    if (!renderer.pendingError.empty()) {
        g_source_remove(renderer.deadline);
        renderer.deadline = 0;
        reply(renderer, renderer.request, "ERROR " + renderer.pendingError);
        renderer.request = 0;
        renderer.pendingError.clear();
        renderer.pageScriptPending = false;
        return;
    }
    webkit_web_view_get_snapshot(renderer.view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, renderer.cancellable, snapshotReady,
                                 &renderer);
}

static void paginate(Renderer &renderer)
{
    if (renderer.pageScriptPending || renderer.stopping || !renderer.request)
        return;
    renderer.pageScriptPending = true;
    std::string options = "const options = {\"width\":" + std::to_string(renderer.width)
            + ",\"uri\":" + jsonString(expectedUri(renderer)) + ",\"height\":"
            + std::to_string(renderer.height) + ",\"margin\":" + std::to_string(renderer.margin)
            + ",\"fontSize\":" + std::to_string(renderer.fontSize)
            + ",\"lineHeight\":" + std::to_string(renderer.lineHeight)
            + ",\"letterSpacing\":" + std::to_string(renderer.letterSpacing) + ",\"wordSpacing\":"
            + std::to_string(renderer.wordSpacing) + ",\"family\":" + jsonString(renderer.family)
            + ",\"dark\":" + (renderer.dark ? "true" : "false")
            + ",\"page\":" + std::to_string(renderer.targetPage)
            + ",\"anchor\":" + (renderer.preserveAnchor ? std::to_string(renderer.anchor) : "null")
            + ",\"element\":" + (renderer.preserveAnchor ? renderer.elementAnchor : "null")
            + ",\"fragment\":" + jsonString(renderer.fragment) + ",\"cfiBase\":"
            + jsonString(renderer.epub ? renderer.book.spineCfi()[renderer.spine] : "/6/2")
            + ",\"restore\":" + jsonString(renderer.sourcePosition) + "};\n";
    options += "return await globalThis.kobaltPaginate(options);";
    webkit_web_view_call_async_javascript_function(
            renderer.view, options.data(), options.size(), nullptr, "kobalt-reader",
            "kobalt-reader://bridge", renderer.cancellable, pageReady, &renderer);
}

static void epubResource(WebKitURISchemeRequest *request, gpointer data)
{
    auto &renderer = *static_cast<Renderer *>(data);
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    GError *error = nullptr;
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
    std::string path;
    kobalt::webkit::Resource resource;
    bool allowed = parsed && !g_strcmp0(g_uri_get_host(parsed), renderer.book.host().c_str())
            && !g_uri_get_userinfo(parsed) && g_uri_get_port(parsed) == -1
            && !g_uri_get_query(parsed) && g_uri_get_path(parsed)[0] == '/';
    if (allowed)
        path = g_uri_get_path(parsed) + 1;
    std::string bytes;
    if (!allowed || !renderer.epub || !renderer.book.resource(path, resource)
        || !renderer.book.read(path, bytes)) {
        g_clear_error(&error);
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Resource denied");
        webkit_uri_scheme_request_finish_error(request, error);
    } else {
        GBytes *owned = g_bytes_new(bytes.data(), bytes.size());
        GInputStream *stream = g_memory_input_stream_new_from_bytes(owned);
        auto *response = webkit_uri_scheme_response_new(stream, bytes.size());
        webkit_uri_scheme_response_set_content_type(response, resource.mime.c_str());
        SoupMessageHeaders *headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
        // nosniff checks the HTTP header as well as the response's MIME field.
        soup_message_headers_append(headers, "Content-Type", resource.mime.c_str());
        soup_message_headers_append(
                headers, "Content-Security-Policy",
                "default-src 'none'; script-src 'none'; style-src kobalt-epub: 'unsafe-inline'; "
                "img-src kobalt-epub: data:; font-src kobalt-epub: data:; connect-src 'none'; "
                "frame-src 'none'; object-src 'none'; media-src 'none'; "
                "base-uri 'none'; form-action 'none'");
        soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff");
        webkit_uri_scheme_response_set_http_headers(response, headers);
        webkit_uri_scheme_request_finish_with_response(request, response);
        g_object_unref(response);
        g_object_unref(stream);
        g_bytes_unref(owned);
    }
    if (parsed)
        g_uri_unref(parsed);
    g_clear_error(&error);
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

static bool decoded(const std::string &text, std::string &value)
{
    gsize size = 0;
    guchar *bytes = g_base64_decode(text.c_str(), &size);
    gchar *canonical = g_base64_encode(bytes, size);
    bool valid = text == canonical && size && !memchr(bytes, 0, size)
            && g_utf8_validate(reinterpret_cast<char *>(bytes), size, nullptr);
    if (valid)
        value.assign(reinterpret_cast<char *>(bytes), size);
    g_free(bytes);
    g_free(canonical);
    return valid;
}

static void beginRequest(Renderer &renderer, unsigned id)
{
    renderer.request = id;
    renderer.deadline = g_timeout_add_seconds(
            60,
            +[](gpointer data) -> gboolean {
                auto &renderer = *static_cast<Renderer *>(data);
                renderer.deadline = 0;
                renderError(renderer, "render-timeout");
                return G_SOURCE_REMOVE;
            },
            &renderer);
    WPEView *view = webkit_web_view_get_wpe_view(renderer.view);
    wpe_toplevel_resize(wpe_view_get_toplevel(view), renderer.width, renderer.height);
}

static void chapter(Renderer &renderer)
{
    renderer.preserveAnchor = false;
    webkit_web_view_load_uri(renderer.view,
                             renderer.book.uri(renderer.book.spine()[renderer.spine].path).c_str());
}

static std::string expectedUri(const Renderer &renderer)
{
    return renderer.epub ? renderer.book.uri(renderer.book.spine()[renderer.spine].path)
                         : "kobalt-runtime://fixture/document.xhtml";
}

static void query(Renderer &renderer)
{
    const std::string script =
            "return globalThis.kobaltQuery(JSON.parse(" + jsonString(renderer.query) + "));";
    webkit_web_view_call_async_javascript_function(
            renderer.view, script.data(), script.size(), nullptr, "kobalt-reader",
            "kobalt-reader://query", renderer.cancellable,
            +[](GObject *object, GAsyncResult *result, gpointer data) {
                auto &renderer = *static_cast<Renderer *>(data);
                GError *error = nullptr;
                JSCValue *value = webkit_web_view_call_async_javascript_function_finish(
                        WEBKIT_WEB_VIEW(object), result, &error);
                if (!renderer.stopping) {
                    JSCValue *source = value && jsc_value_is_object(value)
                            ? jsc_value_object_get_property(value, "source")
                            : nullptr;
                    char *uri = source && jsc_value_is_string(source) ? jsc_value_to_string(source)
                                                                      : nullptr;
                    char *json = uri && expectedUri(renderer) == uri ? jsc_value_to_json(value, 0)
                                                                     : nullptr;
                    if (json && strlen(json) <= 512 * 1024) {
                        const std::string envelope = "{\"spine\":" + std::to_string(renderer.spine)
                                + ",\"page\":" + std::to_string(renderer.page) + ",\"data\":" + json
                                + "}";
                        char *encoded = g_base64_encode(
                                reinterpret_cast<const guchar *>(envelope.data()), envelope.size());
                        reply(renderer, renderer.request, std::string("OK JSON ") + encoded);
                        g_free(encoded);
                    } else
                        reply(renderer, renderer.request, "ERROR query-failed");
                    g_free(json);
                    g_free(uri);
                    g_clear_object(&source);
                    if (renderer.deadline)
                        g_source_remove(renderer.deadline);
                    renderer.deadline = 0;
                    renderer.request = 0;
                    renderer.query.clear();
                }
                g_clear_object(&value);
                g_clear_error(&error);
            },
            &renderer);
}

static bool epubCommand(Renderer &renderer, unsigned id, const std::string &action,
                        std::istringstream &stream)
{
    const std::set<std::string> commands{ "OPEN",  "NEXT",  "PREVIOUS", "GOTO",   "LINK", "RESIZE",
                                          "STYLE", "CLOSE", "RELEASE",  "LOCATE", "QUERY" };
    if (!commands.count(action))
        return false;
    std::vector<std::string> args;
    for (std::string arg; stream >> arg;)
        args.push_back(std::move(arg));
    auto error = [&](const char *code) { reply(renderer, id, std::string("ERROR ") + code); };
    if (action == "RELEASE") {
        unsigned frame = 0;
        if (args.size() != 1 || !number(args[0], frame) || !renderer.frames.erase(frame))
            error("invalid-frame");
        else {
            auto stem = renderer.runtime + "/frame-" + std::to_string(frame);
            for (auto suffix : { ".json", ".bgra", ".png" })
                unlink((stem + suffix).c_str());
            reply(renderer, id, "OK RELEASED");
        }
        return true;
    }
    if (renderer.request) {
        error("busy");
        return true;
    }
    if (action == "CLOSE") {
        if (!args.empty())
            error("invalid-request");
        else {
            renderer.epub = false;
            renderer.book = {};
            webkit_web_view_load_uri(renderer.view, "about:blank");
            reply(renderer, id, "OK CLOSED");
        }
        return true;
    }
    if (action == "QUERY") {
        unsigned target = 0, page = 0;
        std::string request;
        if (!renderer.epub)
            error("no-book");
        else if (args.size() != 3 || !number(args[0], target) || !number(args[1], page)
                 || target >= renderer.book.spine().size() || page > 100000
                 || !decoded(args[2], request) || request.size() > 16384)
            error("invalid-query");
        else {
            renderer.query = request;
            beginRequest(renderer, id);
            if (target == renderer.spine && page == renderer.page)
                query(renderer);
            else {
                const bool reload = target != renderer.spine;
                renderer.spine = target;
                renderer.targetPage = page;
                renderer.fragment.clear();
                renderer.sourcePosition.clear();
                renderer.preserveAnchor = false;
                if (reload)
                    chapter(renderer);
                else
                    paginate(renderer);
            }
        }
        return true;
    }
    if (renderer.frames.size() >= 2) {
        error("frame-limit");
        return true;
    }
    if (action == "OPEN" || action == "RESIZE") {
        unsigned width = 0, height = 0;
        size_t offset = action == "OPEN" ? 1 : 0;
        if (args.size() != offset + 2 || !number(args[offset], width)
            || !number(args[offset + 1], height) || width < 100 || width > 2048 || height < 100
            || height > 2048 || renderer.margin * 2 + 20 >= std::min(width, height)) {
            error("invalid-viewport");
            return true;
        }
        if (action == "OPEN") {
            std::string path;
            Epub book;
            if (!decoded(args[0], path) || path.front() != '/') {
                error("invalid-path");
                return true;
            }
            if (!book.open(path)) {
                error(book.error().c_str());
                return true;
            }
            renderer.book = std::move(book);
            renderer.epub = true;
            renderer.spine = renderer.page = renderer.pages = renderer.anchor = 0;
            while (!renderer.book.spine()[renderer.spine].linear)
                ++renderer.spine;
        } else if (!renderer.epub) {
            error("no-book");
            return true;
        }
        renderer.width = width;
        renderer.height = height;
        renderer.targetPage = renderer.page;
        renderer.fragment.clear();
        renderer.preserveAnchor = action == "RESIZE";
        beginRequest(renderer, id);
        if (action == "OPEN")
            chapter(renderer);
        else
            paginate(renderer);
        return true;
    }
    if (!renderer.epub) {
        error("no-book");
        return true;
    }
    unsigned target = renderer.spine;
    int page = renderer.page;
    std::string fragment, sourcePosition;
    bool reload = false, preserve = false;
    if (action == "STYLE") {
        unsigned values[5]{};
        bool valid = args.size() == 7;
        for (unsigned i = 0; valid && i < 5; ++i)
            valid &= number(args[i], values[i]);
        if (!valid || values[0] < 8 || values[0] > 96 || values[1] < 100 || values[1] > 300
            || values[2] * uint64_t(2) + 20 >= unsigned(std::min(renderer.width, renderer.height))
            || values[3] > 50 || values[4] > 100
            || (args[5] != "publisher" && args[5] != "serif" && args[5] != "sans-serif"
                && args[5] != "monospace")
            || (args[6] != "light" && args[6] != "dark")) {
            error("invalid-style");
            return true;
        }
        renderer.fontSize = values[0];
        renderer.lineHeight = values[1];
        renderer.margin = values[2];
        renderer.letterSpacing = values[3];
        renderer.wordSpacing = values[4];
        renderer.family = args[5];
        renderer.dark = args[6] == "dark";
        preserve = true;
    } else if (action == "GOTO") {
        unsigned index = 0, selected = 0;
        if (args.size() != 2 || !number(args[0], index) || !number(args[1], selected)
            || index >= renderer.book.spine().size() || selected > 100000) {
            error("invalid-location");
            return true;
        }
        target = index;
        page = selected;
        reload = target != renderer.spine;
    } else if (action == "LOCATE") {
        unsigned index = 0;
        if (args.size() != 2 || !number(args[0], index) || index >= renderer.book.spine().size()
            || !decoded(args[1], sourcePosition) || sourcePosition.size() > 4096) {
            error("invalid-source-position");
            return true;
        }
        target = index;
        page = 0;
        renderer.linkSpine = renderer.spine;
        renderer.linkPage = renderer.page;
        reload = target != renderer.spine;
    } else if (action == "LINK") {
        std::string href;
        if (args.size() != 1 || !decoded(args[0], href)
            || !renderer.book.resolveLink(renderer.spine, href, target, fragment)) {
            error("invalid-link");
            return true;
        }
        renderer.linkSpine = renderer.spine;
        renderer.linkPage = renderer.page;
        page = 0;
        reload = target != renderer.spine;
    } else if (!args.empty()) {
        error("invalid-request");
        return true;
    } else if (action == "NEXT") {
        if (++page >= int(renderer.pages)) {
            do {
                ++target;
            } while (target < renderer.book.spine().size()
                     && !renderer.book.spine()[target].linear);
            if (target >= renderer.book.spine().size()) {
                error("end-of-book");
                return true;
            }
            page = 0;
            reload = true;
        }
    } else if (action == "PREVIOUS") {
        if (--page < 0) {
            while (target && !renderer.book.spine()[target - 1].linear)
                --target;
            if (!target) {
                error("start-of-book");
                return true;
            }
            --target;
            reload = true;
        }
    }
    renderer.spine = target;
    renderer.targetPage = page;
    renderer.fragment = fragment;
    renderer.sourcePosition = sourcePosition;
    renderer.preserveAnchor = preserve;
    beginRequest(renderer, id);
    if (reload)
        chapter(renderer);
    else
        paginate(renderer);
    return true;
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
    if (epubCommand(renderer, id, action, stream))
        return;
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
        renderer.epub = false;
        renderer.width = width;
        renderer.height = height;
        beginRequest(renderer, id);
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
        if (newline > 8192)
            break;
        command(renderer, renderer.input.substr(0, newline));
        renderer.input.erase(0, newline + 1);
    }
    if (renderer.input.size() > 8192 && !renderer.stopping) {
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

static std::string matchedFont(const char *generic)
{
    FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>(generic));
    if (!pattern)
        return {};
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(nullptr, pattern, &result);
    FcChar8 *family = nullptr;
    std::string name;
    if (match && FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch)
        name = reinterpret_cast<char *>(family);
    if (match)
        FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return name;
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
    gchar *bridge = nullptr;
    gsize bridgeSize = 0;
    if (!g_file_get_contents((renderer.bundle + "/share/reader.js").c_str(), &bridge, &bridgeSize,
                             nullptr))
        return 1;
    renderer.bridge.assign(bridge, bridgeSize);
    g_free(bridge);
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
    webkit_settings_set_enable_javascript_markup(settings, FALSE);
    // Firmware Fontconfig can supply a fallback without a strongly bound generic
    // alias. Give Skia the resolved families so primary line metrics stay valid.
    auto serif = matchedFont("serif"), sans = matchedFont("sans-serif"),
         mono = matchedFont("monospace");
    if (serif.empty() || sans.empty() || mono.empty()) {
        g_printerr("kobalt-webkit: default reading fonts are unavailable\n");
        return 1;
    }
    webkit_settings_set_default_font_family(settings, serif.c_str());
    webkit_settings_set_serif_font_family(settings, serif.c_str());
    webkit_settings_set_sans_serif_font_family(settings, sans.c_str());
    webkit_settings_set_monospace_font_family(settings, mono.c_str());
    auto *context = webkit_web_context_new();
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    webkit_web_context_add_path_to_sandbox(context, bundle, TRUE);
    webkit_web_context_register_uri_scheme(context, "kobalt-runtime", resource, &renderer, nullptr);
    webkit_web_context_register_uri_scheme(context, "kobalt-epub", epubResource, &renderer,
                                           nullptr);
    auto *security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_local(security, "kobalt-runtime");
    webkit_security_manager_register_uri_scheme_as_secure(security, "kobalt-runtime");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "kobalt-runtime");
    webkit_security_manager_register_uri_scheme_as_secure(security, "kobalt-epub");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "kobalt-epub");
    auto *network = webkit_network_session_new_ephemeral();
    auto *content = webkit_user_content_manager_new();
    // Retain the named world for the document's lifetime. Ad-hoc evaluations
    // alone do not retain its globals, although their DOM mutations survive.
    std::string script =
            "globalThis.kobaltPaginate = async function(options) {\n" + renderer.bridge + "\n};";
    WebKitUserScript *bridgeScript = webkit_user_script_new_for_world(
            script.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, "kobalt-reader", nullptr, nullptr);
    webkit_user_content_manager_add_script(content, bridgeScript);
    webkit_user_script_unref(bridgeScript);
    renderer.view = WEBKIT_WEB_VIEW(g_object_new(
            WEBKIT_TYPE_WEB_VIEW, "display", display, "settings", settings, "web-context", context,
            "user-content-manager", content, "network-session", network, nullptr));
    g_signal_connect(
            renderer.view, "decide-policy",
            G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *decision,
                           WebKitPolicyDecisionType type, gpointer data) -> gboolean {
                auto &renderer = *static_cast<Renderer *>(data);
                if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION
                    || type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
                    auto *navigation = webkit_navigation_policy_decision_get_navigation_action(
                            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
                    const char *uri = webkit_uri_request_get_uri(
                            webkit_navigation_action_get_request(navigation));
                    std::string expected = expectedUri(renderer);
                    bool allowed = type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION
                            && ((!renderer.request && !g_strcmp0(uri, "about:blank"))
                                || (renderer.request && expected == uri
                                    && webkit_navigation_action_get_navigation_type(navigation)
                                            == WEBKIT_NAVIGATION_TYPE_OTHER));
                    if (allowed)
                        webkit_policy_decision_use(decision);
                    else
                        webkit_policy_decision_ignore(decision);
                    return TRUE;
                }
                return FALSE;
            }),
            &renderer);
    g_signal_connect(renderer.view, "permission-request",
                     G_CALLBACK(+[](WebKitWebView *, WebKitPermissionRequest *request,
                                    gpointer) -> gboolean {
                         webkit_permission_request_deny(request);
                         return TRUE;
                     }),
                     nullptr);
    g_signal_connect(
            renderer.view, "load-changed",
            G_CALLBACK(+[](WebKitWebView *view, WebKitLoadEvent event, gpointer data) {
                auto &renderer = *static_cast<Renderer *>(data);
                if (event != WEBKIT_LOAD_FINISHED || renderer.stopping || !renderer.request)
                    return;
                if (g_strcmp0(webkit_web_view_get_uri(view), expectedUri(renderer).c_str()))
                    return;
                if (renderer.epub) {
                    paginate(renderer);
                    return;
                }
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
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, gchar *uri, GError *error,
                                    gpointer data) -> gboolean {
                         auto &renderer = *static_cast<Renderer *>(data);
                         if (!renderer.stopping && renderer.request && expectedUri(renderer) == uri
                             && !g_error_matches(error, WEBKIT_NETWORK_ERROR,
                                                 WEBKIT_NETWORK_ERROR_CANCELLED))
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
    g_object_unref(content);
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

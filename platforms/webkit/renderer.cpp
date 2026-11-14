#include "renderer-frame.h"
#include "renderer1-xml.h"

#include <wpe/headless/wpe-headless.h>

#include <cstring>
#include <limits>
#include <memory>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr char objectPath[] = "/org/kobalt/Renderer1";
constexpr char errorPrefix[] = "org.kobalt.Renderer1.Error.";
constexpr const char *tokens[] = { "fixture:color-pages:0", "fixture:color-pages:1" };

void error(GDBusMethodInvocation *call, const char *name, const char *message)
{
    g_autofree gchar *qualified = g_strconcat(errorPrefix, name, nullptr);
    g_dbus_method_invocation_return_dbus_error(call, qualified, message);
}

struct State
{
    guint64 document = 0, layout = 0, frame = 0;
    guint width = 0, height = 0, page = 0;
};

GVariant *describe(const State &state)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "sourceToken", g_variant_new_string(tokens[state.page]));
    g_variant_builder_add(&builder, "{sv}", "locationPrecision", g_variant_new_string("exact"));
    g_variant_builder_add(&builder, "{sv}", "canTurnBack", g_variant_new_boolean(state.page == 1));
    g_variant_builder_add(&builder, "{sv}", "canTurnForward", g_variant_new_boolean(state.page == 0));
    g_variant_builder_add(&builder, "{sv}", "pageIndex", g_variant_new_uint32(state.page));
    g_variant_builder_add(&builder, "{sv}", "pageCount", g_variant_new_uint32(2));
    return g_variant_builder_end(&builder);
}

int pageForToken(const char *token)
{
    for (int page = 0; page < 2; ++page) {
        if (g_str_equal(token, tokens[page]))
            return page;
    }
    return -1;
}

struct Render
{
    Render(guint64 id, State next, GDBusMethodInvocation *invocation)
        : request(id), candidate(next), call(invocation)
    {
    }

    ~Render()
    {
        if (deadline)
            g_source_remove(deadline);
        g_clear_object(&view);
        g_object_unref(cancel);
        g_clear_object(&call);
    }

    guint64 request;
    State candidate;
    GDBusMethodInvocation *call;
    GCancellable *cancel = g_cancellable_new();
    WebKitWebView *view = nullptr;
    guint deadline = 0;
    bool snapshotPending = false;
    const char *failure = nullptr;
};

class Service
{
public:
    explicit Service(const char *directory) : runDirectory(directory) {}
    ~Service()
    {
        g_clear_object(&shutdownCall);
        g_clear_object(&connection);
        g_main_loop_unref(loop);
    }

    int run();

private:
    void call(const char *, GVariant *, GDBusMethodInvocation *);
    void startRender(guint64, State, GDBusMethodInvocation *);
    void snapshotReady(GAsyncResult *);
    void abortRender(const char *);
    void finishRender(GUnixFDList *);
    void finishShutdown();
    void accept(GSocketConnection *);

    const char *runDirectory;
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    GSocketService *listener = nullptr;
    GDBusNodeInfo *schema = nullptr;
    GDBusConnection *connection = nullptr;
    GDBusMethodInvocation *shutdownCall = nullptr;
    State current;
    guint64 lastRequest = 0, lastDocument = 0;
    bool greeted = false, stopping = false, disconnected = false, flushing = false;
    int exitCode = 0;
    std::unique_ptr<Render> active;
};

void Service::call(const char *method, GVariant *args, GDBusMethodInvocation *invocation)
{
    auto is = [method](const char *name) { return g_str_equal(method, name); };
    auto fail = [invocation](const char *name, const char *message) {
        error(invocation, name, message);
    };
    if (stopping)
        return fail("ShuttingDown", "Renderer is closing");
    if (is("Hello")) {
        guint version;
        g_variant_get(args, "(u)", &version);
        if (version != 1)
            return fail("UnsupportedVersion", "Renderer1 requires protocol version 1");
        if (!(g_dbus_connection_get_capabilities(connection)
              & G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING))
            return fail("UnixFdUnavailable", "Peer did not negotiate Unix FD passing");
        greeted = true;
        GVariantBuilder capabilities;
        g_variant_builder_init(&capabilities, G_VARIANT_TYPE_VARDICT);
        const char *sources[] = { "fixture", nullptr };
        g_variant_builder_add(&capabilities, "{sv}", "sourceKinds", g_variant_new_strv(sources, -1));
        g_variant_builder_add(&capabilities, "{sv}", "maxWidth", g_variant_new_uint32(maxDimension));
        g_variant_builder_add(&capabilities, "{sv}", "maxHeight", g_variant_new_uint32(maxDimension));
        g_variant_builder_add(&capabilities, "{sv}", "maxFrameBytes",
                              g_variant_new_uint32(maxFrameBytes));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 1u, &capabilities));
        return;
    }
    if (!greeted)
        return fail("NotReady", "Call Hello first");
    if (is("Cancel")) {
        guint64 target;
        g_variant_get(args, "(t)", &target);
        const bool accepted = active && active->request == target && !active->failure;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", accepted));
        if (accepted)
            abortRender("Cancelled");
        return;
    }
    if (is("Shutdown")) {
        stopping = true;
        shutdownCall = invocation;
        if (active)
            abortRender("Cancelled");
        else
            finishShutdown();
        return;
    }

    guint64 request;
    g_variant_get_child(args, 0, "t", &request);
    if (request <= lastRequest)
        return fail("InvalidArgument", "Request ID must increase");
    lastRequest = request;
    if (active)
        return fail("Busy", "A foreground request is active");

    State next = current;
    if (is("Open")) {
        const gchar *kind, *source, *fingerprint, *token;
        g_autoptr(GVariant) appearance = nullptr;
        g_variant_get(args, "(t&s&s&suu@a{sv}&s)", &request, &kind, &source, &fingerprint,
                      &next.width, &next.height, &appearance, &token);
        if (!g_str_equal(kind, "fixture") || g_variant_n_children(appearance))
            return fail("Unsupported", "Only the built-in fixture with default appearance is supported");
        if (!g_str_equal(source, "color-pages"))
            return fail("SourceError", "Unknown fixture");
        if (*fingerprint || !validViewport(next.width, next.height))
            return fail("InvalidArgument", "Invalid fixture fingerprint or viewport");
        const int page = *token ? pageForToken(token) : 0;
        if (page < 0)
            return fail("UnresolvedLocation", "Unknown fixture token");
        if (lastDocument == std::numeric_limits<guint64>::max())
            return fail("RenderFailed", "Reconnect to allocate another document");
        next.document = lastDocument + 1;
        next.layout = next.frame = 1;
        next.page = page;
        return startRender(request, next, invocation);
    }

    guint64 document;
    g_variant_get_child(args, 1, "t", &document);
    if (!current.document || document != current.document)
        return fail("StaleState", "Document is no longer current");
    if (is("Close")) {
        current = {};
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    guint64 base;
    g_variant_get_child(args, 2, "t", &base);
    if (base != current.frame)
        return fail("StaleState", "Frame is no longer current");
    if (is("Query")) {
        const gchar *kind;
        g_autoptr(GVariant) arguments = nullptr;
        g_variant_get_child(args, 3, "&s", &kind);
        g_variant_get_child(args, 4, "@a{sv}", &arguments);
        if (!g_str_equal(kind, "state") || g_variant_n_children(arguments))
            return fail("Unsupported", "Only the state query with empty arguments is supported");
        g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(tttt@a{sv})", request, document, current.layout,
                                          current.frame, describe(current)));
        return;
    }
    if (next.frame == std::numeric_limits<guint64>::max())
        return fail("RenderFailed", "Reopen before advancing the frame");
    ++next.frame;
    if (is("Turn")) {
        gint delta;
        g_variant_get_child(args, 3, "i", &delta);
        if (delta != -1 && delta != 1)
            return fail("InvalidArgument", "Turn must be one page");
        const int page = int(next.page) + delta;
        if (page < 0 || page > 1)
            return fail("OutOfRange", "Reached the fixture boundary");
        next.page = page;
    } else if (is("Navigate")) {
        const gchar *kind, *target;
        g_variant_get_child(args, 3, "&s", &kind);
        g_variant_get_child(args, 4, "&s", &target);
        if (g_str_equal(kind, "token")) {
            const int page = pageForToken(target);
            if (page < 0)
                return fail("UnresolvedLocation", "Unknown fixture token");
            next.page = page;
        } else if (g_str_equal(kind, "start") || g_str_equal(kind, "end")) {
            if (*target)
                return fail("InvalidArgument", "Boundary navigation requires an empty target");
            next.page = g_str_equal(kind, "end");
        } else {
            return fail("Unsupported", "Unknown navigation kind");
        }
    } else if (is("SetViewport")) {
        g_variant_get_child(args, 3, "u", &next.width);
        g_variant_get_child(args, 4, "u", &next.height);
        if (!validViewport(next.width, next.height))
            return fail("InvalidArgument", "Viewport exceeds the frame limits");
        if (next.width != current.width || next.height != current.height) {
            if (next.layout == std::numeric_limits<guint64>::max())
                return fail("RenderFailed", "Reopen before changing the layout");
            ++next.layout;
        }
    } else if (is("SetAppearance")) {
        g_autoptr(GVariant) appearance = nullptr;
        g_variant_get_child(args, 3, "@a{sv}", &appearance);
        if (g_variant_n_children(appearance))
            return fail("Unsupported", "The fixture supports only default appearance");
    }
    startRender(request, next, invocation);
}

void Service::startRender(guint64 request, State next, GDBusMethodInvocation *invocation)
{
    active = std::make_unique<Render>(request, next, invocation);
    g_autoptr(WebKitSettings) settings = webkit_settings_new();
    const char *required[] = { "HardwareAcceleration", "AcceleratedCompositing",
                               "ForceCompositingMode" };
    auto *features = webkit_settings_get_all_features();
    unsigned disabled = 0;
    for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
        auto *feature = webkit_feature_list_get(features, i);
        for (const char *id : required) {
            if (g_str_equal(webkit_feature_get_identifier(feature), id)) {
                webkit_settings_set_feature_enabled(settings, feature, FALSE);
                disabled += !webkit_settings_get_feature_enabled(settings, feature);
            }
        }
    }
    webkit_feature_list_unref(features);
    if (disabled != G_N_ELEMENTS(required))
        return abortRender("RenderFailed");

    // Each fixture operation uses its own view. A failed or cancelled load cannot
    // mutate the current document. M4 will replace this with retained spine layout.
    auto *display = wpe_display_headless_new();
    g_autoptr(WebKitNetworkSession) network = webkit_network_session_new_ephemeral();
    active->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
                                                "settings", settings, "network-session", network,
                                                nullptr));
    g_object_unref(display);
    auto *view = active->view;
    wpe_toplevel_resize(wpe_view_get_toplevel(webkit_web_view_get_wpe_view(view)), next.width,
                        next.height);
    g_signal_connect(view, "load-changed", G_CALLBACK(+[](WebKitWebView *view, WebKitLoadEvent event,
                                                        gpointer data) {
        auto &service = *static_cast<Service *>(data);
        if (event != WEBKIT_LOAD_FINISHED || !service.active || service.active->view != view
            || service.active->snapshotPending)
            return;
        service.active->snapshotPending = true;
        webkit_web_view_get_snapshot(
                view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                service.active->cancel,
                +[](GObject *, GAsyncResult *result, gpointer data) {
                    static_cast<Service *>(data)->snapshotReady(result);
                },
                data);
    }), this);
    g_signal_connect(view, "load-failed", G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent,
                                                       const gchar *, GError *, gpointer data) {
        static_cast<Service *>(data)->abortRender("RenderFailed");
        return TRUE;
    }), this);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(+[](WebKitWebView *,
                                                    WebKitWebProcessTerminationReason, gpointer data) {
        static_cast<Service *>(data)->abortRender("RenderFailed");
    }), this);
    active->deadline = g_timeout_add_seconds(30, +[](gpointer data) -> gboolean {
        auto &service = *static_cast<Service *>(data);
        service.active->deadline = 0;
        service.abortRender("RenderFailed");
        return G_SOURCE_REMOVE;
    }, this);
    const char *html = next.page
            ? "<!doctype html><style>html{background:blue}body{margin:0}</style>"
              "<div style='width:32px;height:32px;background:white'></div>"
            : "<!doctype html><style>html{background:red}body{margin:0}</style>"
              "<div style='width:32px;height:32px;background:white'></div>";
    webkit_web_view_load_html(view, html, nullptr);
}

void Service::snapshotReady(GAsyncResult *result)
{
    g_autoptr(WebKitImage) image = webkit_web_view_get_snapshot_finish(active->view, result, nullptr);
    if (!image || active->failure)
        return finishRender(nullptr);

    // Extract the pixel view on the GLib thread, then copy it on a worker.
    const auto &next = active->candidate;
    auto *pixels = fixturePixels(image, next.width, next.height, next.page);
    if (!pixels)
        return finishRender(nullptr);
    pixels->directory = runDirectory;
    g_autoptr(GTask) task = g_task_new(nullptr, active->cancel,
                                     +[](GObject *, GAsyncResult *result, gpointer data) {
        g_autoptr(GUnixFDList) descriptors = static_cast<GUnixFDList *>(
                g_task_propagate_pointer(G_TASK(result), nullptr));
        static_cast<Service *>(data)->finishRender(descriptors);
    }, this);
    g_task_set_task_data(task, pixels, +[](gpointer data) {
        delete static_cast<FixturePixels *>(data);
    });
    g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancel) {
        auto *descriptors = makeFixtureFrame(*static_cast<FixturePixels *>(data), cancel);
        g_task_return_pointer(task, descriptors, descriptors ? g_object_unref : nullptr);
    });
}

void Service::abortRender(const char *reason)
{
    if (!active->failure)
        active->failure = reason;
    if (active->snapshotPending) {
        g_cancellable_cancel(active->cancel);
        return;
    }
    finishRender(nullptr);
}

void Service::finishRender(GUnixFDList *descriptors)
{
    auto render = std::move(active);
    if (render->view)
        g_signal_handlers_disconnect_by_data(render->view, this);
    if (!disconnected) {
        if (render->failure || !descriptors) {
            error(g_steal_pointer(&render->call), render->failure ? render->failure : "RenderFailed",
                  "Fixture snapshot did not commit");
        } else {
            current = render->candidate;
            lastDocument = current.document;
            g_dbus_method_invocation_return_value_with_unix_fd_list(
                    g_steal_pointer(&render->call),
                    g_variant_new("(tttt@a{sv}huuus)", render->request, current.document,
                                  current.layout, current.frame, describe(current), 0, current.width,
                                  current.height, current.width * 4, frameFormat),
                    descriptors);
        }
    }
    render.reset();
    if (stopping)
        finishShutdown();
}

void Service::finishShutdown()
{
    current = {};
    if (disconnected) {
        g_main_loop_quit(loop);
        return;
    }
    g_dbus_method_invocation_return_value(g_steal_pointer(&shutdownCall), nullptr);
    flushing = true;
    g_dbus_connection_flush(connection, nullptr, +[](GObject *source, GAsyncResult *result,
                                                     gpointer data) {
        auto &service = *static_cast<Service *>(data);
        if (!g_dbus_connection_flush_finish(G_DBUS_CONNECTION(source), result, nullptr))
            service.exitCode = 1;
        g_main_loop_quit(service.loop);
    }, this);
}

void Service::accept(GSocketConnection *socket)
{
    g_socket_service_stop(listener);
    g_autoptr(GDBusAuthObserver) observer = g_dbus_auth_observer_new();
    g_signal_connect(observer, "allow-mechanism", G_CALLBACK(+[](GDBusAuthObserver *,
                                                               const gchar *mechanism, gpointer) {
        return gboolean(g_str_equal(mechanism, "EXTERNAL"));
    }), nullptr);
    g_signal_connect(observer, "authorize-authenticated-peer",
                     G_CALLBACK(+[](GDBusAuthObserver *, GIOStream *, GCredentials *credentials,
                                    gpointer) {
        return gboolean(credentials && g_credentials_get_unix_user(credentials, nullptr) == getuid());
    }), nullptr);
    g_autofree gchar *guid = g_dbus_generate_guid();
    g_dbus_connection_new(G_IO_STREAM(socket), guid,
                          GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER
                                               | G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING),
                          observer, nullptr,
                          +[](GObject *, GAsyncResult *result, gpointer data) {
        auto &service = *static_cast<Service *>(data);
        g_autoptr(GError) failure = nullptr;
        service.connection = g_dbus_connection_new_finish(result, &failure);
        static const GDBusInterfaceVTable vtable = {
            +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                GVariant *args, GDBusMethodInvocation *call, gpointer data) {
                static_cast<Service *>(data)->call(method, args, call);
            },
            nullptr, nullptr, { nullptr }
        };
        if (!service.connection
            || !g_dbus_connection_register_object(service.connection, objectPath,
                                                   service.schema->interfaces[0], &vtable,
                                                   &service, nullptr, &failure)) {
            g_printerr("Peer setup failed: %s\n", failure->message);
            service.exitCode = 1;
            g_main_loop_quit(service.loop);
            return;
        }
        g_signal_connect(service.connection, "closed", G_CALLBACK(+[](GDBusConnection *, gboolean,
                                                                     GError *, gpointer data) {
            auto &service = *static_cast<Service *>(data);
            service.disconnected = service.stopping = true;
            if (service.active)
                service.abortRender("Cancelled");
            else if (!service.flushing)
                g_main_loop_quit(service.loop);
        }), &service);
        g_dbus_connection_start_message_processing(service.connection);
    }, this);
}

int Service::run()
{
    struct stat directory {};
    if (runDirectory[0] != '/' || lstat(runDirectory, &directory) || !S_ISDIR(directory.st_mode)
        || (directory.st_mode & 0777) != 0700 || directory.st_uid != getuid()) {
        g_printerr("Expected an absolute, owner-only run directory\n");
        return 1;
    }
    g_autoptr(GError) failure = nullptr;
    g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(rendererXml, &failure);
    if (!info) {
        g_printerr("%s\n", failure->message);
        return 1;
    }
    schema = info;
    g_autofree gchar *path = g_build_filename(runDirectory, "renderer.sock", nullptr);
    if (strlen(path) >= sizeof(sockaddr_un::sun_path)) {
        g_printerr("Run directory path is too long for a Unix socket\n");
        return 1;
    }
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
    g_autoptr(GSocketService) socketService = g_socket_service_new();
    listener = socketService;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(listener), address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, nullptr, nullptr, &failure)) {
        g_printerr("%s\n", failure->message);
        return 1;
    }
    g_signal_connect(listener, "incoming", G_CALLBACK(+[](GSocketService *, GSocketConnection *socket,
                                                        GObject *, gpointer data) {
        static_cast<Service *>(data)->accept(socket);
        return TRUE;
    }), this);
    g_autofree gchar *escaped = g_dbus_address_escape_value(path);
    g_print("unix:path=%s\n", escaped);
    g_main_loop_run(loop);
    if (connection && !g_dbus_connection_is_closed(connection)) {
        g_signal_handlers_disconnect_by_data(connection, this);
        g_dbus_connection_close_sync(connection, nullptr, nullptr);
    }
    g_socket_listener_close(G_SOCKET_LISTENER(listener));
    unlink(path);
    return exitCode;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        g_printerr("usage: %s /absolute/private-run-directory\n", argv[0]);
        return 2;
    }
    return Service(argv[1]).run();
}

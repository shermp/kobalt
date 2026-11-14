#include "webkit-render.h"
#include "../renderer-frame.h"

#include <wpe/headless/wpe-headless.h>

#include <cstring>
#include <limits>

using namespace spike;

struct WebKitRender::Job
{
    ~Job()
    {
        if (deadline)
            g_source_remove(deadline);
        g_clear_object(&view);
        g_object_unref(cancellable);
    }
    uint64_t id = 0;
    Revision revision;
    FrameSpec frame;
    WebKitWebView *view = nullptr;
    GCancellable *cancellable = g_cancellable_new();
    guint deadline = 0;
    bool outstanding = false, cancelled = false;
};

WebKitRender::WebKitRender(std::array<unsigned char *, 2> maps, uint32_t capacity, Reply reply)
    : maps(maps), capacity(capacity), reply(std::move(reply))
{
}

WebKitRender::~WebKitRender()
{
    g_clear_object(&committedView);
}

void WebKitRender::reject(uint64_t id, RenderError code)
{
    std::string payload;
    put(payload, uint32_t(code), 4);
    reply(ErrorReply, id, payload);
}

bool WebKitRender::receive(const Message &msg)
{
    if (shutdownId)
        return false;
    if (!generation) {
        if (msg.type != PoolHello || msg.payload.size() != 12
            || read(msg.payload.data() + 8, 4) != capacity)
            return false;
        generation = read(msg.payload.data(), 8);
        if (!generation)
            return false;
        reply(Ready, msg.id, msg.payload);
        return true;
    }
    if (msg.type == Cancel) {
        if (msg.payload.size() != 8)
            return false;
        const bool accepted = active && !active->cancelled
                && read(msg.payload.data(), 8) == active->id;
        reply(CancelAck, msg.id, std::string(1, accepted ? '\1' : '\0'));
        if (accepted)
            cancel();
        return true;
    }
    if (msg.type == Shutdown) {
        if (!msg.payload.empty())
            return false;
        shutdownId = msg.id;
        if (active)
            cancel();
        else
            reply(Bye, shutdownId, {});
        return true;
    }
    if (active) {
        reject(msg.id, RenderError::Busy);
        return true;
    }
    if (msg.type == Query || msg.type == Close) {
        if (msg.payload.size() != 24)
            return false;
        if (!current.document || !(Revision::decode(msg.payload.data()) == current))
            reject(msg.id, RenderError::Stale);
        else if (msg.type == Query)
            reply(StateReply, msg.id, stateReply(current, frame));
        else {
            current = {};
            g_clear_object(&committedView);
            reply(Closed, msg.id, {});
        }
        return true;
    }
    if (msg.type < Open || msg.type > SetAppearance)
        return false;
    start(msg);
    return true;
}

void WebKitRender::start(const Message &msg)
{
    FrameSpec next;
    if (msg.payload.size() != 48 || !FrameSpec::decode(msg.payload, next)
        || next.generation != generation || !validGeometry(next.width, next.height, capacity))
        return reject(msg.id, RenderError::Invalid);
    const auto base = Revision::decode(msg.payload.data() + 24);
    Revision revision;
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    if (msg.type == Open) {
        if (!(base == Revision{}) || documentSequence == max)
            return reject(msg.id, RenderError::Invalid);
        revision = {documentSequence + 1, 1, 1};
    } else {
        if (!current.document || !(base == current))
            return reject(msg.id, RenderError::Stale);
        const bool resize = next.width != frame.width || next.height != frame.height;
        if ((resize && msg.type != SetViewport)
            || ((msg.type == SetViewport || msg.type == SetAppearance) && next.color != frame.color))
            return reject(msg.id, RenderError::Invalid);
        if (msg.type == Turn && next.color == frame.color)
            return reject(msg.id, RenderError::Boundary);
        if (current.frame == max || (resize && current.layout == max))
            return reject(msg.id, RenderError::Failed);
        revision = {current.document, current.layout + resize, current.frame + 1};
    }

    active = std::make_unique<Job>();
    active->id = msg.id;
    active->revision = revision;
    active->frame = next;
    g_autoptr(WebKitSettings) settings = webkit_settings_new();
    auto *features = webkit_settings_get_all_features();
    unsigned disabled = 0;
    for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
        auto *feature = webkit_feature_list_get(features, i);
        const auto *id = webkit_feature_get_identifier(feature);
        if (g_str_equal(id, "HardwareAcceleration") || g_str_equal(id, "AcceleratedCompositing")
            || g_str_equal(id, "ForceCompositingMode")) {
            webkit_settings_set_feature_enabled(settings, feature, FALSE);
            disabled += !webkit_settings_get_feature_enabled(settings, feature);
        }
    }
    webkit_feature_list_unref(features);
    if (disabled != 3)
        return finish(false);
    auto *display = wpe_display_headless_new();
    g_autoptr(WebKitNetworkSession) network = webkit_network_session_new_ephemeral();
    auto *view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "settings",
                                              settings, "network-session", network, nullptr));
    g_object_unref(display);
    active->view = view;
    wpe_toplevel_resize(wpe_view_get_toplevel(webkit_web_view_get_wpe_view(view)), next.width,
                        next.height);
    g_signal_connect(view, "load-changed",
                     G_CALLBACK(+[](WebKitWebView *sender, WebKitLoadEvent event, gpointer data) {
        auto &self = *static_cast<WebKitRender *>(data);
        if (event != WEBKIT_LOAD_FINISHED || !self.active || self.active->view != sender
            || self.active->outstanding)
            return;
        self.active->outstanding = true;
        webkit_web_view_get_snapshot(sender, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                     WEBKIT_SNAPSHOT_OPTIONS_NONE, self.active->cancellable,
                                     +[](GObject *, GAsyncResult *result, gpointer data) {
            static_cast<WebKitRender *>(data)->snapshot(result);
        }, data);
    }), this);
    g_signal_connect(view, "load-failed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, const char *, GError *,
                                    gpointer data) -> gboolean {
        auto &self = *static_cast<WebKitRender *>(data);
        if (!self.active->outstanding)
            self.finish(false);
        else
            g_cancellable_cancel(self.active->cancellable);
        return TRUE;
    }), this);
    g_signal_connect(view, "web-process-terminated",
                     G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason,
                                    gpointer data) {
        auto &self = *static_cast<WebKitRender *>(data);
        if (!self.active->outstanding)
            self.finish(false);
        else
            g_cancellable_cancel(self.active->cancellable);
    }), this);
    active->deadline = g_timeout_add_seconds(10, +[](gpointer data) -> gboolean {
        auto &self = *static_cast<WebKitRender *>(data);
        self.active->deadline = 0;
        if (!self.active->outstanding)
            self.finish(false);
        else
            g_cancellable_cancel(self.active->cancellable);
        return G_SOURCE_REMOVE;
    }, this);
    constexpr const char *pages[] = {
        "<!doctype html><style>html{background:red}body{margin:0}</style>"
        "<div style='width:32px;height:32px;background:white'></div>",
        "<!doctype html><style>html{background:blue}body{margin:0}</style>"
        "<div style='width:32px;height:32px;background:white'></div>",
    };
    webkit_web_view_load_html(view, pages[next.color], nullptr);
}

void WebKitRender::snapshot(GAsyncResult *result)
{
    g_autoptr(WebKitImage) image = webkit_web_view_get_snapshot_finish(active->view, result, nullptr);
    const auto spec = active->frame;
    auto *pixels = image && !g_cancellable_is_cancelled(active->cancellable)
            ? fixturePixels(image, spec.width, spec.height, spec.color) : nullptr;
    if (!pixels)
        return finish(false);

    struct Copy { std::unique_ptr<FixturePixels> pixels; unsigned char *map; };
    g_autoptr(GTask) task = g_task_new(nullptr, active->cancellable,
                                     +[](GObject *, GAsyncResult *result, gpointer data) {
        static_cast<WebKitRender *>(data)->finish(g_task_propagate_boolean(G_TASK(result), nullptr));
    }, this);
    g_task_set_task_data(task, new Copy{std::unique_ptr<FixturePixels>(pixels), maps[spec.slot]},
                        +[](gpointer data) { delete static_cast<Copy *>(data); });
    g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancel) {
        const auto &copy = *static_cast<Copy *>(data);
        const auto &frame = *copy.pixels;
        if (!validateFixturePixels(frame, cancel))
            return g_task_return_boolean(task, FALSE);
        const auto *src = static_cast<const unsigned char *>(g_bytes_get_data(frame.bytes, nullptr));
        for (guint row = 0; row < frame.height; ++row) {
            if (g_cancellable_is_cancelled(cancel))
                return g_task_return_boolean(task, FALSE);
            std::memcpy(copy.map + size_t(row) * frame.width * 4, src + size_t(row) * frame.stride,
                        frame.width * 4);
        }
        g_task_return_boolean(task, TRUE);
    });
}

void WebKitRender::cancel()
{
    active->cancelled = true;
    g_cancellable_cancel(active->cancellable);
    if (!active->outstanding)
        finish(false);
}

void WebKitRender::finish(bool success)
{
    auto job = std::move(active);
    if (job->view)
        g_signal_handlers_disconnect_by_data(job->view, this);
    if (drained) {
        job.reset();
        drained();
        return;
    }
    if (job->cancelled) {
        reject(job->id, RenderError::Cancelled);
    } else if (!success) {
        reject(job->id, RenderError::Failed);
    } else {
        current = job->revision;
        documentSequence = current.document;
        frame = job->frame;
        g_clear_object(&committedView);
        committedView = G_OBJECT(g_steal_pointer(&job->view));
        reply(FrameReady, job->id, frame.reply(frame.width * 4 * frame.height) + current.encode());
    }
    if (shutdownId)
        reply(Bye, shutdownId, {});
}

void WebKitRender::disconnect(Drain done)
{
    drained = std::move(done);
    if (active)
        cancel();
    else
        drained();
}

#include "view.h"

#include <wpe/headless/wpe-headless.h>

#include <utility>

namespace retained {
namespace {
constexpr char world[] = "kobalt-retained-dom-v1";
constexpr char layoutScript[] = R"JS(
    // Page-authored scripts are disabled. This bridge only measures and moves
    // the current document inside its own fixed viewport.
    if (globalThis.retainedDocument !== document) {
        globalThis.retainedDocument = document;
        globalThis.retainedOperations = 0;
    }
    ++globalThis.retainedOperations;
    const tick = () => new Promise(resolve => requestAnimationFrame(resolve));
    const end = performance.now() + 3000;
    while (innerWidth !== width || innerHeight !== height) {
        if (performance.now() >= end)
            throw new Error('Viewport did not reach the requested dimensions');
        await tick();
    }
    document.documentElement.style.setProperty('--reader-font-size', fontSize + 'px');
    document.body.getBoundingClientRect();
    const faces = await document.fonts.load(fontSize + 'px ReaderFixture');
    await document.fonts.ready;
    await Promise.all(Array.from(document.images, image => image.decode()));
    if (!faces.length)
        throw new Error('Expected fixture font did not load');
    const columns = Math.max(1, Math.ceil(document.documentElement.scrollWidth / width));
    if (column >= columns)
        throw new Error('Column is out of range');
    window.scrollTo(column * width, 0);
    await tick();
    await tick();
    if (window.scrollX !== column * width || document.documentElement.hasAttribute('data-book-script'))
        throw new Error('Scroll position or script policy mismatch');
    return globalThis.retainedOperations;
)JS";

bool software(WebKitSettings *settings)
{
    unsigned disabled = 0;
    auto *features = webkit_settings_get_all_features();
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
    return disabled == 3;
}
} // namespace

View::View(WebKitWebContext *context, const char *style)
{
    g_autoptr(WebKitSettings) settings = webkit_settings_new();
    healthy = software(settings);
    webkit_settings_set_enable_javascript_markup(settings, FALSE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    g_autoptr(WebKitNetworkSession) network = webkit_network_session_new_ephemeral();
    g_autoptr(WebKitUserContentManager) content = webkit_user_content_manager_new();
    auto *sheet =
            webkit_user_style_sheet_new(style, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                       WEBKIT_USER_STYLE_LEVEL_USER, nullptr, nullptr);
    webkit_user_content_manager_add_style_sheet(content, sheet);
    webkit_user_style_sheet_unref(sheet);
    auto *display = wpe_display_headless_new();
    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "settings",
                                       settings, "web-context", context, "network-session", network,
                                       "user-content-manager", content, nullptr));
    g_object_unref(display);

    g_signal_connect(view, "resource-load-started",
                     G_CALLBACK(+[](WebKitWebView *, WebKitWebResource *, WebKitURIRequest *request,
                                    gpointer data) {
        auto &self = *static_cast<View *>(data);
        const auto *uri = webkit_uri_request_get_uri(request);
        if (!uri || !g_str_has_prefix(uri, "kobalt-epub://book-"))
            self.externalResource = true;
    }), this);
    g_signal_connect(view, "decide-policy",
                     G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *decision,
                                    WebKitPolicyDecisionType type, gpointer data) -> gboolean {
        auto &self = *static_cast<View *>(data);
        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && self.loading) {
            auto *action = webkit_navigation_policy_decision_get_navigation_action(
                    WEBKIT_NAVIGATION_POLICY_DECISION(decision));
            auto *request = webkit_navigation_action_get_request(action);
            if (self.candidate.uri == webkit_uri_request_get_uri(request))
                return FALSE;
        }
        if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE && self.loading) {
            auto *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
            auto *request = webkit_response_policy_decision_get_request(response);
            if (self.candidate.uri == webkit_uri_request_get_uri(request))
                return FALSE;
        }
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }), this);
    g_signal_connect(view, "load-changed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent event, gpointer data) {
        auto &self = *static_cast<View *>(data);
        if (event == WEBKIT_LOAD_FINISHED && self.loading && self.completion) {
            self.loading = false;
            self.layout();
        }
    }), this);
    g_signal_connect(view, "load-failed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, const char *, GError *error,
                                    gpointer data) -> gboolean {
        auto &self = *static_cast<View *>(data);
        if (self.loading && self.completion) {
            self.loading = false;
            // A provisional failure can still be followed by LOAD_FINISHED.
            // Settle it outside this emission before attempting a rollback.
            const auto reason = g_strdup(error->message);
            g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer opaque) -> gboolean {
                auto *failure = static_cast<std::pair<View *, gchar *> *>(opaque);
                failure->first->failed(failure->second);
                return G_SOURCE_REMOVE;
            }, new std::pair<View *, gchar *>(&self, reason), +[](gpointer opaque) {
                auto *failure = static_cast<std::pair<View *, gchar *> *>(opaque);
                g_free(failure->second);
                delete failure;
            });
        }
        return TRUE;
    }), this);
}

View::~View()
{
    // The owner drains the asynchronous operation before destroying the view.
    g_assert(!completion);
    g_signal_handlers_disconnect_by_data(view, this);
    g_clear_object(&view);
    g_clear_object(&cancellable);
}

void View::show(Page page, GCancellable *cancel, Completion done)
{
    if (!healthy || completion || page.uri.compare(0, 19, "kobalt-epub://book-") != 0
        || !page.width || !page.height || page.width > 2048 || page.height > 2048
        || page.fontSize < 8 || page.fontSize > 96) {
        done(nullptr, "Invalid retained view request");
        return;
    }
    candidate = std::move(page);
    completion = std::move(done);
    cancellable = G_CANCELLABLE(g_object_ref(cancel));
    apply();
}

void View::apply()
{
    wpe_toplevel_resize(wpe_view_get_toplevel(webkit_web_view_get_wpe_view(view)), candidate.width,
                        candidate.height);
    const auto *uri = webkit_web_view_get_uri(view);
    if (!uri || candidate.uri != uri) {
        loading = true;
        webkit_web_view_load_uri(view, candidate.uri.c_str());
    } else {
        layout();
    }
}

void View::layout()
{
    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&args, "{sv}", "width", g_variant_new_uint32(candidate.width));
    g_variant_builder_add(&args, "{sv}", "height", g_variant_new_uint32(candidate.height));
    g_variant_builder_add(&args, "{sv}", "column", g_variant_new_uint32(candidate.column));
    g_variant_builder_add(&args, "{sv}", "fontSize", g_variant_new_uint32(candidate.fontSize));
    g_autoptr(GVariant) parameters = g_variant_ref_sink(g_variant_builder_end(&args));
    // Drain DOM mutations before rollback. Cancelling the client-side GLib
    // task alone would not prove the page-side script has stopped.
    webkit_web_view_call_async_javascript_function(
            view, layoutScript, -1, parameters, world, nullptr, nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                auto &self = *static_cast<View *>(data);
                g_autoptr(GError) error = nullptr;
                g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
                        WEBKIT_WEB_VIEW(source), result, &error);
                if (!value || !jsc_value_is_number(value))
                    return self.failed(error ? error->message : "Layout bridge did not complete");
                if (!self.restoring && g_cancellable_is_cancelled(self.cancellable))
                    return self.failed("Cancelled");
                webkit_web_view_get_snapshot(
                        self.view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                        self.restoring ? nullptr : self.cancellable,
                        +[](GObject *, GAsyncResult *snapshot, gpointer data) {
                            static_cast<View *>(data)->snapshot(snapshot);
                        }, data);
            }, this);
}

void View::snapshot(GAsyncResult *result)
{
    g_autoptr(GError) error = nullptr;
    g_autoptr(WebKitImage) image = webkit_web_view_get_snapshot_finish(view, result, &error);
    if (!image || webkit_image_get_width(image) != int(candidate.width)
        || webkit_image_get_height(image) != int(candidate.height))
        return failed(error ? error->message : "Unexpected snapshot geometry");
    if (restoring)
        return complete(nullptr, failure.c_str());
    if (g_cancellable_is_cancelled(cancellable))
        return failed("Cancelled");
    current = candidate;
    complete(image, nullptr);
}

void View::failed(const char *reason)
{
    if (restoring || current.uri.empty()) {
        healthy = false;
        return complete(nullptr, reason);
    }
    failure = reason;
    restoring = true;
    candidate = current;
    apply();
}

void View::complete(WebKitImage *image, const char *error)
{
    auto done = std::move(completion);
    completion = {};
    restoring = false;
    g_clear_object(&cancellable);
    done(image, error);
}
} // namespace retained

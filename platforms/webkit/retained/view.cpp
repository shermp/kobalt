#include "view.h"
#include "location-script.h"

#include <wpe/headless/wpe-headless.h>

#include <cmath>
#include <string_view>
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
    const rtl = getComputedStyle(document.documentElement).direction === 'rtl';
    if (requireFixtureFont) {
        const faces = await document.fonts.load(fontSize + 'px ReaderFixture', rtl ? 'שלום WPE' : 'WPE');
        if (!faces.length)
            throw new Error('Expected fixture font did not load');
    }
    await document.fonts.ready;
    const images = Array.from(document.images, image => image.decode());
    if (requireFixtureFont)
        await Promise.all(images);
    else
        await Promise.allSettled(images);
    // WebKit's overflow ends at the last glyph, so the final column may
    // otherwise clamp short of its viewport-aligned scroll position.
    if (globalThis.retainedTail)
        globalThis.retainedTail.remove();
    const extent = document.documentElement.scrollWidth;
    const columns = Math.max(1, Math.ceil(extent / width));
    if (columns * width > extent && columns > 1) {
        const tail = document.createElementNS(document.documentElement.namespaceURI, "span");
        const style = {
            position: "absolute", display: "block", visibility: "hidden",
            width: "1px", height: "1px", margin: "0", padding: "0", border: "0",
            top: "0", [rtl ? "right" : "left"]: (columns * width - 1) + "px"
        };
        for (const [property, value] of Object.entries(style))
            tail.style.setProperty(property, value, "important");
        document.documentElement.appendChild(tail);
        globalThis.retainedTail = tail;
    }
    let actual = column === 0xffffffff ? columns - 1 : column, quality = 2;
    if (restore || fragment) {
        const element = fragment ? document.getElementById(fragment) : null;
        const indexes = fragment ? (element ? elementPath(element) : null)
                                 : (path ? path.split(",").map(Number) : []);
        const resolved = indexes ? resolveSource(indexes, fragment ? -1 : offset, after) : null;
        if (!resolved)
            return [column, columns, 2, [], -1, true, 2];
        quality = resolved[2];
        if (fragment || indexes.length || offset >= 0) {
            const rect = positionRect(resolved[0], resolved[1], after);
            if (rect) {
                const position = rtl ? width - rect.right - window.scrollX : rect.left + window.scrollX;
                actual = Math.max(0, Math.min(columns - 1, Math.floor(position / width)));
            } else {
                quality = 1;
            }
        } else {
            actual = 0;
        }
    }
    if (actual >= columns)
        throw new Error('Column is out of range');
    const scroll = actual * width * (rtl ? -1 : 1);
    window.scrollTo(scroll, 0);
    await tick();
    await tick();
    if (window.scrollX !== scroll || (requireFixtureFont && document.documentElement.hasAttribute('data-book-script')))
        throw new Error('Scroll position or script policy mismatch: expected ' + scroll + ' got ' + window.scrollX + ' extent ' + document.documentElement.scrollWidth);
    const anchor = visibleSource(width, height);
    return [actual, columns, quality, ...anchor, visibleLinks(width, height)];
)JS";

bool integer(JSCValue *array, unsigned index, int min, int max, int &out)
{
    g_autoptr(JSCValue) value = jsc_value_object_get_property_at_index(array, index);
    if (!value || !jsc_value_is_number(value))
        return false;
    const double number = jsc_value_to_double(value);
    if (!std::isfinite(number) || std::trunc(number) != number || number < min || number > max)
        return false;
    out = int(number);
    return true;
}

bool readPageInfo(JSCValue *value, const std::string &href, PageInfo &info)
{
    int column, count, quality, offset, precision;
    if (!value || !jsc_value_is_array(value) || !integer(value, 0, 0, 65535, column)
        || !integer(value, 1, 1, 65536, count) || !integer(value, 2, 0, 2, quality)
        || !integer(value, 4, -1, 2 * 1024 * 1024, offset) || !integer(value, 6, 0, 2, precision))
        return false;
    g_autoptr(JSCValue) path = jsc_value_object_get_property_at_index(value, 3);
    g_autoptr(JSCValue) affinity = jsc_value_object_get_property_at_index(value, 5);
    if (!path || !jsc_value_is_array(path) || !affinity || !jsc_value_is_boolean(affinity))
        return false;
    g_autoptr(JSCValue) length = jsc_value_object_get_property(path, "length");
    const int size = length ? jsc_value_to_int32(length) : -1;
    if (size < 0 || size > 128)
        return false;
    Location anchor;
    anchor.href = href;
    anchor.offset = offset;
    anchor.after = jsc_value_to_boolean(affinity);
    for (int i = 0; i < size; ++i) {
        int index;
        if (!integer(path, i, 0, 2 * 1024 * 1024, index))
            return false;
        anchor.path.push_back(unsigned(index));
    }
    info = { unsigned(column), unsigned(count), std::move(anchor), Restoration(precision),
             Restoration(quality) };
    {
        g_autoptr(JSCValue) links = jsc_value_object_get_property_at_index(value, 7);
        if (links && jsc_value_is_array(links)) {
            g_autoptr(JSCValue) length = jsc_value_object_get_property(links, "length");
            const auto count = length ? jsc_value_to_int32(length) : 0;
            for (int i = 0; i < count && i < 24; ++i) {
                g_autoptr(JSCValue) row = jsc_value_object_get_property_at_index(links, i);
                if (!row || !jsc_value_is_array(row))
                    continue;
                g_autoptr(JSCValue) target = jsc_value_object_get_property_at_index(row, 0);
                g_autofree char *uri = target && jsc_value_is_string(target)
                        ? jsc_value_to_string(target)
                        : nullptr;
                g_autoptr(JSCValue) note = jsc_value_object_get_property_at_index(row, 5);
                int x, y, width, height;
                if (uri && note && jsc_value_is_boolean(note)
                    && std::string_view(uri).size() <= 1800 && integer(row, 1, 0, 2048, x)
                    && integer(row, 2, 0, 2048, y) && integer(row, 3, 1, 2048, width)
                    && integer(row, 4, 1, 2048, height))
                    info.links.push_back({ uri, unsigned(x), unsigned(y), unsigned(width),
                                           unsigned(height), bool(jsc_value_to_boolean(note)) });
            }
        }
    }
    return column < count;
}

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

View::View(WebKitWebContext *context, const char *style, bool fixtureFont,
           const char *securityPolicy)
    : requireFixtureFont(fixtureFont)
{
    g_autoptr(WebKitSettings) settings = webkit_settings_new();
    healthy = software(settings);
    webkit_settings_set_enable_javascript_markup(settings, FALSE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    g_autoptr(WebKitNetworkSession) network = webkit_network_session_new_ephemeral();
    g_autoptr(WebKitUserContentManager) content = webkit_user_content_manager_new();
    auto *sheet = webkit_user_style_sheet_new(style, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                              WEBKIT_USER_STYLE_LEVEL_USER, nullptr, nullptr);
    webkit_user_content_manager_add_style_sheet(content, sheet);
    webkit_user_style_sheet_unref(sheet);
    auto *display = wpe_display_headless_new();
    view = WEBKIT_WEB_VIEW(
            g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "settings", settings,
                         "web-context", context, "network-session", network, "user-content-manager",
                         content, "default-content-security-policy", securityPolicy, nullptr));
    g_object_unref(display);

    g_signal_connect(view, "resource-load-started",
                     G_CALLBACK(+[](WebKitWebView *, WebKitWebResource *, WebKitURIRequest *request,
                                    gpointer data) {
                         auto &self = *static_cast<View *>(data);
                         const auto *uri = webkit_uri_request_get_uri(request);
                         if (!uri || !g_str_has_prefix(uri, "kobalt-epub://book-"))
                             self.externalResource = true;
                     }),
                     this);
    g_signal_connect(view, "decide-policy",
                     G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *decision,
                                    WebKitPolicyDecisionType type, gpointer data) -> gboolean {
                         auto &self = *static_cast<View *>(data);
                         if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION
                             && self.loading) {
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
                     }),
                     this);
    g_signal_connect(view, "load-changed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent event, gpointer data) {
                         auto &self = *static_cast<View *>(data);
                         if (event == WEBKIT_LOAD_FINISHED && self.loading && self.completion) {
                             self.loading = false;
                             self.layout();
                         }
                     }),
                     this);
    g_signal_connect(view, "load-failed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, const char *, GError *error,
                                    gpointer data) -> gboolean {
                         auto &self = *static_cast<View *>(data);
                         if (self.loading && self.completion) {
                             self.loading = false;
                             // A provisional failure can still be followed by LOAD_FINISHED.
                             // Settle it outside this emission before attempting a rollback.
                             const auto reason = g_strdup(error->message);
                             g_idle_add_full(
                                     G_PRIORITY_DEFAULT,
                                     +[](gpointer opaque) -> gboolean {
                                         auto *failure =
                                                 static_cast<std::pair<View *, gchar *> *>(opaque);
                                         failure->first->failed(failure->second);
                                         return G_SOURCE_REMOVE;
                                     },
                                     new std::pair<View *, gchar *>(&self, reason),
                                     +[](gpointer opaque) {
                                         auto *failure =
                                                 static_cast<std::pair<View *, gchar *> *>(opaque);
                                         g_free(failure->second);
                                         delete failure;
                                     });
                         }
                         return TRUE;
                     }),
                     this);
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
    if (!healthy || completion || page.uri.compare(0, 19, "kobalt-epub://book-") != 0 || !page.width
        || !page.height || page.width > 2048 || page.height > 2048 || page.fontSize < 8
        || page.fontSize > 96) {
        done(nullptr, "Invalid retained view request");
        return;
    }
    if (page.href.empty())
        page.href = page.uri.substr(page.uri.find('/', sizeof("kobalt-epub://") - 1) + 1);
    if (page.location
        && (page.location->version != 1 || page.location->href != page.href
            || page.location->path.size() > 128 || page.location->offset < -1
            || page.location->offset > 2 * 1024 * 1024)) {
        done(nullptr, "Unresolved source location");
        return;
    }
    if (!page.location && page.fragment.empty() && confirmed && page.uri == current.uri
        && (page.width != current.width || page.height != current.height
            || page.fontSize != current.fontSize))
        page.location = confirmed->anchor;
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
    g_variant_builder_add(&args, "{sv}", "requireFixtureFont",
                          g_variant_new_uint32(requireFixtureFont));
    const auto location = candidate.location.value_or(Location{});
    std::string path;
    for (auto index : location.path) {
        if (!path.empty())
            path += ',';
        path += std::to_string(index);
    }
    g_variant_builder_add(&args, "{sv}", "path", g_variant_new_string(path.c_str()));
    g_variant_builder_add(&args, "{sv}", "fragment",
                          g_variant_new_string(candidate.fragment.c_str()));
    g_variant_builder_add(&args, "{sv}", "restore",
                          g_variant_new_uint32(candidate.location.has_value()));
    g_variant_builder_add(&args, "{sv}", "offset", g_variant_new_int32(location.offset));
    g_variant_builder_add(&args, "{sv}", "after", g_variant_new_uint32(location.after));
    g_autoptr(GVariant) parameters = g_variant_ref_sink(g_variant_builder_end(&args));
    // Drain DOM mutations before rollback. Cancelling the client-side GLib
    // task alone would not prove the page-side script has stopped.
    static const std::string script = std::string(sourceLocationScript) + layoutScript;
    webkit_web_view_call_async_javascript_function(
            view, script.c_str(), script.size(), parameters, world, nullptr, nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                auto &self = *static_cast<View *>(data);
                g_autoptr(GError) error = nullptr;
                g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
                        WEBKIT_WEB_VIEW(source), result, &error);
                if (!readPageInfo(value, self.candidate.href, self.measured))
                    return self.failed(error ? error->message : "Layout bridge did not complete");
                if ((self.candidate.location || !self.candidate.fragment.empty())
                    && self.measured.restoration == Restoration::Unresolved)
                    return self.failed("Unresolved source location");
                if (!self.restoring && g_cancellable_is_cancelled(self.cancellable))
                    return self.failed("Cancelled");
                webkit_web_view_get_snapshot(
                        self.view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                        self.restoring ? nullptr : self.cancellable,
                        +[](GObject *, GAsyncResult *snapshot, gpointer data) {
                            static_cast<View *>(data)->snapshot(snapshot);
                        },
                        data);
            },
            this);
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
    current.column = measured.column;
    current.location.reset();
    current.fragment.clear();
    confirmed = measured;
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

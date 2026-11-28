#include "view.h"
#include "location-script.h"

#include <wpe/headless/wpe-headless.h>

#include <cmath>
#include <memory>
#include <string_view>
#include <utility>

namespace retained {
namespace {
bool sameLayout(const Page &a, const Page &b)
{
    return a.uri == b.uri && a.width == b.width && a.height == b.height && a.fontSize == b.fontSize;
}

bool coldBatchEnabled(const char *name)
{
    const char *value = g_getenv(name);
    return !value || g_strcmp0(value, "1") == 0;
}

constexpr char world[] = "kobalt-retained-dom-v1";
constexpr char layoutScript[] = R"JS(
    // Page-authored scripts are disabled. This bridge only measures and moves
    // the current document inside its own fixed viewport.
    if (globalThis.retainedDocument !== document) {
        globalThis.retainedDocument = document;
        globalThis.retainedLayout = null;
        // Resource completion can change layout even with publication scripts disabled.
        const invalidate = () => { globalThis.retainedLayout = null; };
        document.addEventListener('load', invalidate, true);
        document.addEventListener('error', invalidate, true);
    }
    const tick = () => new Promise(resolve => requestAnimationFrame(resolve));
    const start = performance.now();
    const end = start + 3000;
    const root = document.documentElement;
    const layoutStable = layout => layout && layout === globalThis.retainedLayout
        && innerWidth === width && innerHeight === height
        && document.fonts.status === 'loaded' && document.fonts.ready === layout.fonts
        && !document.getAnimations().length
        && root.scrollWidth === layout.scrollWidth && root.scrollHeight === layout.scrollHeight;

    async function render(allowReuse) {
        const marks = measure ? [] : null;
        const mark = () => { if (measure) marks.push(Math.round((performance.now() - start) * 1000)); };
        const retained = globalThis.retainedLayout;
        const reuse = allowReuse && layoutStable(retained)
            && (!previewColumns || retained.columns === previewColumns);
        const waitForImages = images => {
            const decodes = images.map(image => image.decode());
            return requireFixtureFont ? Promise.all(decodes) : Promise.allSettled(decodes);
        };
        let rtl, columns, visibleDecode;
        if (reuse) {
            ({rtl, columns, visibleDecode} = retained);
            mark();
            if (measure) marks.push(marks[0], marks[0], marks[0], marks[0]);
        } else {
            globalThis.retainedLayout = null;
            while (innerWidth !== width || innerHeight !== height) {
                if (performance.now() >= end)
                    throw new Error('Viewport did not reach the requested dimensions');
                await tick();
            }
            mark();
)JS"
#ifdef KOBALT_RELATED_VIEW_PROBE
                                R"JS(
            globalThis.webkit?.messageHandlers?.rv0Phase?.postMessage('style');
)JS"
#endif
                                R"JS(
            root.style.setProperty('--reader-font-size', fontSize + 'px');
            document.body.getBoundingClientRect();
            rtl = getComputedStyle(root).direction === 'rtl';
)JS"
#ifdef KOBALT_RELATED_VIEW_PROBE
                                R"JS(
            globalThis.webkit?.messageHandlers?.rv0Phase?.postMessage('style-done');
)JS"
#endif
                                R"JS(
            mark();
            if (requireFixtureFont) {
                const faces = await document.fonts.load(fontSize + 'px ReaderFixture', rtl ? 'שלום WPE' : 'WPE');
                if (!faces.length)
                    throw new Error('Expected fixture font did not load');
            }
            await document.fonts.ready;
            mark();
            const images = Array.from(document.images);
            // Only defer pixel decoding once intrinsic sizes are available.
            visibleDecode = coldVisibleDecode && images.every(image => image.complete
                && image.loading !== 'lazy' && !image.srcset && image.parentElement?.localName !== 'picture');
            if (!skipImageDecode && !visibleDecode)
                await waitForImages(images);
            mark();
            // Extend the final column to its viewport-aligned scroll position.
            if (globalThis.retainedTail)
                globalThis.retainedTail.remove();
            const extent = root.scrollWidth;
            columns = Math.max(1, Math.ceil(extent / width));
            if (columns * width > extent && columns > 1) {
                const tail = document.createElement('div');
                const style = {
                    position: 'absolute', display: 'block', visibility: 'hidden', pointerEvents: 'none',
                    width: '1px', height: '1px', top: '0', [rtl ? 'right' : 'left']: (columns * width - 1) + 'px'
                };
                for (const [property, value] of Object.entries(style))
                    tail.style[property] = value;
                root.appendChild(tail);
                globalThis.retainedTail = tail;
            }
            if (document.fonts.status === 'loaded' && images.every(image => image.complete)
                && !document.getAnimations().length) {
                globalThis.retainedLayout = {
                    rtl, columns, visibleDecode, fonts: document.fonts.ready,
                    scrollWidth: root.scrollWidth, scrollHeight: root.scrollHeight
                };
            }
            mark();
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
        let visibleDecodeUs = 0;
        if (visibleDecode) {
            const decodeStart = measure ? performance.now() : 0;
            const images = Array.from(document.images).filter(image => {
                const rect = image.getBoundingClientRect();
                return rect.width > 0 && rect.height > 0
                    && rect.left < width && rect.right > 0 && rect.top < height && rect.bottom > 0;
            });
            await waitForImages(images);
            if (measure) visibleDecodeUs = Math.round((performance.now() - decodeStart) * 1000);
        }
        await tick();
        await tick();
        if (reuse && !layoutStable(retained))
            return render(false);
        if (window.scrollX !== scroll || (requireFixtureFont && document.documentElement.hasAttribute('data-book-script')))
            throw new Error('Scroll position or script policy mismatch: expected ' + scroll + ' got ' + window.scrollX + ' extent ' + document.documentElement.scrollWidth);
        mark();
        // A successful same-layout preview already has its image and page info.
        // Its rollback only needs to confirm the restored scroll after both ticks.
        if (reuse && previewColumns) {
            const result = [actual, columns, true];
            if (measure) {
                marks.push(marks[5], marks[5], marks[5]);
                result[9] = marks;
                result[10] = true;
                result[11] = Array(9).fill(0);
                result[12] = visibleDecodeUs;
            }
            return result;
        }
        const firstWork = measure ? [0, 0, 0] : null;
        const lastWork = measure ? [0, 0, 0] : null;
        const linkWork = measure ? [0, 0] : null;
)JS"
#ifdef KOBALT_RELATED_VIEW_PROBE
                                R"JS(
        globalThis.webkit?.messageHandlers?.rv0Phase?.postMessage('javascript');
)JS"
#endif
                                R"JS(
        const anchor = visibleSource(width, height, false, firstWork, batchColdFirstSource && actual > 0);
        mark();
        const links = visibleLinks(width, height, linkWork, deferLinkResolution);
        mark();
        const pageEnd = visibleSource(width, height, true, lastWork, reuse || batchColdSource);
        mark();
)JS"
#ifdef KOBALT_RELATED_VIEW_PROBE
                                R"JS(
        globalThis.webkit?.messageHandlers?.rv0Phase?.postMessage('javascript-done');
)JS"
#endif
                                R"JS(
        const result = [actual, columns, quality, ...anchor, links, pageEnd];
        if (measure)
            result.push(marks, reuse, [...firstWork, ...lastWork, ...linkWork, links.length], visibleDecodeUs);
        return result;
    }
    return render(reuseLayout);
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

void readTiming(JSCValue *value, View::LayoutTiming &timing)
{
    if (!value || !jsc_value_is_array(value))
        return;
    g_autoptr(JSCValue) reused = jsc_value_object_get_property_at_index(value, 10);
    timing.reused = reused && jsc_value_is_boolean(reused) && jsc_value_to_boolean(reused);
    integer(value, 12, 0, 60 * G_USEC_PER_SEC, timing.visibleDecodeUs);
    g_autoptr(JSCValue) work = jsc_value_object_get_property_at_index(value, 11);
    if (work && jsc_value_is_array(work)) {
        for (unsigned i = 0; i < timing.sourceWork.size(); ++i)
            integer(work, i, 0, 2 * 1024 * 1024, timing.sourceWork[i]);
    }
    g_autoptr(JSCValue) marks = jsc_value_object_get_property_at_index(value, 9);
    if (!marks || !jsc_value_is_array(marks))
        return;
    int previous = 0;
    for (unsigned i = 0; i < timing.domUs.size(); ++i) {
        int current;
        if (!integer(marks, i, previous, 60 * G_USEC_PER_SEC, current))
            break;
        timing.domUs[i] = current - previous;
        previous = current;
    }
}

bool readWordAnchor(JSCValue *value, Location &out)
{
    if (!value || !jsc_value_is_array(value) || !integer(value, 1, 0, 2 * 1024 * 1024, out.offset))
        return false;
    g_autoptr(JSCValue) path = jsc_value_object_get_property_at_index(value, 0);
    g_autoptr(JSCValue) after = jsc_value_object_get_property_at_index(value, 2);
    if (!path || !jsc_value_is_array(path) || !after || !jsc_value_is_boolean(after))
        return false;
    g_autoptr(JSCValue) length = jsc_value_object_get_property(path, "length");
    const auto size = length ? jsc_value_to_int32(length) : -1;
    if (size < 0 || size > 128)
        return false;
    out.after = jsc_value_to_boolean(after);
    for (int i = 0; i < size; ++i) {
        int index;
        if (!integer(path, i, 0, 2 * 1024 * 1024, index))
            return false;
        out.path.push_back(unsigned(index));
    }
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
        g_autoptr(JSCValue) last = jsc_value_object_get_property_at_index(value, 8);
        int exact;
        if (last && jsc_value_is_array(last) && integer(last, 3, 0, 0, exact)
            && readWordAnchor(last, info.pageEnd))
            info.pageEnd.href = href;
        else
            info.pageEnd = {};
    }
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
           const char *securityPolicy, Timing *measurement
#ifdef KOBALT_RELATED_VIEW_PROBE
           ,
           const View *related
#endif
           )
    : timing(measurement),
      earlyAppearance(g_strcmp0(g_getenv("KOBALT_WPE_EARLY_APPEARANCE"), "1") == 0),
      textOnlyAppearance(!g_getenv("KOBALT_WPE_EARLY_APPEARANCE")
                         || g_strcmp0(g_getenv("KOBALT_WPE_EARLY_APPEARANCE"), "text") == 0),
      skipImageDecode(g_strcmp0(g_getenv("KOBALT_WPE_IMAGE_DECODE"), "none") == 0),
      visibleImageDecode(g_strcmp0(g_getenv("KOBALT_WPE_IMAGE_DECODE"), "visible") == 0),
      coldSourceBatch(coldBatchEnabled("KOBALT_WPE_COLD_SOURCE_BATCH")),
      coldFirstSourceBatch(coldBatchEnabled("KOBALT_WPE_COLD_FIRST_SOURCE_BATCH")),
      coldVisibleLinks(g_strcmp0(g_getenv("KOBALT_WPE_COLD_VISIBLE_LINKS"), "1") == 0),
      requireFixtureFont(fixtureFont)
{
    g_autoptr(WebKitSettings) settings = webkit_settings_new();
    healthy = software(settings);
    webkit_settings_set_enable_javascript_markup(settings, FALSE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    g_autoptr(WebKitUserContentManager) content = webkit_user_content_manager_new();
    // Register the isolated world so its layout state survives separate async script calls.
    auto *sheet = webkit_user_style_sheet_new_for_world(style, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                        WEBKIT_USER_STYLE_LEVEL_USER, world,
                                                        nullptr, nullptr);
    webkit_user_content_manager_add_style_sheet(content, sheet);
    webkit_user_style_sheet_unref(sheet);
#ifdef KOBALT_RELATED_VIEW_PROBE
    if (related) {
        view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related->view,
                                            "settings", settings, "user-content-manager", content,
                                            "default-content-security-policy", securityPolicy,
                                            nullptr));
    } else
#endif
    {
        g_autoptr(WebKitNetworkSession) network = webkit_network_session_new_ephemeral();
        auto *display = wpe_display_headless_new();
        view = WEBKIT_WEB_VIEW(g_object_new(
                WEBKIT_TYPE_WEB_VIEW, "display", display, "settings", settings, "web-context",
                context, "network-session", network, "user-content-manager", content,
                "default-content-security-policy", securityPolicy, nullptr));
        g_object_unref(display);
    }

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
                         if (!self.loading || !self.completion)
                             return;
                         if (event == WEBKIT_LOAD_COMMITTED && self.timing)
                             self.layoutTiming().loadCommitUs = g_get_monotonic_time();
                         if (event == WEBKIT_LOAD_FINISHED) {
                             if (self.timing)
                                 self.layoutTiming().loadFinishUs = g_get_monotonic_time();
                             self.loading = false;
                             self.layout(0, true);
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
#ifdef KOBALT_RELATED_VIEW_PROBE
    if (probeStage) {
        auto *content = webkit_web_view_get_user_content_manager(view);
        g_signal_handlers_disconnect_by_data(content, this);
        webkit_user_content_manager_unregister_script_message_handler(content, "rv0Phase", world);
    }
#endif
    g_signal_handlers_disconnect_by_data(view, this);
    g_clear_object(&view);
    if (appearance)
        webkit_user_style_sheet_unref(appearance);
    g_clear_object(&cancellable);
}

void View::wordAt(unsigned x, unsigned y, std::function<void(Word)> done)
{
    if (!confirmed || completion || x >= current.width || y >= current.height)
        return done({});
    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&args, "{sv}", "x", g_variant_new_uint32(x));
    g_variant_builder_add(&args, "{sv}", "y", g_variant_new_uint32(y));
    g_autoptr(GVariant) parameters = g_variant_ref_sink(g_variant_builder_end(&args));
    static const std::string script =
            std::string(sourceLocationScript) + "\nreturn wordAtPoint(x, y);";
    webkit_web_view_call_async_javascript_function(
            view, script.c_str(), script.size(), parameters, world, nullptr, nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                const std::unique_ptr<std::function<void(Word)>> finish(
                        static_cast<std::function<void(Word)> *>(data));
                g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
                        WEBKIT_WEB_VIEW(source), result, nullptr);
                Word word;
                int x, y, width, height;
                if (!value || !jsc_value_is_array(value) || !integer(value, 3, 0, 2048, x)
                    || !integer(value, 4, 0, 2048, y) || !integer(value, 5, 1, 2048, width)
                    || !integer(value, 6, 1, 2048, height) || x + width > 2048 || y + height > 2048)
                    return (*finish)({});
                g_autoptr(JSCValue) name = jsc_value_object_get_property_at_index(value, 0);
                g_autoptr(JSCValue) start = jsc_value_object_get_property_at_index(value, 1);
                g_autoptr(JSCValue) end = jsc_value_object_get_property_at_index(value, 2);
                if (!name || !jsc_value_is_string(name) || !readWordAnchor(start, word.start)
                    || !readWordAnchor(end, word.end))
                    return (*finish)({});
                g_autofree char *text = jsc_value_to_string(name);
                if (!text || !*text || std::string_view(text).size() > 1024)
                    return (*finish)({});
                word.text = text;
                word.x = unsigned(x);
                word.y = unsigned(y);
                word.width = unsigned(width);
                word.height = unsigned(height);
                (*finish)(std::move(word));
            },
            new std::function<void(Word)>(std::move(done)));
}

void View::rangeRects(const Location &start, const Location &end,
                      std::function<void(RangeGeometry)> done)
{
    if (!confirmed || completion)
        return done({});
    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE_VARDICT);
    const auto add = [&](const char *pathName, const char *offsetName, const char *afterName,
                         const Location &position) {
        std::string path;
        for (const auto index : position.path) {
            if (!path.empty())
                path += ',';
            path += std::to_string(index);
        }
        g_variant_builder_add(&args, "{sv}", pathName, g_variant_new_string(path.c_str()));
        g_variant_builder_add(&args, "{sv}", offsetName, g_variant_new_int32(position.offset));
        g_variant_builder_add(&args, "{sv}", afterName, g_variant_new_uint32(position.after));
    };
    add("firstPath", "firstOffset", "firstAfter", start);
    add("lastPath", "lastOffset", "lastAfter", end);
    g_autoptr(GVariant) parameters = g_variant_ref_sink(g_variant_builder_end(&args));
    static const std::string script = std::string(sourceLocationScript)
            + "\nreturn rangeRects([firstPath, firstOffset, firstAfter],"
              " [lastPath, lastOffset, lastAfter]);";
    using Completion = std::function<void(RangeGeometry)>;
    webkit_web_view_call_async_javascript_function(
            view, script.c_str(), script.size(), parameters, world, nullptr, nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                const std::unique_ptr<Completion> finish(static_cast<Completion *>(data));
                g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
                        WEBKIT_WEB_VIEW(source), result, nullptr);
                if (!value || !jsc_value_is_array(value))
                    return (*finish)({});
                g_autoptr(JSCValue) rows = jsc_value_object_get_property_at_index(value, 0);
                if (!rows || !jsc_value_is_array(rows))
                    return (*finish)({});
                g_autoptr(JSCValue) length = jsc_value_object_get_property(rows, "length");
                const auto count = length ? jsc_value_to_int32(length) : -1;
                if (count < 0 || count > 64)
                    return (*finish)({});
                RangeGeometry geometry;
                const auto edge = [value](unsigned index) {
                    g_autoptr(JSCValue) row = jsc_value_object_get_property_at_index(value, index);
                    int fixed, first, last, direction;
                    if (!row || !jsc_value_is_array(row) || !integer(row, 0, 0, 2048, fixed)
                        || !integer(row, 1, 0, 2048, first)
                        || !integer(row, 2, first + 1, 2048, last)
                        || !integer(row, 3, 1, 4, direction))
                        return RangeGeometry::Edge{};
                    return RangeGeometry::Edge{ unsigned(fixed), unsigned(first), unsigned(last),
                                                unsigned(direction) };
                };
                geometry.start = edge(1);
                geometry.end = edge(2);
                for (int i = 0; i < count; ++i) {
                    g_autoptr(JSCValue) rect = jsc_value_object_get_property_at_index(rows, i);
                    int x, y, width, height;
                    if (!rect || !jsc_value_is_array(rect) || !integer(rect, 0, 0, 2048, x)
                        || !integer(rect, 1, 0, 2048, y) || !integer(rect, 2, 1, 2048, width)
                        || !integer(rect, 3, 1, 2048, height) || x + width > 2048
                        || y + height > 2048)
                        return (*finish)({});
                    geometry.rects.push_back(
                            { unsigned(x), unsigned(y), unsigned(width), unsigned(height) });
                }
                (*finish)(std::move(geometry));
            },
            new Completion(std::move(done)));
}

#ifdef KOBALT_RELATED_VIEW_PROBE
void View::abandonProbeLoad()
{
    // The probe may discard a fresh view if WebKit sends no stop notification.
    // A DOM operation or a rollback must always complete before destruction.
    g_assert(loading && completion && current.uri.empty());
    loading = false;
    failed("Probe discarded a provisional load without a WebKit notification");
}

void View::observeProbeStages(std::function<void(const char *)> observer)
{
    g_assert(!probeStage);
    probeStage = std::move(observer);
    auto *content = webkit_web_view_get_user_content_manager(view);
    g_signal_connect(content, "script-message-received::rv0Phase",
                     G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *value, gpointer data) {
                         auto &self = *static_cast<View *>(data);
                         g_autofree char *name = jsc_value_to_string(value);
                         self.probeStage(name);
                     }),
                     this);
    const bool registered =
            webkit_user_content_manager_register_script_message_handler(content, "rv0Phase", world);
    g_assert(registered);
}

void View::prepare(Page page, GCancellable *cancel, std::function<void(const char *)> done)
{
    g_assert(!completion);
    preparing = true;
    show(std::move(page), cancel, [this, done = std::move(done)](WebKitImage *, const char *error) {
        preparing = false;
        done(error);
    });
}
#endif

void View::show(Page page, GCancellable *cancel, Completion done, bool preview)
{
    if (!healthy || completion || page.uri.compare(0, 19, "kobalt-epub://book-") != 0 || !page.width
        || !page.height || page.width > 2048 || page.height > 2048 || page.fontSize < 8
        || page.fontSize > 96 || (preview && !confirmed)) {
        needsFullLayout = true;
        done(nullptr, "Invalid retained view request");
        return;
    }
    if (page.href.empty())
        page.href = page.uri.substr(page.uri.find('/', sizeof("kobalt-epub://") - 1) + 1);
    if (page.location
        && (page.location->version != 1 || page.location->href != page.href
            || page.location->path.size() > 128 || page.location->offset < -1
            || page.location->offset > 2 * 1024 * 1024)) {
        needsFullLayout = true;
        done(nullptr, "Unresolved source location");
        return;
    }
    if (!page.location && page.fragment.empty() && confirmed && page.uri == current.uri
        && (page.width != current.width || page.height != current.height
            || page.fontSize != current.fontSize))
        page.location = confirmed->anchor;
    if (timing) {
        if (preview)
            timing->operation = "preview";
        else if (!confirmed)
            timing->operation = "open";
        else if (current.uri != page.uri)
            timing->operation = "crossing";
        else if (current.width != page.width || current.height != page.height
                 || current.fontSize != page.fontSize)
            timing->operation = "reflow";
        else
            timing->operation = page.location || !page.fragment.empty() ? "relocate" : "turn";
    }
    candidate = std::move(page);
    previewing = preview;
    previewInfo.reset();
    completion = std::move(done);
    cancellable = G_CANCELLABLE(g_object_ref(cancel));
    apply();
}

void View::installAppearance(unsigned fontSize)
{
    if (fontSize == appearanceFontSize)
        return;
    auto *content = webkit_web_view_get_user_content_manager(view);
    if (appearance) {
        webkit_user_content_manager_remove_style_sheet(content, appearance);
        webkit_user_style_sheet_unref(appearance);
    }
    // Preserve the font used when WebKit first constructs broken-image placeholders.
    constexpr char textOnlyRoot[] =
            ":root:not(:has(img, svg, image, picture, video, canvas, object, embed, input))";
    const auto css = std::string(textOnlyAppearance ? textOnlyRoot : ":root")
            + " { --reader-font-size: " + std::to_string(fontSize) + "px; }";
    appearance = webkit_user_style_sheet_new_for_world(
            css.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_STYLE_LEVEL_USER, world,
            nullptr, nullptr);
    webkit_user_content_manager_add_style_sheet(content, appearance);
    appearanceFontSize = fontSize;
}

void View::apply(unsigned previewColumns)
{
    if (timing)
        layoutTiming().applyUs = g_get_monotonic_time();
    auto *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(view));
    int width, height;
    wpe_toplevel_get_size(toplevel, &width, &height);
    if (width != int(candidate.width) || height != int(candidate.height))
        wpe_toplevel_resize(toplevel, candidate.width, candidate.height);
    const auto *uri = webkit_web_view_get_uri(view);
    if (!uri || candidate.uri != uri) {
        if (earlyAppearance || textOnlyAppearance)
            installAppearance(candidate.fontSize);
        loading = true;
        if (timing) {
            layoutTiming().earlyFontSize = appearanceFontSize;
            layoutTiming().loadStartUs = g_get_monotonic_time();
        }
        webkit_web_view_load_uri(view, candidate.uri.c_str());
    } else {
        layout(previewColumns);
    }
}

void View::layout(unsigned previewColumns, bool freshDocument)
{
    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&args, "{sv}", "width", g_variant_new_uint32(candidate.width));
    g_variant_builder_add(&args, "{sv}", "height", g_variant_new_uint32(candidate.height));
    const bool reuse = previewColumns
            || (confirmed && !needsFullLayout && !restoring && !previewing
                && sameLayout(current, candidate) && !candidate.location
                && candidate.fragment.empty());
    g_variant_builder_add(&args, "{sv}", "reuseLayout", g_variant_new_uint32(reuse));
    g_variant_builder_add(&args, "{sv}", "previewColumns", g_variant_new_uint32(previewColumns));
    g_variant_builder_add(&args, "{sv}", "column", g_variant_new_uint32(candidate.column));
    g_variant_builder_add(&args, "{sv}", "fontSize", g_variant_new_uint32(candidate.fontSize));
    g_variant_builder_add(&args, "{sv}", "skipImageDecode", g_variant_new_uint32(skipImageDecode));
    const bool cold = freshDocument && !restoring && !previewing;
    g_variant_builder_add(&args, "{sv}", "coldVisibleDecode",
                          g_variant_new_uint32(visibleImageDecode && cold));
    g_variant_builder_add(&args, "{sv}", "batchColdSource",
                          g_variant_new_uint32(coldSourceBatch && cold));
    g_variant_builder_add(&args, "{sv}", "batchColdFirstSource",
                          g_variant_new_uint32(coldFirstSourceBatch && cold));
    g_variant_builder_add(&args, "{sv}", "deferLinkResolution",
                          g_variant_new_uint32(coldVisibleLinks && cold));
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
    g_variant_builder_add(&args, "{sv}", "measure", g_variant_new_uint32(timing != nullptr));
    g_autoptr(GVariant) parameters = g_variant_ref_sink(g_variant_builder_end(&args));
    // Drain DOM mutations before rollback. Cancelling the client-side GLib
    // task alone would not prove the page-side script has stopped.
    static const std::string script = std::string(sourceLocationScript) + layoutScript;
    if (timing)
        layoutTiming().domStartUs = g_get_monotonic_time();
    webkit_web_view_call_async_javascript_function(
            view, script.c_str(), script.size(), parameters, world, nullptr, nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                auto &self = *static_cast<View *>(data);
                g_autoptr(GError) error = nullptr;
                g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
                        WEBKIT_WEB_VIEW(source), result, &error);
                if (self.timing) {
                    auto &timing = self.layoutTiming();
                    timing.domEndUs = g_get_monotonic_time();
                    readTiming(value, timing);
                }
                if (self.restoring && self.previewImage && value && jsc_value_is_array(value)) {
                    g_autoptr(JSCValue) restored = jsc_value_object_get_property_at_index(value, 2);
                    if (restored && jsc_value_is_boolean(restored)
                        && jsc_value_to_boolean(restored)) {
                        int column, columns;
                        if (!integer(value, 0, 0, 65535, column)
                            || !integer(value, 1, 1, 65536, columns)
                            || unsigned(column) != self.current.column
                            || unsigned(columns) != self.confirmed->chapterPages)
                            return self.failed("Unexpected preview rollback geometry");
                        return self.complete(self.previewImage, nullptr);
                    }
                }
                if (!readPageInfo(value, self.candidate.href, self.measured))
                    return self.failed(error ? error->message : "Layout bridge did not complete");
                if ((self.candidate.location || !self.candidate.fragment.empty())
                    && self.measured.restoration == Restoration::Unresolved)
                    return self.failed("Unresolved source location");
                if (!self.restoring && g_cancellable_is_cancelled(self.cancellable))
                    return self.failed("Cancelled");
                if (self.restoring && self.previewImage)
                    return self.complete(self.previewImage, nullptr);
#ifdef KOBALT_RELATED_VIEW_PROBE
                if (!self.restoring && self.preparing) {
                    self.current = self.candidate;
                    self.current.column = self.measured.column;
                    self.confirmed = self.measured;
                    self.complete(nullptr, nullptr);
                    return;
                }
#endif
                if (self.timing)
                    self.layoutTiming().snapshotStartUs = g_get_monotonic_time();
#ifdef KOBALT_RELATED_VIEW_PROBE
                if (self.probeStage)
                    self.probeStage("snapshot");
#endif
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
    if (timing)
        layoutTiming().snapshotEndUs = g_get_monotonic_time();
    if (!image || webkit_image_get_width(image) != int(candidate.width)
        || webkit_image_get_height(image) != int(candidate.height))
        return failed(error ? error->message : "Unexpected snapshot geometry");
    if (restoring)
        return complete(nullptr, failure.c_str());
    if (g_cancellable_is_cancelled(cancellable))
        return failed("Cancelled");
    if (previewing) {
        previewImage = WEBKIT_IMAGE(g_object_ref(image));
        previewInfo = measured;
        restoring = true;
        const unsigned previewColumns =
                sameLayout(candidate, current) ? confirmed->chapterPages : 0;
        candidate = current;
        apply(previewColumns);
        return;
    }
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
    g_autoptr(WebKitImage) captured = std::exchange(previewImage, nullptr);
    auto done = std::move(completion);
    completion = {};
    needsFullLayout = restoring || !image;
#ifdef KOBALT_RELATED_VIEW_PROBE
    if (!restoring && preparing && !error)
        needsFullLayout = false;
#endif
    restoring = false;
    previewing = false;
    g_clear_object(&cancellable);
    done(image, error);
}
} // namespace retained

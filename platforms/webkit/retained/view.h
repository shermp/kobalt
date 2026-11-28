#pragma once

#include <wpe/webkit.h>

#include "page-model.h"

#include <array>
#include <functional>
#include <string>

namespace retained {
struct Page
{
    std::string uri;
    unsigned width = 0, height = 0, column = 0, fontSize = 18;
    std::string href = {};
    std::optional<Location> location = {};
    std::string fragment = {};
};

struct Word
{
    Location start, end;
    std::string text;
    unsigned x = 0, y = 0, width = 0, height = 0;
};

struct Rect
{
    unsigned x, y, width, height;
};

struct RangeGeometry
{
    struct Edge
    {
        unsigned fixed = 0, first = 0, last = 0, direction = 0;
    };

    std::vector<Rect> rects;
    Edge start, end;
};

class View
{
public:
    struct LayoutTiming
    {
        long long applyUs = 0, loadStartUs = 0, loadCommitUs = 0, loadFinishUs = 0;
        long long domStartUs = 0, domEndUs = 0;
        long long snapshotStartUs = 0, snapshotEndUs = 0;
        bool reused = false;
        unsigned earlyFontSize = 0;
        int visibleDecodeUs = -1;
        std::array<int, 9> sourceWork{ -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        std::array<int, 9> domUs{ -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    };

    struct Timing
    {
        const char *operation = "invalid";
        LayoutTiming candidate, rollback;
    };

    // The frame is borrowed for the duration of completion. A null frame
    // means the candidate did not commit. Calls are serialized by the owner.
    using Completion = std::function<void(WebKitImage *, const char *)>;
    View(WebKitWebContext *, const char *style, bool requireFixtureFont = true,
         const char *securityPolicy = nullptr, Timing *timing = nullptr
#ifdef KOBALT_RELATED_VIEW_PROBE
         ,
         const View *related = nullptr
#endif
    );
    ~View();
    void show(Page, GCancellable *, Completion, bool preview = false);
#ifdef KOBALT_RELATED_VIEW_PROBE
    void prepare(Page, GCancellable *, std::function<void(const char *)>);
    void abandonProbeLoad();
    void observeProbeStages(std::function<void(const char *)>);

    WebKitWebView *webView() const { return view; }
#endif
    void wordAt(unsigned x, unsigned y, std::function<void(Word)>);
    void rangeRects(const Location &start, const Location &end, std::function<void(RangeGeometry)>);

    std::optional<PageInfo> pageInfo() const { return confirmed; }

    std::optional<PageInfo> previewPageInfo() const { return previewInfo; }

    bool usable() const { return healthy; }

    bool sawExternalResource() const { return externalResource; }

private:
#ifdef KOBALT_RELATED_VIEW_PROBE
    bool preparing = false;
    std::function<void(const char *)> probeStage;
#endif
    void installAppearance(unsigned fontSize);
    void apply(unsigned previewColumns = 0);
    void layout(unsigned previewColumns, bool freshDocument = false);
    void snapshot(GAsyncResult *);
    void failed(const char *);
    void complete(WebKitImage *, const char *);

    LayoutTiming &layoutTiming() { return restoring ? timing->rollback : timing->candidate; }

    WebKitWebView *view = nullptr;
    WebKitUserStyleSheet *appearance = nullptr;
    unsigned appearanceFontSize = 0;
    Page current, candidate;
    std::optional<PageInfo> confirmed, previewInfo;
    PageInfo measured;
    Completion completion;
    GCancellable *cancellable = nullptr;
    WebKitImage *previewImage = nullptr;
    std::string failure;
    Timing *const timing;
    const bool earlyAppearance, textOnlyAppearance, skipImageDecode, visibleImageDecode,
            coldSourceBatch, coldFirstSourceBatch, coldVisibleLinks;
    bool requireFixtureFont;
    bool healthy = true, loading = false, restoring = false, externalResource = false;
    bool previewing = false, needsFullLayout = false;
};
} // namespace retained

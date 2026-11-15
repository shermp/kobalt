#pragma once

#include <wpe/webkit.h>

#include "page-model.h"

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
    // The frame is borrowed for the duration of completion. A null frame
    // means the candidate did not commit. Calls are serialized by the owner.
    using Completion = std::function<void(WebKitImage *, const char *)>;
    View(WebKitWebContext *, const char *style, bool requireFixtureFont = true,
         const char *securityPolicy = nullptr);
    ~View();
    void show(Page, GCancellable *, Completion, bool preview = false);
    void wordAt(unsigned x, unsigned y, std::function<void(Word)>);
    void rangeRects(const Location &start, const Location &end, std::function<void(RangeGeometry)>);

    std::optional<PageInfo> pageInfo() const { return confirmed; }

    std::optional<PageInfo> previewPageInfo() const { return previewInfo; }

    bool usable() const { return healthy; }

    bool sawExternalResource() const { return externalResource; }

private:
    void apply();
    void layout();
    void snapshot(GAsyncResult *);
    void failed(const char *);
    void complete(WebKitImage *, const char *);

    WebKitWebView *view = nullptr;
    Page current, candidate;
    std::optional<PageInfo> confirmed, previewInfo;
    PageInfo measured;
    Completion completion;
    GCancellable *cancellable = nullptr;
    WebKitImage *previewImage = nullptr;
    std::string failure;
    bool requireFixtureFont;
    bool healthy = true, loading = false, restoring = false, externalResource = false;
    bool previewing = false;
};
} // namespace retained

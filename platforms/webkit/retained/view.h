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

class View
{
public:
    // The frame is borrowed for the duration of completion. A null frame
    // means the candidate did not commit. Calls are serialized by the owner.
    using Completion = std::function<void(WebKitImage *, const char *)>;
    View(WebKitWebContext *, const char *style, bool requireFixtureFont = true,
         const char *securityPolicy = nullptr);
    ~View();
    void show(Page, GCancellable *, Completion);
    void wordAt(unsigned x, unsigned y, std::function<void(Word)>);
    void rangeRects(const Location &start, const Location &end,
                    std::function<void(std::vector<Rect>)>);

    std::optional<PageInfo> pageInfo() const { return confirmed; }

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
    std::optional<PageInfo> confirmed;
    PageInfo measured;
    Completion completion;
    GCancellable *cancellable = nullptr;
    std::string failure;
    bool requireFixtureFont;
    bool healthy = true, loading = false, restoring = false, externalResource = false;
};
} // namespace retained

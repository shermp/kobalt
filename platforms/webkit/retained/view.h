#pragma once

#include <wpe/webkit.h>

#include <functional>
#include <string>

namespace retained {
struct Page
{
    std::string uri;
    unsigned width = 0, height = 0, column = 0, fontSize = 18;
};

class View
{
public:
    // The frame is borrowed for the duration of completion. A null frame
    // means the candidate did not commit. Calls are serialized by the owner.
    using Completion = std::function<void(WebKitImage *, const char *)>;
    View(WebKitWebContext *, const char *style);
    ~View();
    void show(Page, GCancellable *, Completion);
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
    Completion completion;
    GCancellable *cancellable = nullptr;
    std::string failure;
    bool healthy = true, loading = false, restoring = false, externalResource = false;
};
} // namespace retained

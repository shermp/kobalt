#pragma once

#include "book-wire.h"
#include "../retained/view.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>

class BookRender
{
public:
    using Reply = std::function<void(spike::Type, uint64_t, const std::string &)>;
    BookRender(std::array<unsigned char *, 2>, uint32_t capacity, Reply, int trace);
    ~BookRender();
    bool receive(const spike::Message &);
    void disconnect(std::function<void()>);

private:
    struct Resource
    {
        Resource(uint64_t, WebKitURISchemeRequest *);
        ~Resource();
        uint64_t id;
        WebKitURISchemeRequest *request;
        std::string media, bytes;
        size_t length = 0;
        long long queuedUs = 0, requestedUs = 0;
        unsigned chunks = 0, queueDepth = 0;
        uint64_t render = 0;
    };

    void enqueue(WebKitURISchemeRequest *);
    void requestResource();
    bool resourceData(const std::string &);
    void refuseResources();
    bool query(const spike::Message &);
    void finishQuery(spike::Type, spike::Revision, const std::string &);
    void render(uint64_t, spike::BookPage, long long receivedUs);
    void snapshot(WebKitImage *);
    void finish(bool);
    void reject(uint64_t, spike::RenderError);
    void cancel();
    void drainHelpers();
    void logTiming(uint64_t id, bool success) const;

    const std::array<unsigned char *, 2> maps;
    const uint32_t capacity;
    Reply reply;
    std::function<void()> drained;
    WebKitWebContext *context = nullptr;
    std::unique_ptr<retained::View> view;
#ifdef KOBALT_LAZY_BOOK_PROBE
    std::unique_ptr<retained::View> previous;
    std::string currentUri, previousUri;
    bool selectedPrevious = false;
#endif
    GCancellable *cancellable = nullptr;
    std::deque<std::unique_ptr<Resource>> resources;
    spike::BookPage page;
    spike::Revision committed;
    uint64_t generation = 0, renderId = 0, queryId = 0, shutdownId = 0, nextResource = 0;
    const bool measure;
    const int trace;
    retained::View::Timing viewTiming;
    long long receivedUs = 0, pixelStartUs = 0, pixelEndUs = 0, copyStartUs = 0, copyEndUs = 0;
};

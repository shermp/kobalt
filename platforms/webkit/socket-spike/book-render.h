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
    BookRender(std::array<unsigned char *, 2>, uint32_t capacity, Reply);
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
    };

    void enqueue(WebKitURISchemeRequest *);
    void requestResource();
    bool resourceData(const std::string &);
    void refuseResources();
    bool query(const spike::Message &);
    void finishQuery(spike::Type, spike::Revision, const std::string &);
    void render(uint64_t, spike::BookPage);
    void snapshot(WebKitImage *);
    void finish(bool);
    void reject(uint64_t, spike::RenderError);
    void cancel();
    void drainHelpers();

    const std::array<unsigned char *, 2> maps;
    const uint32_t capacity;
    Reply reply;
    std::function<void()> drained;
    WebKitWebContext *context = nullptr;
    std::unique_ptr<retained::View> view;
    GCancellable *cancellable = nullptr;
    std::deque<std::unique_ptr<Resource>> resources;
    spike::BookPage page;
    spike::Revision committed;
    uint64_t generation = 0, renderId = 0, queryId = 0, shutdownId = 0, nextResource = 0;
};

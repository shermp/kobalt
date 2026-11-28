#pragma once

#include "render-wire.h"

#include <gio/gio.h>

#include <array>
#include <functional>
#include <memory>

class WebKitRender
{
public:
    using Reply = std::function<void(spike::Type, uint64_t, const std::string &)>;
    using Drain = std::function<void()>;

    WebKitRender(std::array<unsigned char *, 2>, uint32_t capacity, Reply);
    ~WebKitRender();
    bool receive(const spike::Message &);
    void disconnect(Drain);

private:
    struct Job;
    void start(const spike::Message &);
    void snapshot(GAsyncResult *);
    void finish(bool);
    void cancel();
    void reject(uint64_t, spike::RenderError);
    uint64_t generation = 0, documentSequence = 0, shutdownId = 0;
    const std::array<unsigned char *, 2> maps;
    const uint32_t capacity;
    Reply reply;
    Drain drained;
    spike::Revision current;
    spike::FrameSpec frame;
    std::unique_ptr<Job> active;
    GObject *committedView = nullptr;
};

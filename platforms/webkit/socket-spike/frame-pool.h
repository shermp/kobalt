#pragma once

#include "pool-wire.h"

#include <QtGui/QImage>

#include <array>
#include <atomic>
#include <memory>

class FramePool
{
public:
    FramePool(uint64_t generation, uint32_t width, uint32_t height);
    bool valid() const { return buffers[0] && buffers[1]; }
    int descriptor(unsigned slot) const;
    bool reserve(uint32_t color, spike::FrameSpec &);
    bool reserve(uint32_t color, uint32_t width, uint32_t height, spike::FrameSpec &);
    QImage publish(const spike::FrameSpec &, const std::string &reply);
    void discard(const spike::FrameSpec &);

    const uint64_t generation;
    const uint32_t width, height, capacity;

private:
    enum State { Free, Writing, Leased };
    struct Slot
    {
        ~Slot();
        int fd = -1;
        const uchar *map = nullptr;
        uint32_t capacity = 0;
        std::atomic<State> state{Free};
    };
    static void release(void *);
    bool matches(const spike::FrameSpec &) const;
    std::array<std::shared_ptr<Slot>, 2> buffers;
};

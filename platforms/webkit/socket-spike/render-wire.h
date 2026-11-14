#pragma once

#include "pool-wire.h"

namespace spike {
enum class RenderError : uint32_t { Invalid = 1, Stale, Busy, Failed, Cancelled, Boundary };

struct Revision
{
    uint64_t document = 0, layout = 0, frame = 0;
    bool operator==(const Revision &other) const
    {
        return document == other.document && layout == other.layout && frame == other.frame;
    }
    std::string encode() const
    {
        std::string data;
        put(data, document, 8);
        put(data, layout, 8);
        put(data, frame, 8);
        return data;
    }
    static Revision decode(const char *data)
    {
        return {read(data, 8), read(data + 8, 8), read(data + 16, 8)};
    }
};

inline std::string targetRequest(uint64_t id)
{
    std::string data;
    put(data, id, 8);
    return data;
}

inline std::string stateReply(Revision revision, const FrameSpec &frame)
{
    auto reply = revision.encode();
    put(reply, frame.color, 4); // exact fixture token and two-page index
    put(reply, frame.width, 4);
    put(reply, frame.height, 4);
    return reply;
}
} // namespace spike

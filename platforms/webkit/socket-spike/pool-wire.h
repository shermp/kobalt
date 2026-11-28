#pragma once

#include "wire.h"

namespace spike {
constexpr unsigned maxFrameBytes = 16 * 1024 * 1024;
constexpr uint32_t bgraPremultiplied = 1;

inline bool validGeometry(uint32_t width, uint32_t height, uint32_t capacity)
{
    return width && height && capacity <= maxFrameBytes && uint64_t(width) * 4 <= capacity
            && uint64_t(width) * 4 * height <= capacity;
}

// The mock writes tightly packed opaque BGRA rows. The production codec is
// scoped to M3.4c. These fields exercise the same publication checks.
struct FrameSpec
{
    uint64_t generation = 0;
    uint32_t slot = 0, width = 0, height = 0, color = 0;

    std::string encode() const
    {
        std::string data;
        put(data, generation, 8);
        put(data, slot, 4);
        put(data, width, 4);
        put(data, height, 4);
        put(data, color, 4);
        return data;
    }

    static bool decode(const std::string &data, FrameSpec &spec)
    {
        if (data.size() < 24)
            return false;
        const auto *p = data.data();
        spec = { read(p, 8), uint32_t(read(p + 8, 4)), uint32_t(read(p + 12, 4)),
                 uint32_t(read(p + 16, 4)), uint32_t(read(p + 20, 4)) };
        return spec.generation && spec.slot < 2 && spec.color <= 1;
    }

    std::string reply(uint32_t capacity) const
    {
        auto data = encode();
        put(data, width * 4, 4);
        put(data, capacity, 4);
        put(data, bgraPremultiplied, 4);
        return data;
    }
};
} // namespace spike

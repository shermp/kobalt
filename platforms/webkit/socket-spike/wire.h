#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace spike {
constexpr unsigned headerSize = 20;
constexpr unsigned maxPayload = 4096;
constexpr uint32_t magic = 0x53455057; // WPES, on the wire
constexpr uint16_t version = 1;

enum Type : uint16_t {
    Hello = 1, Work, Cancel, Shutdown,
    PoolHello, Frame, Finish,
    Open = 20, Turn, Navigate, SetViewport, SetAppearance, Query, Close,
    Ready = 101, Pending, Cancelled, CancelAck, Bye, HalfWritten, FrameReady, StateReply, ErrorReply, Closed
};

struct Message
{
    Type type;
    uint64_t id;
    std::string payload;
};

inline void put(std::string &bytes, uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(char((value >> (i * 8)) & 255));
}

inline uint64_t read(const char *bytes, unsigned width)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return value;
}

inline std::string encode(Type type, uint64_t id, const std::string &payload = {})
{
    std::string bytes;
    put(bytes, magic, 4);
    put(bytes, version, 2);
    put(bytes, type, 2);
    put(bytes, payload.size(), 4);
    put(bytes, id, 8);
    return bytes + payload;
}

enum class Parse { Incomplete, Message, Invalid };

inline Parse take(std::string &bytes, Message &message)
{
    if (bytes.size() < headerSize)
        return Parse::Incomplete;
    const char *p = bytes.data();
    const auto length = read(p + 8, 4);
    const auto id = read(p + 12, 8);
    if (read(p, 4) != magic || read(p + 4, 2) != version || length > maxPayload || !id)
        return Parse::Invalid;
    if (bytes.size() < headerSize + length)
        return Parse::Incomplete;
    message = {Type(read(p + 6, 2)), id, bytes.substr(headerSize, length)};
    bytes.erase(0, headerSize + length);
    return Parse::Message;
}
} // namespace spike

#pragma once

#include "render-wire.h"

#include <vector>

namespace spike {
constexpr unsigned maxBookPath = 128;
constexpr unsigned maxBookHref = 1400;
constexpr unsigned maxResourceBytes = 8 * 1024 * 1024;
constexpr unsigned resourceChunk = 4000;
constexpr uint32_t lastColumn = UINT32_MAX;

// A render command describes a position in the host's admitted spine. The
// render request ID names the frame, independently of document/layout changes.
struct BookAnchor
{
    std::vector<uint32_t> path;
    int offset = -1;
    bool after = true;

    void append(std::string &data) const
    {
        put(data, path.size(), 2);
        put(data, unsigned(offset + 1), 4);
        put(data, after, 1);
        for (const auto index : path)
            put(data, index, 4);
    }

    bool decode(const std::string &data, size_t start)
    {
        if (start > data.size() || data.size() - start < 7)
            return false;
        const auto *p = data.data() + start;
        const auto count = read(p, 2), encoded = read(p + 2, 4), affinity = read(p + 6, 1);
        if (count > maxBookPath || encoded > 2 * 1024 * 1024 + 1 || affinity > 1
            || data.size() - start != 7 + count * 4)
            return false;
        path.clear();
        for (unsigned i = 0; i < count; ++i) {
            const auto index = read(p + 7 + i * 4, 4);
            if (index > 2 * 1024 * 1024)
                return false;
            path.push_back(uint32_t(index));
        }
        offset = int(encoded) - 1;
        after = affinity != 0;
        return true;
    }
};

struct BookPage
{
    FrameSpec frame;
    Revision revision;
    uint32_t column = 0, fontPixels = 18;
    std::string uri, href;
    bool restore = false;
    BookAnchor anchor;

    std::string encode() const
    {
        auto data = frame.encode() + revision.encode();
        put(data, column, 4);
        put(data, fontPixels, 4);
        put(data, uri.size(), 2);
        put(data, href.size(), 2);
        put(data, restore, 1);
        data += uri;
        data += href;
        if (restore)
            anchor.append(data);
        return data;
    }

    bool decode(const std::string &data)
    {
        constexpr unsigned header = 61;
        if (data.size() < header || !FrameSpec::decode(data, frame) || frame.color)
            return false;
        const auto *p = data.data();
        revision = Revision::decode(p + 24);
        column = uint32_t(read(p + 48, 4));
        fontPixels = uint32_t(read(p + 52, 4));
        const auto uriSize = read(p + 56, 2), hrefSize = read(p + 58, 2);
        const auto flag = read(p + 60, 1);
        const auto end = header + uriSize + hrefSize;
        if (!revision.document || !revision.layout || !revision.frame || fontPixels < 8
            || fontPixels > 96 || (column > 65535 && column != lastColumn) || !uriSize
            || uriSize > maxBookHref || !hrefSize || hrefSize > maxBookHref || flag > 1
            || end > data.size())
            return false;
        uri = data.substr(header, uriSize);
        href = data.substr(header + uriSize, hrefSize);
        if (uri.compare(0, 19, "kobalt-epub://book-") || uri.find('\0') != std::string::npos
            || href.find('\0') != std::string::npos)
            return false;
        restore = flag != 0;
        return restore ? anchor.decode(data, end) : end == data.size();
    }
};

struct BookFrame
{
    Revision revision;
    uint32_t column = 0, columns = 0;
    unsigned precision = 2, restoration = 2;
    BookAnchor anchor;

    std::string encode(const FrameSpec &frame) const
    {
        auto data = frame.reply(frame.width * 4 * frame.height) + revision.encode();
        put(data, column, 4);
        put(data, columns, 4);
        put(data, precision, 1);
        put(data, restoration, 1);
        anchor.append(data);
        return data;
    }

    bool decode(const std::string &data)
    {
        constexpr unsigned header = 70;
        if (data.size() < header)
            return false;
        const auto *p = data.data();
        revision = Revision::decode(p + 36);
        column = uint32_t(read(p + 60, 4));
        columns = uint32_t(read(p + 64, 4));
        precision = unsigned(read(p + 68, 1));
        restoration = unsigned(read(p + 69, 1));
        return column < columns && columns <= 65536 && precision <= 2 && restoration <= 2
                && anchor.decode(data, header);
    }
};
} // namespace spike

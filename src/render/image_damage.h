#pragma once
#include <QImage>
#include <QRect>
#include <cstring>

namespace kobalt::render {
// One rectangle bounds the changed pixels, so a text page does not turn into
// hundreds of separate panel submissions. Row padding is not display content.
inline QRect imageDamage(const QImage &before, const QImage &after)
{
    if (before.cacheKey() == after.cacheKey())
        return {};
    const QRect all = before.rect().united(after.rect());
    if (before.size() != after.size() || before.format() != after.format() || before.depth() != 32
        || before.devicePixelRatio() != after.devicePixelRatio())
        return all;
    const int width = after.width();
    const auto rowEqual = [&](int y) {
        return std::memcmp(before.constScanLine(y), after.constScanLine(y),
                           size_t(width) * sizeof(quint32))
                == 0;
    };
    int top = 0, bottom = after.height() - 1;
    while (top <= bottom && rowEqual(top))
        ++top;
    if (top > bottom)
        return {};
    while (bottom > top && rowEqual(bottom))
        --bottom;

    // Once the top and bottom are known, pixels inside the horizontal
    // bounds cannot change the rectangle. Compare only its two margins.
    int left = width, right = -1;
    for (int y = top; y <= bottom; ++y) {
        const auto *a = reinterpret_cast<const quint32 *>(before.constScanLine(y));
        const auto *b = reinterpret_cast<const quint32 *>(after.constScanLine(y));
        if (left > 0 && std::memcmp(a, b, size_t(left) * sizeof(quint32)) != 0) {
            int x = 0;
            while (a[x] == b[x])
                ++x;
            left = x;
        }
        const int tail = right + 1;
        if (tail < width
            && std::memcmp(a + tail, b + tail, size_t(width - tail) * sizeof(quint32)) != 0) {
            int x = width - 1;
            while (a[x] == b[x])
                --x;
            right = x;
        }
        if (left == 0 && right == width - 1)
            break;
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}
} // namespace kobalt::render

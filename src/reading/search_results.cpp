#include "search_results.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace kobalt {
namespace {
bool positionsOnly(const SearchHit &hit)
{
    return !hit.start.isEmpty() && !hit.end.isEmpty() && hit.page == -1 && hit.before.isNull()
            && hit.text.isNull() && hit.after.isNull() && hit.chapter.isNull()
            && hit.printedPage.isNull();
}
} // namespace

void SearchResults::append(const QVector<SearchHit> &hits)
{
    // Nearby matches usually share most of their positions. Small groups keep
    // that prefix useful even when one batch covers several chapters.
    constexpr int groupSize = 16;
    for (int first = 0; first < hits.size();) {
        const int count = qMin(groupSize, int(hits.size()) - first);
        const auto *begin = hits.constData() + first;
        const auto *end = begin + count;
        Group group;
        group.first = count_;
        if (count > 1 && std::all_of(begin, end, positionsOnly)) {
            const auto *base = begin->start.constData();
            int shared = begin->start.size();
            qint64 units = 0;
            for (auto *hit = begin; hit != end; ++hit) {
                for (const auto *position : { &hit->start, &hit->end }) {
                    units += position->size();
                    shared = qMin(shared, int(position->size()));
                    const auto *chars = position->constData();
                    // Most positions keep the entire prefix found so far.
                    if (shared && std::memcmp(base, chars, shared * sizeof(QChar)) != 0)
                        shared = std::mismatch(base, base + shared, chars).first - base;
                }
            }
            const auto tails = units - qint64(shared) * count * 2;
            if (tails <= std::numeric_limits<int>::max()) {
                group.prefix = begin->start.left(shared);
                group.suffixes.reserve(int(tails));
                group.ends.reserve(2 * count);
                for (auto *hit = begin; hit != end; ++hit) {
                    for (const auto *position : { &hit->start, &hit->end }) {
                        group.suffixes.append(position->constData() + shared,
                                              position->size() - shared);
                        group.ends.append(group.suffixes.size());
                    }
                }
            }
        }
        if (group.ends.isEmpty())
            group.detailed = hits.mid(first, count);
        groups_.append(std::move(group));
        first += count;
        count_ += count;
    }
}

SearchHit SearchResults::Group::hit(int index) const
{
    if (!detailed.isEmpty())
        return detailed.at(index);
    const int i = index * 2;
    const int begin = i ? ends.at(i - 1) : 0;
    const int middle = ends.at(i);
    return { prefix + suffixes.mid(begin, middle - begin),
             prefix + suffixes.mid(middle, ends.at(i + 1) - middle),
             {},
             {},
             {},
             -1 };
}

QVector<SearchHit> SearchResults::page(int first, int count) const
{
    QVector<SearchHit> result;
    if (first < 0 || first >= count_ || count <= 0)
        return result;
    count = qMin(count, count_ - first);
    result.reserve(count);
    auto group = std::upper_bound(
                         groups_.cbegin(), groups_.cend(), first,
                         [](int index, const Group &candidate) { return index < candidate.first; })
            - 1;
    for (int i = first; i < first + count; ++i) {
        if (group + 1 != groups_.cend() && i == (group + 1)->first)
            ++group;
        result.append(group->hit(i - group->first));
    }
    return result;
}

void SearchResults::clear()
{
    groups_.clear();
    count_ = 0;
}
} // namespace kobalt

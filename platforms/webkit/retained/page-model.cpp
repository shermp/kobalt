#include "page-model.h"

#include <algorithm>
#include <limits>

namespace retained {
std::optional<unsigned> PageModel::chapter(const std::string &href) const
{
    for (unsigned i = 0; i < spine.size(); ++i) {
        if (spine[i].href == href)
            return i;
    }
    return {};
}

bool PageModel::observe(unsigned index, unsigned columns)
{
    if (index >= entries.size() || !columns || columns > maxPagesPerChapter)
        return false;
    if (spine[index].bytes) {
        if (entries[index].precision == Precision::Exact)
            knownPages -= entries[index].pages;
        else
            knownBytes += spine[index].bytes;
        knownPages += columns;
    }
    entries[index] = {columns, Precision::Exact};
    return true;
}

void PageModel::resetLayout()
{
    std::fill(entries.begin(), entries.end(), Count{});
    next = 0;
    knownPages = knownBytes = 0;
}

PageModel::Count PageModel::count(unsigned index) const
{
    return index < entries.size() ? entries[index] : Count{};
}

PageModel::Count PageModel::total() const
{
    if (entries.empty())
        return {};
    uint64_t sum = 0;
    auto precision = Precision::Exact;
    for (const auto &entry : entries) {
        if (entry.precision == Precision::Unknown)
            return {};
        if (entry.precision == Precision::Estimated)
            precision = Precision::Estimated;
        sum += entry.pages;
        if (sum > std::numeric_limits<unsigned>::max())
            return {};
    }
    return {unsigned(sum), precision};
}

bool PageModel::indexNext(const std::function<bool()> &cancelled)
{
    if (cancelled() || next >= entries.size() || !knownPages || !knownBytes)
        return false;
    // Calibrate on chapters already laid out. The byte-based result is
    // deliberately reported as an estimate.
    if (entries[next].precision == Precision::Unknown && spine[next].bytes) {
        const double estimate = double(spine[next].bytes) * double(knownPages) / double(knownBytes);
        entries[next] = {unsigned(std::clamp(estimate + 0.5, 1.0, double(maxPagesPerChapter))),
                         Precision::Estimated};
    }
    ++next;
    return true;
}
} // namespace retained

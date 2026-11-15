#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace retained {
// Standalone representation of Blaze Location v1. The eventual host adapter
// converts these fields to kobalt::Location without a new storage format.
struct Location
{
    unsigned version = 1;
    std::string href;
    std::vector<unsigned> path;
    int offset = -1;
    bool after = true;

    bool operator==(const Location &) const = default;
};

enum class Restoration { Exact, Approximate, Unresolved };

struct PageLink
{
    std::string uri;
    unsigned x = 0, y = 0, width = 0, height = 0;
};

struct PageInfo
{
    unsigned column = 0, chapterPages = 0;
    Location anchor;
    Restoration precision = Restoration::Unresolved;
    Restoration restoration = Restoration::Unresolved;
    std::vector<PageLink> links = {};
};

// In-memory page model for one publication and layout. Exact totals require
// every spine item to have a layout observation. An indexer contributes only
// bounded estimates, one source item per step, and never renders frames.
class PageModel
{
public:
    enum class Precision { Unknown, Estimated, Exact };

    struct Count
    {
        unsigned pages = 0;
        Precision precision = Precision::Unknown;
    };

    struct Chapter
    {
        std::string href;
        uint32_t bytes; // admitted XHTML size supplied by the owning publication
    };

    explicit PageModel(std::vector<Chapter> spine)
        : spine(std::move(spine)), entries(this->spine.size())
    {
    }

    std::optional<unsigned> chapter(const std::string &href) const;
    bool observe(unsigned index, unsigned columns);
    void resetLayout();
    Count count(unsigned index) const;
    Count total() const;

    // Returns true if an item was indexed. The caller schedules another step
    // only after serving foreground work. Cancellation keeps previous units.
    bool indexNext(const std::function<bool()> &cancelled);

private:
    static constexpr unsigned maxPagesPerChapter = 65536;
    std::vector<Chapter> spine;
    std::vector<Count> entries;
    size_t next = 0;
    uint64_t knownPages = 0, knownBytes = 0;
};
} // namespace retained

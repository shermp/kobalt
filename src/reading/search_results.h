#pragma once
#include "engine/types.h"

namespace kobalt {
// The search panel keeps every match so it can open any results page.
class SearchResults
{
public:
    void append(const QVector<SearchHit> &hits);
    void clear();

    bool isEmpty() const { return count_ == 0; }

    int size() const { return count_; }

    QVector<SearchHit> page(int first, int count) const;

private:
    struct Group
    {
        int first;
        // Plain matches share the common prefix of their position strings.
        // Each pair of ends selects the two suffixes for one match.
        QString prefix, suffixes;
        QVector<int> ends;
        QVector<SearchHit> detailed;

        SearchHit hit(int index) const;
    };

    QVector<Group> groups_;
    int count_ = 0;
};
} // namespace kobalt

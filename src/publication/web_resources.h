#pragma once

#include "resource_cache.h"
#include "engine/types.h"
#include "location.h"
#include "reading/chapter.h"
#include <functional>

namespace kobalt {
class Publication;
class SourceLease;

// Worker-owned access to one admitted EPUB. The supplied cache must belong to
// the same opening and lease. The renderer never receives a filesystem path.
class WebResources
{
public:
    struct Resource
    {
        QByteArray bytes;
        QByteArray mediaType;
    };

    static std::unique_ptr<WebResources> open(const Publication &, std::shared_ptr<ResourceCache>,
                                              std::shared_ptr<SourceLease>, QString *error);

    const QByteArray &fingerprint() const { return fingerprint_; }

    const QStringList &spine() const { return spine_; }

    QString navigation() const { return navigation_; }

    QString title() const { return title_; }

    QString author() const { return author_; }

    QVector<ChapterLink> chapters();
    QString noteText(const QString &spineMember, const QString &fragment);
    SearchBatch searchText(const QString &query, SearchCursor, int lastItem,
                           const std::function<bool()> &current);
    TextRange textRange(const QString &start, const QString &end, bool withContext);
    TextRange selectionRange(const QStringList &tokens);
    QVector<int> intersectingRanges(const QString &start, const QString &end,
                                    const QVector<QPair<QString, QString>> &ranges);

    QByteArray url(const QString &member) const;
    Resource read(const QByteArray &encodedUrl);

private:
    WebResources() = default;

    struct Member
    {
        QString name;
        QByteArray media;
        qint64 size;
    };

    struct SearchRun
    {
        QString text;
        QVector<int> path;
        int offset;
    };

    bool indexSearchItem(int item, const std::function<bool()> &current);
    int countedPosition(const Location &) const;

    QByteArray origin_, fingerprint_;
    QStringList spine_;
    QString navigation_, ncx_, title_, author_;
    QMap<QByteArray, Member> members_;
    std::shared_ptr<ResourceCache> cache_;
    std::shared_ptr<SourceLease> lease_;
    qint64 servedBytes_ = 0;
    int requests_ = 0;
    int searchItem_ = -1;
    QVector<SearchRun> searchRuns_;
    QMap<QByteArray, QPair<int, int>> indexedElements_;
    QString indexedText_;
};
} // namespace kobalt

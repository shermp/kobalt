#pragma once

#include "resource_cache.h"
#include "reading/chapter.h"

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

    QByteArray origin_, fingerprint_;
    QStringList spine_;
    QString navigation_, ncx_, title_, author_;
    QMap<QByteArray, Member> members_;
    std::shared_ptr<ResourceCache> cache_;
    std::shared_ptr<SourceLease> lease_;
    qint64 servedBytes_ = 0;
    int requests_ = 0;
};
} // namespace kobalt

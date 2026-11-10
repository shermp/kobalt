#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace kobalt {
struct Bookmark;
struct Highlight;
struct HighlightExcerpt;
enum class AnnotationPositionFormat { XPointer, SourceV1 };

QString annotationIdentity(const QString &, AnnotationPositionFormat);
bool annotationPositionValid(const QString &, AnnotationPositionFormat);
bool annotationMetadataValid(const QString &folder, AnnotationPositionFormat, QString *error,
                             QJsonObject *metadata = nullptr);

// Captured by each store. Explicit 0/1 overrides the build default.
bool annotationDatabaseEnabled();

struct AnnotationBook
{
    QString root, identity, title, key;
    QByteArray fingerprint;
    QString database = {};
    AnnotationPositionFormat positions = AnnotationPositionFormat::XPointer;
};

struct AnnotationDatabaseState
{
    qint64 revision = -1;
    QHash<QString, int> rowBytes;
    QVector<qint64> ordinals;
    QString adoptedFrom;
};

// Every operation runs on the profile writer thread, which keeps one connection
// open until closeProfile() (profile_database.h). Callers on other threads wait
// for the operation.
class AnnotationDatabase
{
public:
    static bool load(const AnnotationBook &, QVector<Highlight> *, AnnotationDatabaseState *,
                     QString *error);
    static bool load(const AnnotationBook &, QVector<Bookmark> *, AnnotationDatabaseState *,
                     QString *error);
    static bool loadCompact(const AnnotationBook &, QVector<Highlight> *, AnnotationDatabaseState *,
                            QString *error);
    static bool details(const AnnotationBook &, const QStringList &ids,
                        const AnnotationDatabaseState &, QVector<Highlight> *, QString *error);
    static bool excerpts(const AnnotationBook &, const QStringList &ids,
                         const AnnotationDatabaseState &, QVector<HighlightExcerpt> *,
                         QString *error);
    static bool save(const AnnotationBook &, const QVector<Highlight> &,
                     const QVector<Highlight> &previous, AnnotationDatabaseState *, QString *error);
    static bool save(const AnnotationBook &, const QVector<Bookmark> &,
                     const QVector<Bookmark> &previous, AnnotationDatabaseState *, QString *error);
    static QString path(const QString &root);

private:
    static bool loadRows(const AnnotationBook &, int kind, QVector<Highlight> *,
                         AnnotationDatabaseState *, QString *error, bool compact = false);
    static bool legacy(const QString &folder, QVector<Highlight> *, QString *error,
                       AnnotationPositionFormat);
    static bool legacy(const QString &folder, QVector<Bookmark> *, QString *error,
                       AnnotationPositionFormat);
};
} // namespace kobalt

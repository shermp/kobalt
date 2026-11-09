#pragma once
#include "document_types.h"

namespace kobalt {
// Worker-owned document contract. Optional reading features return unavailable
// until a backend implements them; source positions never cross backend formats.
class Document
{
public:
    virtual ~Document() = default;
    virtual const EngineFontInputs &fontInputs() const = 0;
    virtual bool openResources(std::shared_ptr<ResourceCache>, const QMap<QString, qint64> &, QSize,
                               const QJsonObject &, QString *, OpeningProgress = {},
                               OpeningResult * = nullptr, std::function<void()> = {}) = 0;
    virtual bool applyAppearance(const QJsonObject &, QSize = {}, OpeningProgress = {}) = 0;
    virtual bool go(int) = 0;
    virtual bool goTo(const QString &) = 0;
    virtual int turnTarget(int, int) const = 0;
    virtual int page() const = 0;
    virtual int pages() const = 0;
    virtual QSize viewport() const = 0;
    virtual QImage image(int) = 0;
    virtual QJsonObject state() const = 0;
    virtual QString title() const = 0;
    virtual QString author() const = 0;
    virtual QString resourceError() const = 0;
    virtual QJsonObject resourceStats() const = 0;
    virtual void releaseImages() = 0;
    virtual int cachedPages() const = 0;

    virtual const char *backend() const { return "crengine"; }

    virtual bool prepareNeighbours() const { return true; }

    // Current request/lease predicate, read only on the worker. Proxies may
    // interrupt IPC without involving the GUI or publishing an obsolete frame.
    virtual void setCurrent(std::function<bool()>) { }

    virtual void close() { }

    virtual QStringList fontFaces() { return {}; }

    virtual QVector<ReadingFont> fontCatalog() { return {}; }

    virtual QString textFontFace() const { return {}; }

    virtual int embeddedFontCount() const { return 0; }

    virtual bool loadNextImage() { return false; }

    virtual qint64 cachedImageKey(int) const { return 0; }

    virtual void prepareDamage(int, int) { }

    virtual bool preparedDamage(qint64, qint64, QRect *) const { return false; }

    virtual bool rebindResources(const std::shared_ptr<ResourceCache> &,
                                 const QMap<QString, qint64> &)
    {
        return false;
    }

    virtual void setDocumentCache(QString, QByteArray, QByteArray, qint64) { }

    virtual void setUserStyleSheet(QByteArray) { }

    virtual const QByteArray &userStyleSheet() const
    {
        static const QByteArray empty;
        return empty;
    }

    virtual bool openedFromCache() const { return false; }

    virtual bool saveDocumentCache() { return false; }

    virtual DocumentWordCounts wordCounts(std::function<bool()> = {}) { return {}; }

    virtual qint64 totalWords() const { return -1; }

    virtual bool openPreview(Document &, QSize, QString *error)
    {
        if (error)
            *error = "Preview unavailable for this backend";
        return false;
    }
    enum class PositionTarget { Exact, Chapter, Book, Unavailable };

    virtual QString platformPosition(const PositionCodec &, QString *error) const
    {
        if (error)
            *error = "Native positions unavailable for this backend";
        return {};
    }

    virtual bool goToPlatform(const PositionCodec &, const QString &, QString *error)
    {
        if (error)
            *error = "Native positions unavailable for this backend";
        return false;
    }

    virtual PositionTarget goToPlatformNearest(const PositionCodec &, const QString &,
                                               QString *error)
    {
        if (error)
            *error = "Native positions unavailable for this backend";
        return PositionTarget::Unavailable;
    }

    virtual int comparePosition(const QString &, QString *error) const
    {
        if (error)
            *error = "Position unavailable for this backend";
        return 0;
    }

    virtual int comparePlatformPosition(const PositionCodec &, const QString &position,
                                        QString *error) const
    {
        return comparePosition(position, error);
    }

    virtual QVector<int> pagesOf(const QStringList &positions, bool = false,
                                 QVector<int> *order = nullptr, std::function<bool()> = {}) const
    {
        if (order)
            *order = QVector<int>(positions.size(), -1);
        return QVector<int>(positions.size(), -1);
    }

    virtual QString pageStart(int) const { return {}; }

    virtual QString pageExcerpt(int) const { return {}; }

    virtual WordSelection wordAt(QPoint) { return {}; }

    virtual QImage imageAt(QPoint) { return {}; }

    virtual AnnotationPageEdges annotationPageEdges() { return {}; }

    virtual AnnotationToken annotationTokenAt(QPoint) { return {}; }

    virtual AnnotationToken annotationDragTokenAt(QPoint, const QString &, bool) { return {}; }

    virtual TextRange textRange(const QString &, const QString &) { return {}; }

    virtual QVector<int> intersectingRanges(const QString &, const QString &,
                                            const QVector<QPair<QString, QString>> &) const
    {
        return {};
    }

    virtual TextRange annotationRange(const QString &, const QString &) { return {}; }

    virtual TextRange annotationRange(const QString &, const QString &, const QString &,
                                      const QString &, bool = false, bool = false,
                                      std::function<bool()> = {})
    {
        return {};
    }

    virtual QVector<PageLink> pageLinks() { return {}; }

    virtual LinkDetails linkDetails(const QString &, bool = true) { return {}; }

    virtual SearchBatch searchPage(const QString &, int)
    {
        SearchBatch result;
        result.done = true;
        return result;
    }

    virtual QVector<int> visibleRanges(const QVector<QPair<QString, QString>> &) const
    {
        return {};
    }

    virtual bool rangeVisible(const QString &, const QString &) const { return false; }

    virtual AnnotationGeometry annotationGeometry(const QString &, const QString &) { return {}; }

    virtual QVector<QRect> rangeRects(const QString &, const QString &, bool = false) { return {}; }

    virtual QVector<QVector<QRect>> pageRangeRects(int,
                                                   const QVector<QPair<QString, QString>> &ranges)
    {
        return QVector<QVector<QRect>>(ranges.size());
    }

    virtual QImage coverImage(QSize, const QString & = {}, QString * = nullptr) const { return {}; }

    virtual QVector<ChapterLink> chapters() const { return {}; }

    virtual QVector<ChapterLink> chapterGraph() const { return {}; }

    virtual ChapterSelection chapterSelection(const QVector<ChapterLink> &) const { return {}; }

    virtual ChapterProgress chapterProgress() const { return {}; }

    virtual QString chapterTitle(const QString &) const { return {}; }

    virtual void setChapters(QVector<ChapterLink>, QVector<ChapterLink>) { }

    virtual ResolvedChapters resolveChapters(const PositionCodec &, const QVector<ChapterTarget> &,
                                             const QVector<ChapterTarget> &) const
    {
        return {};
    }
};
} // namespace kobalt

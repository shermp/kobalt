#pragma once
#include "reading/document.h"
#include <QList>
#include <QProcess>

namespace kobalt {
// Qt-only proxy: WPE and its private libraries remain in the supervised helper.
// All methods (including construction of QProcess) run on the document worker.
class WebKitDocument final : public Document
{
public:
    WebKitDocument(EngineFontInputs fonts, QString path);
    ~WebKitDocument() override;

    const EngineFontInputs &fontInputs() const override { return fonts_; }

    const char *backend() const override { return "webkit"; }

    bool prepareNeighbours() const override { return false; }

    void setCurrent(std::function<bool()> current) override { current_ = std::move(current); }

    void setFingerprint(QByteArray fingerprint) { fingerprint_ = std::move(fingerprint); }

    void close() override;
    bool openResources(std::shared_ptr<ResourceCache>, const QMap<QString, qint64> &, QSize,
                       const QJsonObject &, QString *, OpeningProgress = {},
                       OpeningResult * = nullptr, std::function<void()> = {}) override;
    bool applyAppearance(const QJsonObject &, QSize = {}, OpeningProgress = {}) override;
    bool go(int) override;
    bool goTo(const QString &) override;
    int turnTarget(int, int) const override;
    int page() const override;
    int pages() const override;

    QSize viewport() const override { return size_; }

    QImage image(int) override;
    QJsonObject state() const override;
    QString title() const override;

    QString author() const override { return {}; }

    QString resourceError() const override { return error_; }

    QJsonObject resourceStats() const override;
    void releaseImages() override;

    int cachedPages() const override { return image_.isNull() ? 0 : 1; }

    qint64 cachedImageKey(int selected) const override;
    QStringList fontFaces() override;
    QVector<PageLink> pageLinks() override;
    LinkDetails linkDetails(const QString &, bool = true) override;
    ChapterProgress chapterProgress() const override;

    QVector<ChapterLink> chapters() const override { return chapters_; }

    DocumentWordCounts wordCounts(std::function<bool()> current = {}) override;

    qint64 totalWords() const override { return counts_.total; }

    QVector<ChapterLink> chapterGraph() const override;

    ChapterSelection chapterSelection(const QVector<ChapterLink> &) const override;
    QString chapterTitle(const QString &) const override;
    WordSelection wordAt(QPoint) override;
    AnnotationToken annotationTokenAt(QPoint) override;
    AnnotationToken annotationDragTokenAt(QPoint, const QString &, bool) override;
    AnnotationPageEdges annotationPageEdges() override;
    TextRange annotationRange(const QString &, const QString &) override;
    TextRange annotationRange(const QString &, const QString &, const QString &, const QString &,
                              bool = false, bool = false, std::function<bool()> = {}) override;
    TextRange textRange(const QString &, const QString &) override;
    QVector<QRect> rangeRects(const QString &, const QString &, bool = false) override;
    AnnotationGeometry annotationGeometry(const QString &, const QString &) override;
    QVector<int> intersectingRanges(const QString &, const QString &,
                                    const QVector<QPair<QString, QString>> &) const override;
    QVector<int> visibleRanges(const QVector<QPair<QString, QString>> &) const override;
    bool rangeVisible(const QString &, const QString &) const override;
    QVector<QVector<QRect>> pageRangeRects(int, const QVector<QPair<QString, QString>> &) override;
    SearchBatch searchPage(const QString &, int) override;
    QVector<int> pagesOf(const QStringList &, bool = false, QVector<int> * = nullptr,
                         std::function<bool()> = {}) const override;
    QString pageStart(int) const override;
    QString pageExcerpt(int) const override;

private:
    EngineFontInputs fonts_;
    QString path_, error_, runtime_;
    std::unique_ptr<QProcess> process_;
    QByteArray input_, ready_, processError_;
    QByteArray fingerprint_;
    bool readySeen_ = false;
    unsigned request_ = 0;
    QSize size_;
    QJsonObject appearance_, frame_, position_;
    QImage image_;
    QVector<int> offsets_;
    QVector<bool> linear_;
    QVector<ChapterLink> chapters_;
    QVector<int> chapterGraphOrder_;
    DocumentWordCounts counts_;
    QMap<QString, bool> chapterStartsAtPage_;
    QMap<int, QJsonObject> summaries_;

    struct SourcePoint
    {
        int spine = -1;
        int offset = 0;
        QJsonObject locator;

        QPair<int, int> order() const { return { spine, offset }; }
    };

    QMap<QString, SourcePoint> sourcePoints_;
    int geometryPage_ = -1;
    int geometryRectangles_ = 0;
    QMap<QPair<QString, QString>, AnnotationGeometry> geometries_;

    struct RenderedFrame
    {
        QJsonObject metadata, position;
        QImage image;
    };

    bool opened_ = false;
    bool helperMoved_ = false;
    bool restartableFailure_ = false;
    bool queriesBlocked_ = false;
    std::function<bool()> current_;
    bool start();
    bool ensureOpen();
    bool runOperation(const std::function<bool()> &, bool changesLayout = false);
    bool applyLayout(const QJsonObject &, QSize);
    bool checkPublication();
    bool command(const QByteArray &, RenderedFrame *page = nullptr, QJsonObject *json = nullptr);
    bool render(const QByteArray &);
    QByteArray pageCommand(int) const;
    QJsonObject query(int, int, const QJsonObject &, const QString &fragment = {});
    bool queryPage(int, int, const QJsonObject &, const QString &, QJsonObject &);
    QJsonObject layout(int, const QString &fragment = {});
    QJsonObject summary(int);
    QJsonObject queryRange(const QString &, const QString &);
    int sourceSpine(const QJsonObject &) const;
    QString sourceToken(QJsonObject, int) const;
    QVector<QRect> queryRectangles(const QJsonObject &) const;
    SelectionEndpoint queryEndpoint(const QJsonObject &) const;
    AnnotationToken queryAnnotationToken(const QJsonObject &, int) const;
    QVector<SourcePoint> resolvePositions(const QStringList &);
    QList<AnnotationGeometry> rangeGeometry(int, const QVector<QPair<QString, QString>> &);
    bool readFrame(const QString &, unsigned, RenderedFrame &);
    bool style();
    bool scanBook(bool countWords);
    bool countSpineWords(int index, int page, const QVector<QPair<int, int>> &anchors,
                         QVector<QPair<int, qint64>> &prefixes, qint64 &total);
    bool locate(const QJsonObject &);
    ChapterSelection selectContents(const QVector<ChapterLink> &, const QJsonObject &) const;
    bool fail(QString);
    bool rendererFailed(QString);
    void collectProcessError();
    bool processExited();
    int spine() const;
    int chapterPage() const;
    int chapterPages() const;
};
} // namespace kobalt

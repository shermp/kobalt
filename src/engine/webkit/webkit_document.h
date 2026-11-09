#pragma once
#include "reading/document.h"
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

private:
    EngineFontInputs fonts_;
    QString path_, error_, runtime_;
    std::unique_ptr<QProcess> process_;
    QByteArray input_, ready_;
    bool readySeen_ = false;
    unsigned request_ = 0;
    QSize size_;
    QJsonObject appearance_, frame_;
    QImage image_;
    QVector<int> offsets_;
    QVector<bool> linear_;
    bool opened_ = false;
    std::function<bool()> current_;
    bool start();
    bool ensureOpen();
    bool command(const QByteArray &, bool page = false);
    bool readFrame(const QString &, unsigned);
    bool style();
    bool paginateBook();
    bool fail(QString);
    int spine() const;
    int chapterPage() const;
    int chapterPages() const;
};
} // namespace kobalt

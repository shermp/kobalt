#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)

#  include "engine/query.h"
#  include "engine/links.h"
#  include "engine/types.h"
#  include "publication/location.h"
#  include "publication/web_resources.h"
#  include "platforms/webkit/socket-spike/book-wire.h"

#  include <QImage>
#  include <QObject>
#  include <QSize>
#  include <functional>

class FramePool;

namespace kobalt {
class SourceLease;

namespace webkit {
struct Source
{
    std::shared_ptr<WebResources> resources;
    std::shared_ptr<SourceLease> lease;
    QStringList spine;
    QVector<QByteArray> uris;
};

struct Page
{
    QImage image;
    PageInfo pages;
    Location anchor;
    QVector<PageLink> links;
    spike::Revision revision;
    Restoration restoration = Restoration::Unresolved;
    int spineIndex = 0;
    bool before = false, after = false, boundary = false;
    QString error;
};

class BookClient final : public QObject
{
public:
    using Completion = std::function<void(Page)>;
    using Post = std::function<void()>;

    BookClient(QString executable, Source source, QSize capacity);
    ~BookClient() override;
    void render(QSize size, int fontPixels, int direction, quint64 layout, Completion,
                const Location &target = {}, const QString &fragment = {});
    void accept(const Page &);
    BookClient *recover();

    bool contains(const Location &location) const { return source_.spine.contains(location.href); }

    Location destination(const QString &token, const QByteArray &fingerprint,
                         QString *fragment = nullptr) const;
    void linkDetails(const PageLink &, std::function<void(LinkDetails)>);
    void wordAt(QPoint, std::function<void(WordSelection)>);
    // Close detaches completions immediately. The object deletes itself after
    // the process and the outstanding worker resource request have drained.
    void close();
    static void drain(Post);

private:
    struct Transport;
    void start();
    void receive();
    bool handle(const spike::Message &);
    void send(spike::Type, const std::string & = {});
    void resource(const spike::Message &);
    void resourceChunk(quint64, unsigned);
    void submit();
    void fail(QString);
    void settled();
    void finish(Page);
    void stopProcess();
    void finishWord(WordSelection = {});

    std::unique_ptr<Transport> transport_;
    QString executable_;
    Source source_;
    QSize capacity_;
    std::unique_ptr<FramePool> pool_;
    Completion complete_;
    std::function<void(WordSelection)> word_;
    spike::BookPage candidate_;
    Page displayed_;
    WebResources::Resource reading_;
    quint64 next_ = 0, renderId_ = 0, resourceId_ = 0;
    quint64 wordId_ = 0;
    spike::Revision wordRevision_{};
    QSize wordSize_;
    unsigned resourceOffset_ = 0;
    QSize size_;
    int fontPixels_ = 18, index_ = 0;
    quint64 layout_ = 1;
    bool greeting_ = false, ready_ = false, reserved_ = false, failed_ = false;
    bool closing_ = false, readingPending_ = false, done_ = false;
};
} // namespace webkit
} // namespace kobalt
#endif

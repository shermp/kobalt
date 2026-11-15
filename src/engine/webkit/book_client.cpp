#include "book_client.h"
#include "publication/source_position.h"
#include <QUrl>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#  include "engine/engine_worker.h"
#  include "publication/source_lease.h"
#  include "support/qt_compat.h"
#  include "platforms/webkit/socket-spike/frame-pool.h"
#  include "platforms/webkit/socket-spike/inherited-process.h"

#  include <QCoreApplication>
#  include <QLocalSocket>
#  include <QPointer>
#  include <QTimer>
#  include <algorithm>
#  include <cerrno>
#  include <signal.h>
#  include <sys/socket.h>

namespace kobalt {
namespace webkit {
namespace {
struct CloseQueue
{
    unsigned active = 0;
    QVector<BookClient::Post> waiters;
};

CloseQueue &closingBooks()
{
    static CloseQueue queue;
    return queue;
}

Page errorPage(const QString &error)
{
    Page failure;
    failure.error = error;
    return failure;
}
} // namespace

struct BookClient::Transport
{
    InheritedProcess child;
    QLocalSocket socket;
    QTimer deadline, retry, queryDeadline;
    std::string input;
    int inherited = -1;
    pid_t group = 0;
};

BookClient::BookClient(QString executable, Source source, QSize capacity)
    : QObject(qApp),
      transport_(new Transport),
      executable_(std::move(executable)),
      source_(std::move(source)),
      capacity_(capacity),
      pool_(new FramePool(source_.lease->generation(), capacity.width(), capacity.height()))
{
    auto &io = *transport_;
    io.deadline.setSingleShot(true);
    io.retry.setSingleShot(true);
    io.queryDeadline.setSingleShot(true);
    connect(&io.queryDeadline, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("WebKit word lookup timed out")); });
    connect(&io.retry, &QTimer::timeout, this, [this] { submit(); });
    connect(&io.deadline, &QTimer::timeout, this, [this] {
        if (closing_)
            stopProcess();
        else
            fail(QStringLiteral("WebKit renderer timed out"));
    });
    connect(&io.socket, &QLocalSocket::readyRead, this, [this] { receive(); });
    connect(&io.socket, &QLocalSocket::stateChanged, this,
            [this](QLocalSocket::LocalSocketState state) {
                if (state == QLocalSocket::UnconnectedState && !closing_)
                    fail(QStringLiteral("WebKit renderer disconnected"));
            });
    connect(&io.child, &QProcess::started, this, [this] {
        auto &io = *transport_;
        if (io.inherited >= 0) {
            ::close(io.inherited);
            io.inherited = -1;
        }
#  if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        io.group = pid_t(io.child.pid());
#  else
        io.group = pid_t(io.child.processId());
#  endif
        if (closing_ || failed_)
            return stopProcess();
        greeting_ = true;
        std::string hello;
        spike::put(hello, pool_->generation, 8);
        spike::put(hello, pool_->capacity, 4);
        send(spike::PoolHello, hello);
    });
    connect(&io.child,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                stopProcess();
                if (!closing_)
                    fail(QStringLiteral("WebKit renderer exited"));
                settled();
            });
#  if QT_VERSION < QT_VERSION_CHECK(5, 6, 0)
    connect(&io.child, static_cast<void (QProcess::*)(QProcess::ProcessError)>(&QProcess::error),
#  else
    connect(&io.child, &QProcess::errorOccurred,
#  endif
            this, [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                if (!closing_)
                    fail(QStringLiteral("WebKit renderer could not start"));
                settled();
            });
}

BookClient::~BookClient()
{
    if (transport_->inherited >= 0)
        ::close(transport_->inherited);
}

void BookClient::start()
{
    if (!pool_->valid())
        return fail(QStringLiteral("Could not allocate WebKit frame pool"));
    auto &io = *transport_;
    int descriptors[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors))
        return fail(QStringLiteral("Could not create WebKit control socket"));
    // Qt owns the parent descriptor once it adopts it. Keep audit numbers
    // above the range normally opened during the child dynamic load.
    const int parent = fcntl(descriptors[0], F_DUPFD_CLOEXEC, 80);
    ::close(descriptors[0]);
    io.inherited = descriptors[1];
    const int sentinel = parent >= 0 ? fcntl(parent, F_DUPFD_CLOEXEC, 90) : -1;
    if (parent < 0 || sentinel < 0) {
        if (parent >= 0)
            ::close(parent);
        return fail(QStringLiteral("Could not reserve WebKit descriptors"));
    }
    if (!io.socket.setSocketDescriptor(parent, QLocalSocket::ConnectedState,
                                       QIODevice::ReadWrite)) {
        ::close(sentinel);
        ::close(parent);
        return fail(QStringLiteral("Could not attach WebKit control socket"));
    }
    io.child.passDescriptors({ { io.inherited, pool_->descriptor(0), pool_->descriptor(1) } },
                             true);
    const QStringList args{ QString::number(io.inherited),
                            QString::number(parent),
                            QString::number(sentinel),
                            QString::number(pool_->descriptor(0)),
                            QString::number(pool_->descriptor(1)),
                            QString::number(pool_->capacity) };
    io.child.setProcessChannelMode(QProcess::ForwardedChannels);
    io.child.start(executable_, args);
    ::close(sentinel);
}

Location BookClient::destination(const QString &token, const QByteArray &fingerprint,
                                 QString *fragment) const
{
    const auto anchor = sourcePositionFromToken(token, fingerprint);
    if (anchor.valid())
        return contains(anchor) ? anchor : Location{};
    if (!token.startsWith(QStringLiteral("kobalt-epub://book-"))
        || token.size() > int(spike::maxBookHref + spike::maxFragmentBytes))
        return {};
    auto uri = QUrl::fromEncoded(token.toUtf8(), QUrl::StrictMode);
    if (!uri.isValid() || uri.hasQuery())
        return {};
    const auto id = uri.fragment(QUrl::FullyDecoded);
    if (id.toUtf8().size() > int(spike::maxFragmentBytes) || id.contains(QChar(0)))
        return {};
    uri.setFragment(QString());
    const auto index = source_.uris.indexOf(uri.toEncoded());
    if (index < 0)
        return {};
    if (fragment)
        *fragment = id;
    Location location;
    location.href = source_.spine[index];
    return location;
}

void BookClient::linkDetails(const PageLink &link, std::function<void(LinkDetails)> finished)
{
    LinkDetails details{ LinkKind::Passage, {}, link.source, {}, {}, {} };
    QString fragment;
    const auto location = destination(link.source, {}, &fragment);
    if (link.kind != LinkKind::Footnote || !location.valid() || fragment.isEmpty()) {
        QTimer::singleShot(0, this, [finished, details] { finished(details); });
        return;
    }
    qt::invoke(
            engineWorker(),
            [scope = source_.resources, member = location.href, fragment, finished,
             details]() mutable {
                const auto text = scope->noteText(member, fragment);
                for (const auto &line : text.split(QLatin1Char('\n'))) {
                    const auto paragraph = line.simplified();
                    if (!paragraph.isEmpty())
                        details.html += QStringLiteral("<p>") + paragraph.toHtmlEscaped()
                                + QStringLiteral("</p>");
                }
                if (!details.html.isEmpty())
                    details.kind = LinkKind::Footnote;
                qt::invoke(qApp, [finished, details] { finished(details); }, Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void BookClient::wordAt(QPoint point, std::function<void(WordSelection)> finished)
{
    if (!QRect(QPoint(), size_).contains(point)) {
        finished({});
        return;
    }
    std::string data;
    spike::put(data, unsigned(point.x()), 2);
    spike::put(data, unsigned(point.y()), 2);
    startQuery(spike::BookWordAt, std::move(data),
               [finished](QueryReply reply) { finished(std::move(reply.word)); });
}

void BookClient::rangeRects(const QString &start, const QString &end,
                            std::function<void(QVector<QRect>)> finished)
{
    rangeGeometry(start, end, [finished](AnnotationGeometry geometry) {
        finished(std::move(geometry.rectangles));
    });
}

void BookClient::rangeGeometry(const QString &start, const QString &end,
                               std::function<void(AnnotationGeometry)> finished)
{
    const auto &digest = source_.resources->fingerprint();
    const auto first = sourcePositionFromToken(start, digest);
    const auto last = sourcePositionFromToken(end, digest);
    if (!first.valid() || first.href != displayed_.anchor.href || first.href != last.href
        || first.offset < 0 || last.offset < 0) {
        finished({});
        return;
    }
    const auto encode = [](const Location &location, std::string &bytes) {
        spike::BookAnchor anchor;
        anchor.path.assign(location.path.begin(), location.path.end());
        anchor.offset = location.offset;
        anchor.after = location.after;
        anchor.append(bytes);
    };
    std::string data, firstBytes;
    encode(first, firstBytes);
    spike::put(data, firstBytes.size(), 2);
    data += firstBytes;
    encode(last, data);
    startQuery(spike::BookRangeRects, std::move(data),
               [finished](QueryReply reply) { finished(std::move(reply.range)); });
}

void BookClient::startQuery(spike::Type type, std::string payload,
                            std::function<void(QueryReply)> finished)
{
    if (!ready_ || failed_ || closing_ || complete_ || !displayed_.revision.frame
        || queuedQueries_.size() >= 8) {
        finished({});
        return;
    }
    if (query_) {
        const auto revision = displayed_.revision;
        queuedQueries_.append([this, revision, type, payload = std::move(payload), finished] {
            if (!(revision == displayed_.revision))
                finished({});
            else
                startQuery(type, std::move(payload), finished);
        });
        return;
    }
    query_ = std::move(finished);
    queryId_ = next_ + 1;
    queryReply_ = type == spike::BookWordAt ? spike::BookWordReply : spike::BookRangeReply;
    queryRevision_ = displayed_.revision;
    querySize_ = size_;
    transport_->queryDeadline.start(3000);
    send(type, queryRevision_.encode() + payload);
}

void BookClient::finishQuery(QueryReply reply)
{
    transport_->queryDeadline.stop();
    auto finished = std::move(query_);
    query_ = {};
    auto queued = std::exchange(queuedQueries_, {});
    for (const auto &next : queued)
        next();
    if (finished)
        finished(std::move(reply));
}

void BookClient::searchText(const QString &query, SearchCursor cursor, SearchScope scope,
                            bool first, std::shared_ptr<std::atomic_bool> cancelled,
                            std::function<void(SearchBatch)> finished)
{
    const int last =
            scope == SearchScope::Chapter ? displayed_.spineIndex : source_.spine.size() - 1;
    if (scope == SearchScope::Chapter && first)
        cursor.item = last;
    qt::invoke(
            engineWorker(),
            [scope, source = source_.resources, lease = source_.lease, query, cursor, last,
             cancelled, finished] {
                const auto current = [&] { return !lease->revoked() && !cancelled->load(); };
                if (!current())
                    return;
                SearchBatch result;
                if (scope == SearchScope::Book || scope == SearchScope::Chapter)
                    result = source->searchText(query, cursor, last, current);
                else
                    result.done = result.limited = true;
                if (current())
                    qt::invoke(
                            qApp, [finished, result] { finished(result); }, Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void BookClient::render(QSize size, int fontPixels, int direction, quint64 layout, Completion done,
                        const Location &target, const QString &fragment, bool preview)
{
    if (!failed_ && !closing_ && complete_ && candidate_.preview) {
        if (!preview)
            cancelPreview();
        if (!preview || !queuedRender_) {
            auto &queued = preview ? queuedPreview_ : queuedRender_;
            queued = [this, size, fontPixels, direction, layout, done, target, fragment, preview] {
                render(size, fontPixels, direction, layout, done, target, fragment, preview);
            };
        }
        return;
    }
    if (failed_ || closing_ || complete_) {
        done(errorPage(QStringLiteral("WebKit renderer is unavailable")));
        return;
    }
    size_ = size;
    fontPixels_ = fontPixels;
    layout_ = layout;
    index_ = displayed_.spineIndex;
    candidate_ = {};
    candidate_.preview = preview;
    if (target.valid()) {
        index_ = source_.spine.indexOf(target.href);
        if (index_ < 0) {
            done(errorPage(QStringLiteral("WebKit destination is outside the book spine")));
            return;
        }
        const auto fragmentBytes = fragment.toUtf8();
        candidate_.fragment.assign(fragmentBytes.constData(), fragmentBytes.size());
        candidate_.restore = fragmentBytes.isEmpty();
        candidate_.anchor.offset = target.offset;
        candidate_.anchor.after = target.after;
        for (const auto index : target.path)
            candidate_.anchor.path.push_back(unsigned(index));
    } else if (!displayed_.anchor.href.isEmpty()) {
        const int column = displayed_.pages.chapterPage.value();
        const int columns = displayed_.pages.chapterPages.value();
        if (!direction) {
            candidate_.restore = true;
            candidate_.anchor.offset = displayed_.anchor.offset;
            candidate_.anchor.after = displayed_.anchor.after;
            for (const auto index : displayed_.anchor.path)
                candidate_.anchor.path.push_back(unsigned(index));
        } else if (column + direction >= 0 && column + direction < columns) {
            candidate_.column = unsigned(column + direction);
        } else {
            index_ += direction;
            if (index_ < 0 || index_ >= source_.spine.size()) {
                Page boundary;
                boundary.boundary = true;
                done(std::move(boundary));
                return;
            }
            if (direction < 0)
                candidate_.column = spike::lastColumn;
        }
    }
    complete_ = std::move(done);
    transport_->deadline.start(15000);
    if (ready_)
        submit();
    else if (!greeting_)
        start();
}

void BookClient::submit()
{
    if (closing_ || failed_ || reserved_)
        return;
    if (!spike::validGeometry(size_.width(), size_.height(), pool_->capacity))
        return finish(errorPage(QStringLiteral("Viewport exceeds the WebKit frame pool")));
    if (!pool_->reserve(0, size_.width(), size_.height(), candidate_.frame)) {
        transport_->retry.start(20);
        return;
    }
    reserved_ = true;
    const auto uri = source_.uris.value(index_);
    const auto href = source_.spine.value(index_).toUtf8();
    candidate_.uri.assign(uri.constData(), uri.size());
    candidate_.href.assign(href.constData(), href.size());
    candidate_.revision = { source_.lease->generation(), layout_, next_ + 1 };
    candidate_.fontPixels = unsigned(fontPixels_);
    renderId_ = next_ + 1;
    const auto data = candidate_.encode();
    if (data.size() > spike::maxPayload)
        return fail(QStringLiteral("WebKit spine item path is oversized"));
    send(spike::RenderBookPage, data);
}

void BookClient::send(spike::Type type, const std::string &data)
{
    auto &socket = transport_->socket;
    const auto wire = spike::encode(type, ++next_, data);
    if (socket.bytesToWrite() > 2 * spike::maxPayload
        || socket.write(wire.data(), wire.size()) != qint64(wire.size())) {
        if (closing_)
            stopProcess();
        else
            fail(QStringLiteral("WebKit control write failed"));
    }
}

void BookClient::receive()
{
    auto &io = *transport_;
    while (io.socket.bytesAvailable()) {
        const auto bytes = io.socket.read(4096);
        if (bytes.isEmpty())
            break;
        io.input.append(bytes.constData(), bytes.size());
        spike::Message message{};
        for (;;) {
            const auto parsed = spike::take(io.input, message);
            if (parsed == spike::Parse::Incomplete)
                break;
            if (parsed == spike::Parse::Invalid || !handle(message)) {
                if (closing_)
                    stopProcess();
                else
                    fail(QStringLiteral("Invalid WebKit renderer reply"));
                return;
            }
        }
    }
}

bool BookClient::handle(const spike::Message &message)
{
    if (closing_)
        return true;
    if (!ready_) {
        if (!greeting_ || message.type != spike::Ready || message.id != 1
            || message.payload.size() != 12
            || spike::read(message.payload.data(), 8) != pool_->generation
            || spike::read(message.payload.data() + 8, 4) != pool_->capacity)
            return false;
        ready_ = true;
        submit();
        return true;
    }
    if (message.type == spike::ResourceRequest) {
        resource(message);
        return true;
    }
    if (message.type == spike::BookWordReply) {
        if (!query_ || message.id != queryId_ || message.type != queryReply_)
            return false;
        spike::BookWord result;
        WordSelection word;
        if (!message.payload.empty()) {
            if (!result.decode(message.payload)
                || result.x + result.width > unsigned(querySize_.width())
                || result.y + result.height > unsigned(querySize_.height()))
                return false;
            if (!complete_ && queryRevision_ == displayed_.revision) {
                const auto position = [this](const spike::BookAnchor &anchor) {
                    Location location;
                    location.href = displayed_.anchor.href;
                    for (const auto index : anchor.path)
                        location.path.append(int(index));
                    location.offset = anchor.offset;
                    location.after = anchor.after;
                    return sourcePositionToken(location, source_.resources->fingerprint());
                };
                word = { QString::fromUtf8(result.text.data(), int(result.text.size())),
                         position(result.start),
                         position(result.end),
                         { QRect(result.x, result.y, result.width, result.height) } };
            }
        }
        finishQuery({ std::move(word), {} });
        return true;
    }
    if (message.type == spike::BookRangeReply) {
        const auto size = message.payload.size();
        constexpr size_t header = 14;
        if (!query_ || message.id != queryId_ || message.type != queryReply_
            || (size && (size < header || size > header + 64 * 8 || (size - header) % 8)))
            return false;
        AnnotationGeometry result;
        const QRect viewport(QPoint(), querySize_);
        if (size) {
            const auto decodeEdge = [&](const char *p, SelectionEndpoint &edge) {
                const auto fixed = int(spike::read(p, 2)), first = int(spike::read(p + 2, 2)),
                           last = int(spike::read(p + 4, 2)),
                           direction = int(spike::read(p + 6, 1));
                if (!direction)
                    return !fixed && !first && !last;
                const bool vertical = direction > 2;
                if (direction > 4 || first >= last
                    || fixed > (vertical ? querySize_.height() : querySize_.width())
                    || last > (vertical ? querySize_.width() : querySize_.height()))
                    return false;
                const int outward = direction % 2 ? -1 : 1;
                edge = vertical ? SelectionEndpoint{ { first, fixed },
                                                     { last, fixed },
                                                     { 0, outward },
                                                     true }
                                : SelectionEndpoint{
                                      { fixed, first }, { fixed, last }, { outward, 0 }, true
                                  };
                result.vertical |= vertical;
                return true;
            };
            const auto *p = message.payload.data();
            if (!decodeEdge(p, result.start) || !decodeEdge(p + 7, result.end))
                return false;
        }
        for (size_t i = header; i < size; i += 8) {
            const auto *p = message.payload.data() + i;
            const QRect rect(int(spike::read(p, 2)), int(spike::read(p + 2, 2)),
                             int(spike::read(p + 4, 2)), int(spike::read(p + 6, 2)));
            if (rect.isEmpty() || !viewport.contains(rect))
                return false;
            result.rectangles.append(rect);
        }
        if (complete_ || !(queryRevision_ == displayed_.revision))
            result = {};
        finishQuery({ {}, std::move(result) });
        return true;
    }
    if (!reserved_ || message.id != renderId_)
        return false;
    if (message.type == spike::ErrorReply && message.payload.size() == 4) {
        finish(errorPage(QStringLiteral("WebKit could not render the requested page")));
        return true;
    }
    spike::BookFrame packet;
    if (message.type != spike::BookFrameReady || !packet.decode(message.payload)
        || !(packet.revision == candidate_.revision))
        return false;
    Page page;
    page.revision = packet.revision;
    page.image = pool_->publish(candidate_.frame, message.payload.substr(0, 36));
    if (page.image.isNull())
        return false;
    reserved_ = false;
    const LayoutGeneration generation{ packet.revision.document, packet.revision.layout };
    page.pages.chapterPage = Query<int>::exact(int(packet.column), generation);
    page.pages.chapterPages = Query<int>::exact(int(packet.columns), generation);
    page.pages.bookPage = Query<int>::pending(generation);
    page.pages.bookPages = Query<int>::pending(generation);
    page.pages.bookProgress = Query<double>::pending(generation);
    page.spineIndex = index_;
    page.before = index_ > 0;
    page.after = index_ + 1 < source_.spine.size();
    page.anchor.href = source_.spine.value(index_);
    for (const auto index : packet.anchor.path)
        page.anchor.path.push_back(int(index));
    page.anchor.offset = packet.anchor.offset;
    page.anchor.after = packet.anchor.after;
    page.restoration = Restoration(packet.restoration);
    for (const auto &link : packet.links) {
        const auto token = QString::fromUtf8(link.uri.data(), int(link.uri.size()));
        QString fragment;
        if (!destination(token, {}, &fragment).valid()
            || link.x + link.width > candidate_.frame.width
            || link.y + link.height > candidate_.frame.height)
            continue;
        const QRect rect(link.x, link.y, link.width, link.height);
        const auto kind = link.note && !fragment.isEmpty() ? LinkKind::Footnote : LinkKind::Passage;
        const auto existing =
                std::find_if(page.links.begin(), page.links.end(),
                             [&token](const PageLink &entry) { return entry.source == token; });
        if (existing == page.links.end()) {
            page.links.append({ token, { rect }, kind });
        } else {
            existing->rectangles.append(rect);
            if (kind == LinkKind::Footnote)
                existing->kind = kind;
        }
    }
    finish(std::move(page));
    return true;
}

void BookClient::resource(const spike::Message &message)
{
    if (source_.lease->revoked() || message.payload.size() < 4
        || message.payload.size() > spike::maxBookHref + 4 || readingPending_)
        return fail(QStringLiteral("Invalid WebKit resource request"));
    const auto offset = unsigned(spike::read(message.payload.data(), 4));
    if (offset) {
        if (message.id != resourceId_ || offset != resourceOffset_ || message.payload.size() != 4
            || offset >= unsigned(reading_.bytes.size()))
            return fail(QStringLiteral("Invalid WebKit resource continuation"));
        resourceChunk(message.id, offset);
        return;
    }
    if (message.id <= resourceId_ || message.payload.size() == 4 || !reading_.bytes.isEmpty())
        return fail(QStringLiteral("Invalid WebKit resource sequence"));
    resourceId_ = message.id;
    resourceOffset_ = 0;
    readingPending_ = true;
    const auto scope = source_.resources;
    const auto uri = QByteArray(message.payload.data() + 4, int(message.payload.size() - 4));
    QPointer<BookClient> guard(this);
    qt::invoke(
            engineWorker(),
            [guard, scope, uri, id = resourceId_] {
                auto data = scope->read(uri);
                qt::invoke(
                        qApp,
                        [guard, data = std::move(data), id]() mutable {
                            if (!guard)
                                return;
                            guard->readingPending_ = false;
                            if (guard->closing_)
                                return guard->settled();
                            if (guard->failed_ || guard->source_.lease->revoked())
                                return;
                            guard->reading_ = std::move(data);
                            guard->resourceChunk(id, 0);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void BookClient::resourceChunk(quint64 id, unsigned offset)
{
    const auto size = unsigned(reading_.bytes.size());
    const auto media = offset ? QByteArray() : reading_.mediaType;
    if (size > spike::maxResourceBytes || media.size() > 64)
        return fail(QStringLiteral("Oversized WebKit resource"));
    const auto count = qMin(size - offset, spike::resourceChunk);
    std::string data;
    spike::put(data, id, 8);
    spike::put(data, offset, 4);
    spike::put(data, size, 4);
    spike::put(data, media.size(), 1);
    data.append(media.constData(), media.size());
    data.append(reading_.bytes.constData() + offset, count);
    resourceOffset_ = offset + count;
    if (resourceOffset_ == size)
        reading_ = {};
    send(spike::ResourceData, data);
}

void BookClient::accept(const Page &page)
{
    displayed_ = page;
    displayed_.image = {};
}

BookClient *BookClient::recover()
{
    if (!failed_)
        return this;
    // The failed child may still own writable mappings. The replacement
    // gets new slots and a new socket, even while the old child drains.
    auto *replacement = new BookClient(executable_, source_, capacity_);
    replacement->displayed_ = displayed_;
    close();
    return replacement;
}

void BookClient::finish(Page page)
{
    transport_->deadline.stop();
    transport_->retry.stop();
    if (reserved_) {
        pool_->discard(candidate_.frame);
        reserved_ = false;
    }
    auto callback = std::move(complete_);
    complete_ = {};
    if (callback)
        callback(std::move(page));
    if (!closing_ && !complete_) {
        auto next = std::exchange(queuedRender_, {});
        if (!next)
            next = std::exchange(queuedPreview_, {});
        if (next)
            next();
    }
}

void BookClient::stopProcess()
{
    auto &io = *transport_;
    if (io.group > 0) {
        ::kill(-io.group, SIGKILL);
        io.group = 0;
    }
    if (io.child.state() != QProcess::NotRunning)
        io.child.kill();
}

void BookClient::fail(QString message)
{
    if (failed_ || closing_)
        return;
    failed_ = true;
    // On a protocol failure a reserved slot stays unpublished until the
    // renderer has been stopped. The failed client cannot submit again.
    stopProcess();
    finishQuery();
    finish(errorPage(message));
}

void BookClient::close()
{
    if (closing_)
        return;
    closing_ = true;
    complete_ = {};
    queuedRender_ = {};
    cancelPreview();
    query_ = {};
    queuedQueries_.clear();
    transport_->queryDeadline.stop();
    ++closingBooks().active;
    transport_->retry.stop();
    if (transport_->child.state() != QProcess::NotRunning) {
        transport_->deadline.start(2000);
        if (transport_->socket.state() == QLocalSocket::ConnectedState)
            send(spike::Shutdown);
        else
            stopProcess();
    }
    settled();
}

void BookClient::settled()
{
    if (done_ || !closing_ || readingPending_ || transport_->child.state() != QProcess::NotRunning)
        return;
    done_ = true;
    stopProcess();
    transport_->deadline.stop();
    transport_->socket.abort();
    // WebResources and its cache were used on the worker. Drop our copy there
    // before publishing the end of the drain.
    QPointer<BookClient> guard(this);
    qt::invoke(
            engineWorker(),
            [guard, scope = std::move(source_.resources)]() mutable {
                scope.reset();
                qt::invoke(
                        qApp,
                        [guard] {
                            if (!guard)
                                return;
                            guard->deleteLater();
                            auto &queue = closingBooks();
                            if (--queue.active == 0) {
                                for (const auto &callback : std::exchange(queue.waiters, {}))
                                    callback();
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void BookClient::drain(Post callback)
{
    auto &queue = closingBooks();
    if (queue.active)
        queue.waiters.append(std::move(callback));
    else
        callback();
}
} // namespace webkit
} // namespace kobalt
#endif

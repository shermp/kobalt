#include "frame-pool.h"
#include "render-wire.h"
#include "inherited-process.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtNetwork/QLocalSocket>

#include <cerrno>
#include <functional>
#include <sys/socket.h>

namespace {
using namespace spike;

[[noreturn]] void fail(const char *reason)
{
    qFatal("%s", reason);
}

void require(bool condition, const char *reason)
{
    if (!condition)
        fail(reason);
}

class Session : public QObject
{
public:
    using Reply = std::function<void(const Message &)>;

    Session(FramePool &pool, const QStringList &command, std::function<void()> started,
            Reply received, std::function<void()> stopped)
        : received(std::move(received)), stopped(std::move(stopped))
    {
        int pair[2];
        require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair");
        const int parent = fcntl(pair[0], F_DUPFD_CLOEXEC, 80);
        close(pair[0]);
        require(parent >= 0, "duplicate parent endpoint");
        child = childNumber = pair[1];
        require(socket.setSocketDescriptor(parent, QLocalSocket::ConnectedState, QIODevice::ReadWrite),
                "adopt socket");
        const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        require(fd >= 0, "open unrelated descriptor");
        sentinel = fcntl(fd, F_DUPFD_CLOEXEC, 90);
        close(fd);
        require(sentinel >= 0, "duplicate unrelated descriptor");
        connect(&socket, &QLocalSocket::readyRead, this, [this] { receive(); });
        connect(&socket, &QLocalSocket::disconnected, this, [this] {
            receive();
            require(in.empty(), "truncated child reply");
            eof = true;
            settled();
        });
        connect(&socket, &QLocalSocket::bytesWritten, this, [this](qint64) { flush(); });
        connect(&process, &QProcess::started, this, [this, &pool, started] {
            for (int fd : {child, pool.descriptor(0), pool.descriptor(1)}) {
                const int flags = fcntl(fd, F_GETFD);
                require(flags >= 0 && (flags & FD_CLOEXEC), "parent lost close-on-exec flag");
            }
            close(child);
            child = -1;
            started();
        });
        connect(&process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [this](int status, QProcess::ExitStatus exit) {
            require(killed ? exit == QProcess::CrashExit
                           : (exit == QProcess::NormalExit && status == 0), "unexpected exit");
            reaped = true;
            settled();
        });
        process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        process.passDescriptors({{child, pool.descriptor(0), pool.descriptor(1)}});
        QStringList args = command.mid(1);
        args << QString::number(child) << QString::number(parent) << QString::number(sentinel)
             << QString::number(pool.descriptor(0)) << QString::number(pool.descriptor(1))
             << QString::number(pool.capacity);
        process.start(command[0], args);
    }

    ~Session() override
    {
        if (child >= 0)
            close(child);
        close(sentinel);
    }

    void send(Type type, uint64_t id, const std::string &payload = {})
    {
        out += encode(type, id, payload);
        require(out.size() <= maxPayload, "tx queue overflow");
        flush();
    }

    QSet<int> auditHelpers(const FramePool &pool) const
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        const auto pid = process.pid();
#else
        const auto pid = process.processId();
#endif
        const auto proc = QStringLiteral("/proc/%1").arg(pid);
        const auto control = descriptorTarget(proc, childNumber);
        const auto shmA = descriptorTarget(QStringLiteral("/proc/self"), pool.descriptor(0));
        const auto shmB = descriptorTarget(QStringLiteral("/proc/self"), pool.descriptor(1));
        require(control.startsWith("socket:") && shmA.contains("kobalt-wpe-")
                && shmB.contains("kobalt-wpe-"), "cannot identify inherited resources");
        require(descriptorTarget(proc, pool.descriptor(0)) != shmA
                && descriptorTarget(proc, pool.descriptor(1)) != shmB, "renderer retained SHM FD");

        QSet<int> helpers;
        bool web = false, network = false;
        QDir tasks(proc + QStringLiteral("/task"));
        for (const auto &thread : tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile children(tasks.filePath(thread + QStringLiteral("/children")));
            if (!children.open(QIODevice::ReadOnly))
                continue;
            for (const auto &number : children.readAll().split(' ')) {
                const int childPid = number.toInt();
                if (childPid <= 0 || helpers.contains(childPid))
                    continue;
                const auto childPath = QStringLiteral("/proc/%1").arg(childPid);
                QFile cmdline(childPath + QStringLiteral("/cmdline"));
                if (!cmdline.open(QIODevice::ReadOnly))
                    continue;
                const auto command = cmdline.readAll();
                if (!command.contains("WPEWebProcess") && !command.contains("WPENetworkProcess"))
                    continue;
                helpers.insert(childPid);
                web |= command.contains("WPEWebProcess");
                network |= command.contains("WPENetworkProcess");
                QDir descriptors(childPath + QStringLiteral("/fd"));
                const auto files = descriptors.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
                require(!files.isEmpty(), "cannot inspect WebKit helper descriptors");
                for (const auto &file : files) {
                    const auto target = descriptorTarget(childPath, file.toInt());
                    require(target != control && target != shmA && target != shmB,
                            "launcher descriptor leaked into WebKit helper");
                }
            }
        }
        require(web && network, "did not inspect both WebKit helpers");
        return helpers;
    }

    void kill()
    {
        killed = true;
        process.kill();
    }

private:
    static QByteArray descriptorTarget(const QString &processPath, int fd)
    {
        const auto file = QFile::encodeName(processPath + QStringLiteral("/fd/%1").arg(fd));
        char target[512];
        const auto size = readlink(file.constData(), target, sizeof target);
        return size > 0 ? QByteArray(target, int(size)) : QByteArray();
    }

    void flush()
    {
        if (out.empty())
            return;
        const auto count = socket.write(out.data(), out.size());
        require(count > 0, "socket write");
        out.erase(0, size_t(count));
    }

    void receive()
    {
        while (socket.bytesAvailable()) {
            const auto bytes = socket.read(11);
            require(!bytes.isEmpty(), "socket read");
            in.append(bytes.constData(), bytes.size());
            Message message{};
            for (;;) {
                auto parsed = take(in, message);
                require(parsed != Parse::Invalid, "bad reply");
                if (parsed == Parse::Incomplete)
                    break;
                received(message);
            }
        }
    }

    void settled()
    {
        if (eof && reaped)
            stopped();
    }

    InheritedProcess process;
    QLocalSocket socket;
    Reply received;
    std::function<void()> stopped;
    int child, childNumber, sentinel;
    std::string in, out;
    bool killed = false, eof = false, reaped = false;
};

class Smoke : public QObject
{
public:
    Smoke(QStringList command, bool webkit) : command(std::move(command)), webkit(webkit)
    {
        deferred.setInterval(1);
        connect(&deferred, &QTimer::timeout, this, [this] {
            if (!reserveSlot(0))
                return;
            deferred.stop();
            sendCandidate();
        });
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, this, [] { fail("pool smoke timed out"); });
        deadline.start(10000);
        pool = std::make_unique<FramePool>(1, webkit ? 208 : 160, webkit ? 136 : 120);
        launch();
    }

private:
    enum Phase { FirstRed, Blue, DeferredRed, Cancelling, Resize, Crashing, RestartBlue, ResizedRed };
    enum Event { Handshake, Partial, Publication, Cancellation, CancelAcknowledged, QueryRollback, StaleQuery, CloseReply, Goodbye, Exit };

    void check(const QImage &image, uint32_t color) const
    {
        require(!image.isNull(), "missing frame");
        const auto pixel = color ? qRgb(0, 0, 255) : qRgb(255, 0, 0);
        for (int y = 0; y < image.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                require(row[x] == (webkit && x < 32 && y < 32 ? qRgb(255, 255, 255) : pixel),
                        "torn or changed pixel");
        }
    }

    void launch()
    {
        require(pool->valid(), "POSIX SHM allocation");
        request = 0;
        revision = {};
        event = Handshake;
        session = std::make_unique<Session>(
                *pool, command,
                [this] {
                    hello.clear();
                    put(hello, pool->generation, 8);
                    put(hello, pool->capacity, 4);
                    session->send(PoolHello, ++request, hello);
                },
                [this](const Message &msg) { receive(msg); },
                [this] {
                    // Let Qt finish emitting completion before replacing its sender.
                    QTimer::singleShot(0, this, [this] { stopped(); });
                });
    }

    bool reserveSlot(uint32_t color)
    {
        if (!webkit)
            return pool->reserve(color, candidate);
        const bool large = phase == Cancelling || phase == Resize || phase == ResizedRed;
        return pool->reserve(color, large ? 208 : 160, large ? 136 : 120, candidate);
    }

    void reserve(uint32_t color)
    {
        require(reserveSlot(color), "no writable slot");
        sendCandidate();
    }

    void sendCandidate()
    {
        event = Partial;
        frameRequest = ++request;
        if (!webkit)
            return session->send(Frame, frameRequest, candidate.encode());
        if (phase == Crashing) {
            event = Exit;
            return session->kill();
        }
        Type method = Open;
        if (phase == Blue)
            method = Turn;
        else if (phase == DeferredRed)
            method = Navigate;
        else if (phase == Cancelling || phase == Resize)
            method = SetViewport;
        event = phase == Cancelling ? CancelAcknowledged : Publication;
        expectedRevision = method == Open ? Revision{1, 1, 1}
                                           : Revision{revision.document, revision.layout + (phase == Resize),
                                                      revision.frame + 1};
        session->send(method, frameRequest, candidate.encode() + revision.encode());
        if (phase == Cancelling)
            session->send(Cancel, ++request, targetRequest(frameRequest));
    }

    void receive(const Message &msg)
    {
        std::string target;
        switch (event) {
        case Handshake:
            require(msg.type == Ready && msg.id == request && msg.payload == hello, "pool handshake");
            reserve(phase == RestartBlue);
            break;
        case Partial:
            require(msg.type == HalfWritten && msg.id == frameRequest && msg.payload.empty(),
                    "expected write barrier");
            if (phase == Crashing) {
                event = Exit;
                session->kill();
            } else {
                put(target, frameRequest, 8);
                event = phase == Cancelling ? Cancellation : Publication;
                session->send(phase == Cancelling ? Cancel : Finish, ++request, target);
            }
            break;
        case Publication: {
            auto bad = msg;
            require(msg.payload.size() == (webkit ? 60u : 36u), "wrong frame payload size");
            ++bad.id;
            require(frameFrom(bad).isNull(), "accepted stale request");
            bad = msg;
            bad.payload[0] ^= 1;
            require(frameFrom(bad).isNull(), "accepted stale generation");
            bad = msg;
            bad.payload[24] ^= 1;
            require(frameFrom(bad).isNull(), "accepted bad stride");
            bad = msg;
            bad.payload.pop_back();
            require(frameFrom(bad).isNull(), "accepted truncated frame metadata");
            if (webkit) {
                bad = msg;
                bad.payload[52] ^= 1;
                require(frameFrom(bad).isNull(), "accepted stale frame revision");
            }
            auto image = frameFrom(msg);
            check(image, candidate.color);
            require(frameFrom(msg).isNull(), "accepted duplicate publication");
            revision = expectedRevision;
            published(std::move(image));
            break;
        }
        case Cancellation:
            if (webkit) {
                require(msg.type == ErrorReply && msg.id == frameRequest && msg.payload.size() == 4
                        && read(msg.payload.data(), 4) == uint32_t(RenderError::Cancelled),
                        "cancelled render published or lost its terminal result");
                pool->discard(candidate);
                check(red, 0);
                event = QueryRollback;
                session->send(Query, ++request, revision.encode());
                return;
            }
            require(msg.type == Cancelled && msg.id == frameRequest && msg.payload.empty(),
                    "missing cancellation result");
            event = CancelAcknowledged;
            break;
        case CancelAcknowledged:
            if (webkit) {
                require(msg.type == CancelAck && msg.id == request
                        && msg.payload == std::string(1, '\1'), "cancel did not win");
                event = Cancellation;
                return;
            }
            require(msg.type == CancelAck && msg.id == request && msg.payload.empty(),
                    "missing cancellation acknowledgement");
            pool->discard(candidate);
            check(red, 0);
            phase = Crashing;
            reserve(1);
            break;
        case QueryRollback: {
            auto expected = candidate;
            expected.width = 160;
            expected.height = 120;
            require(msg.type == StateReply && msg.id == request
                    && msg.payload == stateReply(revision, expected), "layout advanced on cancellation");
            event = StaleQuery;
            auto stale = revision;
            --stale.frame;
            session->send(Query, ++request, stale.encode());
            break;
        }
        case StaleQuery:
            require(msg.type == ErrorReply && msg.id == request && msg.payload.size() == 4
                    && read(msg.payload.data(), 4) == uint32_t(RenderError::Stale), "accepted stale state");
            phase = Resize;
            reserve(0);
            break;
        case CloseReply:
            require(msg.type == Closed && msg.id == request && msg.payload.empty(), "missing close reply");
            event = Goodbye;
            session->send(Shutdown, ++request);
            break;
        case Goodbye:
            require(msg.type == Bye && msg.id == request && msg.payload.empty(), "missing goodbye");
            event = Exit;
            break;
        case Exit:
            fail("reply after termination");
        }
    }

    QImage frameFrom(const Message &msg)
    {
        if (msg.type != FrameReady || msg.id != frameRequest
            || msg.payload.size() != (webkit ? 60u : 36u))
            return {};
        if (webkit && !(Revision::decode(msg.payload.data() + 36) == expectedRevision))
            return {};
        return pool->publish(candidate, msg.payload.substr(0, 36));
    }

    void published(QImage image)
    {
        if (webkit)
            helpers.unite(session->auditHelpers(*pool));
        switch (phase) {
        case FirstRed:
            red = std::move(image);
            phase = Blue;
            reserve(1);
            break;
        case Blue: {
            blue = std::move(image);
            QImage detached = red;
            detached.setPixel(0, 0, qRgb(0, 255, 0));
            require(detached.pixel(0, 0) == qRgb(0, 255, 0), "detach failed");
            check(red, 0);
            QImage painted(red.size(), red.format());
            QPainter painter(&painted);
            painter.drawImage(0, 0, red);
            painter.end();
            check(painted, 0);
            FrameSpec unused;
            require(!pool->reserve(0, unused), "overwrote a leased slot");
            phase = DeferredRed;
            // Hold a second copy after dropping the original. The deferred request
            // can resume only when the final copy is released.
            heldBlue = blue;
            blue = {};
            require(!pool->reserve(0, unused), "released a slot with a live copy");
            deferred.start();
            QTimer::singleShot(0, this, [this] {
                check(heldBlue, 1);
                heldBlue = {};
            });
            break;
        }
        case DeferredRed:
            check(red, 0);
            image = {};
            phase = Cancelling;
            reserve(webkit ? 0 : 1);
            break;
        case Resize:
            require(image.size() == QSize(208, 136) && revision.layout == 2,
                    "resize did not commit a new layout");
            check(red, 0);
            image = {};
            phase = Crashing;
            reserve(1);
            break;
        case RestartBlue:
            check(red, 0);
            blue = std::move(image);
            event = Goodbye;
            session->send(Shutdown, ++request);
            break;
        case ResizedRed:
            require(image.size() == QSize(208, 136), "bad resized geometry");
            check(red, 0);
            check(blue, 1);
            event = webkit ? CloseReply : Goodbye;
            if (webkit)
                session->send(Close, ++request, revision.encode());
            else
                session->send(Shutdown, ++request);
            break;
        default:
            fail("unexpected publication");
        }
    }

    void stopped()
    {
        require(event == Exit, "renderer exited before expected");
        session.reset();
        waitForHelpers();
    }

    void waitForHelpers()
    {
        for (int pid : helpers) {
            if (QFile::exists(QStringLiteral("/proc/%1").arg(pid))) {
                QTimer::singleShot(10, this, [this] { waitForHelpers(); });
                return;
            }
        }
        helpers.clear();
        rendererStopped();
    }

    void rendererStopped()
    {
        if (phase == Crashing) {
            check(red, 0);
            pool->discard(candidate);
            phase = RestartBlue;
            launch();
        } else if (phase == RestartBlue) {
            retired = {{pool->descriptor(0), pool->descriptor(1)}};
            pool = std::make_unique<FramePool>(2, 208, 136);
            phase = ResizedRed;
            launch();
        } else {
            require(phase == ResizedRed, "unexpected exit phase");
            check(red, 0);
            check(blue, 1);
            const int newFds[] = {pool->descriptor(0), pool->descriptor(1)};
            pool.reset();
            red = {};
            blue = {};
            for (int fd : {retired[0], retired[1], newFds[0], newFds[1]})
                require(fcntl(fd, F_GETFD) < 0 && errno == EBADF, "SHM descriptor leaked");
            qDebug("Qt %s pool: pixels, leases, cancellation, restart and retirement passed",
                   qVersion());
            QCoreApplication::exit(0);
        }
    }

    const QStringList command;
    const bool webkit;
    std::unique_ptr<FramePool> pool;
    std::unique_ptr<Session> session;
    QTimer deferred, deadline;
    Phase phase = FirstRed;
    Event event = Handshake;
    uint64_t request = 0, frameRequest = 0;
    FrameSpec candidate;
    Revision revision, expectedRevision;
    std::string hello;
    QImage red, blue, heldBlue;
    std::array<int, 2> retired{};
    QSet<int> helpers;
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto command = app.arguments().mid(1);
    const bool webkit = !command.isEmpty() && command[0] == QStringLiteral("--webkit");
    if (webkit)
        command.removeFirst();
    if (command.isEmpty())
        return 2;
    Smoke smoke(command, webkit);
    return app.exec();
}

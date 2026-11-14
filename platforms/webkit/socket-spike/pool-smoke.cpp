#include "frame-pool.h"
#include "inherited-process.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
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
        child = pair[1];
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

    void kill()
    {
        killed = true;
        process.kill();
    }

private:
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
    int child, sentinel;
    std::string in, out;
    bool killed = false, eof = false, reaped = false;
};

class Smoke : public QObject
{
public:
    explicit Smoke(QStringList command) : command(std::move(command))
    {
        deferred.setInterval(1);
        connect(&deferred, &QTimer::timeout, this, [this] {
            if (!pool->reserve(0, candidate))
                return;
            deferred.stop();
            sendCandidate();
        });
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, this, [] { fail("pool smoke timed out"); });
        deadline.start(10000);
        pool = std::make_unique<FramePool>(1, 160, 120);
        launch();
    }

private:
    enum Phase { FirstRed, Blue, DeferredRed, Cancelling, Crashing, RestartBlue, ResizedRed };
    enum Event { Handshake, Partial, Publication, Cancellation, CancelAcknowledged, Goodbye, Exit };

    static void check(const QImage &image, uint32_t color)
    {
        require(!image.isNull(), "missing frame");
        const auto pixel = color ? qRgb(0, 0, 255) : qRgb(255, 0, 0);
        for (int y = 0; y < image.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                require(row[x] == pixel, "torn or changed pixel");
        }
    }

    void launch()
    {
        require(pool->valid(), "POSIX SHM allocation");
        request = 0;
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

    void reserve(uint32_t color)
    {
        require(pool->reserve(color, candidate), "no writable slot");
        sendCandidate();
    }

    void sendCandidate()
    {
        event = Partial;
        frameRequest = ++request;
        session->send(Frame, frameRequest, candidate.encode());
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
            require(msg.type == FrameReady && msg.id == frameRequest, "wrong frame reply");
            // Reject stale generation and invalid stride without releasing the reservation.
            auto malformed = msg.payload;
            require(malformed.size() == 36, "wrong frame payload size");
            malformed[0] ^= 1;
            require(pool->publish(candidate, malformed).isNull(), "accepted stale generation");
            malformed = msg.payload;
            malformed[24] ^= 1;
            require(pool->publish(candidate, malformed).isNull(), "accepted bad stride");
            auto image = pool->publish(candidate, msg.payload);
            check(image, candidate.color);
            published(std::move(image));
            break;
        }
        case Cancellation:
            require(msg.type == Cancelled && msg.id == frameRequest && msg.payload.empty(),
                    "missing cancellation result");
            event = CancelAcknowledged;
            break;
        case CancelAcknowledged:
            require(msg.type == CancelAck && msg.id == request && msg.payload.empty(),
                    "missing cancellation acknowledgement");
            pool->discard(candidate);
            check(red, 0);
            phase = Crashing;
            reserve(1);
            break;
        case Goodbye:
            require(msg.type == Bye && msg.id == request && msg.payload.empty(), "missing goodbye");
            event = Exit;
            break;
        case Exit:
            fail("reply after termination");
        }
    }

    void published(QImage image)
    {
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
            event = Goodbye;
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
    std::unique_ptr<FramePool> pool;
    std::unique_ptr<Session> session;
    QTimer deferred, deadline;
    Phase phase = FirstRed;
    Event event = Handshake;
    uint64_t request = 0, frameRequest = 0;
    FrameSpec candidate;
    std::string hello;
    QImage red, blue, heldBlue;
    std::array<int, 2> retired{};
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto command = app.arguments().mid(1);
    if (command.isEmpty())
        return 2;
    Smoke smoke(command);
    return app.exec();
}

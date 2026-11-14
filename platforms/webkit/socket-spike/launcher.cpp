#include "wire.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalSocket>

#include <algorithm>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace spike;

void inheritControl(int descriptor)
{
    const int flags = fcntl(descriptor, F_GETFD);
    if (flags < 0 || fcntl(descriptor, F_SETFD, flags & ~FD_CLOEXEC) < 0)
        _exit(126);
}

class Process final : public QProcess
{
public:
    void passControl(int fd)
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        control = fd;
#else
        setChildProcessModifier([fd] { inheritControl(fd); });
#endif
    }

private:
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void setupChildProcess() override
    {
        if (control >= 0)
            inheritControl(control);
    }
    int control = -1;
#endif
};

class Smoke final : public QObject
{
public:
    explicit Smoke(QString mode) : mode(std::move(mode)) {}
    ~Smoke() override
    {
        if (childFd >= 0)
            close(childFd);
        if (sentinel >= 0)
            close(sentinel);
    }
    bool start(const QString &program, QStringList args);

private:
    void queue(std::string bytes);
    void pump();
    void receive();
    void received(const Message &);
    void checkDone();
    void fail(const char *);

    const QString mode;
    Process process;
    QLocalSocket socket;
    int childFd = -1, sentinel = -1;
    std::string in, out;
    int step = 0;
    bool eof = false, reaped = false, finished = false, halfClosed = false;
};

bool Smoke::start(const QString &program, QStringList args)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
        return false;
    // Audit two unrelated high-numbered FDs. The child can check their absence
    // without confusing them with low descriptors opened by runtime startup.
    const int parent = fcntl(pair[0], F_DUPFD_CLOEXEC, 80);
    close(pair[0]);
    childFd = pair[1];
    const int unrelated = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (unrelated >= 0) {
        sentinel = fcntl(unrelated, F_DUPFD_CLOEXEC, 90);
        close(unrelated);
    }
    if (parent < 0 || sentinel < 0
        || !socket.setSocketDescriptor(parent, QLocalSocket::ConnectedState, QIODevice::ReadWrite)) {
        if (parent >= 0)
            close(parent);
        return false;
    }
    const int parentFlags = fcntl(parent, F_GETFD);
    if (parentFlags < 0 || !(parentFlags & FD_CLOEXEC))
        return false;

    connect(&socket, &QLocalSocket::readyRead, this, [this] { receive(); });
    connect(&socket, &QLocalSocket::bytesWritten, this, [this](qint64) { pump(); });
    connect(&socket, &QLocalSocket::disconnected, this, [this] {
        receive();
        eof = true;
        checkDone();
    });
    connect(&process, &QProcess::started, this, [this] {
        const int flags = fcntl(childFd, F_GETFD);
        close(childFd);
        childFd = -1;
        if (flags < 0 || !(flags & FD_CLOEXEC))
            return fail("Child inheritance changed the parent's descriptor flags");
        if (mode == QStringLiteral("missing-fd"))
            return;
        auto hello = encode(Hello, 1);
        if (mode == QStringLiteral("version"))
            hello[4] = 2;
        if (mode == QStringLiteral("oversize"))
            hello[9] = 0x20;
        if (mode == QStringLiteral("truncated"))
            hello.resize(9);
        queue(std::move(hello));
    });
    connect(&process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
        reaped = true;
        if (mode == QStringLiteral("kill")) {
            if (step != 2 || status != QProcess::CrashExit)
                return fail("Expected a crash while work was pending");
        } else {
            int expected = 3;
            if (mode == QStringLiteral("normal"))
                expected = 0;
            else if (mode == QStringLiteral("missing-fd"))
                expected = 2;
            if (status != QProcess::NormalExit || code != expected)
                return fail("Unexpected child exit status");
        }
        checkDone();
    });
    QTimer::singleShot(10000, this, [this] { fail("Socket spike timed out"); });
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    if (mode != QStringLiteral("missing-fd"))
        process.passControl(childFd);
    args << QString::number(childFd) << QString::number(parent) << QString::number(sentinel);
    process.start(program, args);
    return true;
}

void Smoke::queue(std::string bytes)
{
    out += bytes;
    pump();
}

void Smoke::pump()
{
    if (socket.bytesToWrite() || finished)
        return;
    if (!out.empty()) {
        const auto accepted = socket.write(out.data(), std::min<size_t>(out.size(), 13));
        if (accepted <= 0)
            return fail("Socket write failed");
        out.erase(0, size_t(accepted));
    } else if (mode == QStringLiteral("truncated") && !halfClosed) {
        halfClosed = true;
        if (::shutdown(int(socket.socketDescriptor()), SHUT_WR) != 0)
            fail("Truncation half-close failed");
    }
}

void Smoke::receive()
{
    while (socket.bytesAvailable()) {
        const auto bytes = socket.read(11);
        if (bytes.isEmpty())
            return fail("Socket read failed");
        in.append(bytes.constData(), size_t(bytes.size()));
        Message message{};
        for (;;) {
            const auto parsed = take(in, message);
            if (parsed == Parse::Incomplete)
                break;
            if (parsed == Parse::Invalid)
                return fail("Malformed child reply");
            received(message);
            if (finished)
                return;
        }
    }
}

void Smoke::received(const Message &message)
{
    if (!message.payload.empty() || (mode != QStringLiteral("normal") && mode != QStringLiteral("kill")))
        return fail("Unexpected reply");
    ++step;
    if (step == 1 && message.type == Ready && message.id == 1) {
        queue(encode(Work, 2, std::string(maxPayload, 'w')));
    } else if (step == 2 && message.type == Pending && message.id == 2) {
        if (mode == QStringLiteral("kill")) {
            process.kill();
        } else {
            std::string target;
            put(target, 2, 8);
            queue(encode(Cancel, 3, target));
        }
    } else if (step == 3 && message.type == Cancelled && message.id == 2) {
        return;
    } else if (step == 4 && message.type == CancelAck && message.id == 3) {
        queue(encode(Shutdown, 4));
    } else if (!(step == 5 && message.type == Bye && message.id == 4)) {
        fail("Unexpected reply type or request ID");
    }
}

void Smoke::checkDone()
{
    if (!eof || !reaped || finished)
        return;
    const int expected = mode == QStringLiteral("normal") ? 5 : mode == QStringLiteral("kill") ? 2 : 0;
    if (!in.empty() || step != expected)
        return fail("EOF before expected replies completed");
    finished = true;
    qDebug("Qt %s %s passed: socket EOF and child reaped", qVersion(), qPrintable(mode));
    QCoreApplication::exit(0);
}

void Smoke::fail(const char *reason)
{
    if (finished)
        return;
    finished = true;
    qCritical("%s: %s", reason, qPrintable(process.errorString()));
    if (process.state() == QProcess::NotRunning) {
        QCoreApplication::exit(1);
    } else {
        connect(&process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [](int, QProcess::ExitStatus) { QCoreApplication::exit(1); });
        process.kill();
    }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 3 || !QStringList{QStringLiteral("normal"), QStringLiteral("kill"),
                                      QStringLiteral("version"), QStringLiteral("oversize"),
                                      QStringLiteral("truncated"), QStringLiteral("missing-fd")}.contains(args[1]))
        return 2;
    Smoke smoke(args[1]);
    if (!smoke.start(args[2], args.mid(3))) {
        qCritical("Could not create or adopt the close-on-exec socketpair");
        return 1;
    }
    return app.exec();
}

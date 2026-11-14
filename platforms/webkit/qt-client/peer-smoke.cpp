#include "peer.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QDebug>
#include <QtDBus/QDBusError>
#include <csignal>
#include <unistd.h>

namespace {
class Smoke
{
public:
    Smoke(const QDBusConnection &connection, const QString &directory, int crashPid)
        : peer(connection, directory), connection(connection), crashPid(crashPid)
    {
    }

    void start()
    {
        peer.hello([this](RendererPeer::Result hello) {
            if (!ok(hello))
                return;
            started.start();
            peer.open(QStringLiteral("fixture"), QStringLiteral("color-pages"), {}, QSize(160, 120),
                      {}, {}, [this](RendererPeer::Result opened) {
                if (!check(opened, 1, qRgb(255, 0, 0)))
                    return;
                red = opened.frame;
                peer.open(QStringLiteral("fixture"), QStringLiteral("color-pages"), {},
                          QSize(1024, 1024), {}, {}, [this](RendererPeer::Result cancelled) {
                    if (!cancelled.error.endsWith(QStringLiteral(".Cancelled"))
                        || !cancelled.frame.isNull()) {
                        qCritical() << "Expected cancellation" << cancelled.error;
                        return QCoreApplication::exit(1);
                    }
                    turn();
                });
                peer.cancel([](RendererPeer::Result result) {
                    if (!ok(result) || !result.cancelled)
                        QCoreApplication::exit(1);
                });
            });
        });
    }

private:
    void turn()
    {
        peer.turn(1, [this](RendererPeer::Result blue) {
            if (!check(blue, 2, qRgb(0, 0, 255)))
                return;
            blueFrame = blue.frame;
            peer.navigate(QStringLiteral("start"), {}, [this](RendererPeer::Result again) {
                if (!check(again, 3, qRgb(255, 0, 0)))
                    return;
                qDebug("fixture sequence ms=%lld", static_cast<long long>(started.elapsed()));
                if (crashPid) {
                    if (kill(crashPid, SIGKILL) != 0)
                        return QCoreApplication::exit(1);
                    disconnectPoll.setInterval(10);
                    QObject::connect(&disconnectPoll, &QTimer::timeout, &disconnectPoll, [this] {
                        if (!connection.isConnected()) {
                            disconnectPoll.stop();
                            finish();
                        }
                    });
                    disconnectPoll.start();
                    return;
                }
                peer.shutdown([this](RendererPeer::Result closed) {
                    if (ok(closed))
                        finish();
                });
            });
        });
    }

    void finish()
    {
        QImage copy = red;
        copy.bits()[0] = 0;
        if (red.pixel(0, 0) != qRgb(255, 255, 255)
            || red.pixel(40, 40) != qRgb(255, 0, 0)
            || blueFrame.pixel(40, 40) != qRgb(0, 0, 255)) {
            qCritical("Retained frames changed after renderer exit or copy detachment");
            return QCoreApplication::exit(1);
        }
        qDebug("Qt %s transfer, cancellation, pixels and retained mappings passed", qVersion());
        QCoreApplication::quit();
    }

    static bool ok(const RendererPeer::Result &result)
    {
        if (result.error.isEmpty())
            return true;
        qCritical() << qVersion() << result.error;
        QCoreApplication::exit(1);
        return false;
    }

    static bool check(const RendererPeer::Result &result, qulonglong revision, QRgb background)
    {
        if (!ok(result))
            return false;
        if (result.document == 1 && result.layout == 1 && result.revision == revision
            && result.frame.size() == QSize(160, 120)
            && result.frame.pixel(0, 0) == qRgb(255, 255, 255)
            && result.frame.pixel(40, 40) == background)
            return true;
        qCritical("Unexpected frame pixels or identity");
        QCoreApplication::exit(1);
        return false;
    }

    RendererPeer peer;
    QDBusConnection connection;
    QTimer disconnectPoll;
    QElapsedTimer started;
    QImage red, blueFrame;
    int crashPid;
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2 || argc > 4)
        return 2;
    // Standalone bootstrap before the event loop. The client itself accepts an
    // already established peer, so a GUI caller can bootstrap off-thread.
    const auto peer = QDBusConnection::connectToPeer(QString::fromLocal8Bit(argv[1]),
                                                     QStringLiteral("renderer-smoke"));
    if (!peer.isConnected()) {
        qCritical() << peer.lastError().message();
        return 1;
    }
    bool validPid = false;
    const int crashPid = argc == 4 ? QString::fromLocal8Bit(argv[3]).toInt(&validPid) : 0;
    if (argc == 4 && (!validPid || crashPid <= 1))
        return 2;
    Smoke smoke(peer, argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString(), crashPid);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&app] {
        qCritical("Renderer1 smoke deadline exceeded");
        app.exit(1);
    });
    deadline.start(40000);
    smoke.start();
    return app.exec();
}

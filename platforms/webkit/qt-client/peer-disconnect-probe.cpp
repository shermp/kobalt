#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>

#include <csignal>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 4)
        return 2;
    bool validPid = false;
    const int pid = QString::fromLocal8Bit(argv[3]).toInt(&validPid);
    if (!validPid || pid <= 1)
        return 2;

    const auto connection = QDBusConnection::connectToPeer(
            QString::fromLocal8Bit(argv[1]), QStringLiteral("renderer-disconnect-probe"));
    if (!connection.isConnected())
        return 1;

    QTimer deadline, poll;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&app] {
        qCritical("Peer disconnect probe timed out");
        app.exit(1);
    });
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (connection.isConnected())
            return;
        poll.stop();
        qDebug("Qt %s observed abrupt disconnect without receiving any frame", qVersion());
        app.quit();
    });

    auto hello = QDBusMessage::createMethodCall(QString(), QStringLiteral("/org/kobalt/RendererShm1"),
                                               QStringLiteral("org.kobalt.RendererShm1"),
                                               QStringLiteral("Hello"));
    hello.setArguments(QVariantList{1u});
    QDBusPendingCallWatcher watcher(connection.asyncCall(hello, 5000));
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &app, [&] {
        const auto reply = watcher.reply();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QStringLiteral("ua{sv}")
            || reply.arguments().value(0).toUInt() != 1) {
            qCritical("Probe Hello failed");
            app.exit(1);
            return;
        }
        qDebug("Qt %s Hello passed, killing renderer before Open", qVersion());
        if (::kill(pid, SIGKILL) != 0) {
            qCritical("Probe failed to kill its renderer");
            app.exit(1);
            return;
        }
        poll.start(10);
    });
    deadline.start(10000);
    const int status = app.exec();
    QDBusConnection::disconnectFromPeer(connection.name());
    return status;
}

#pragma once

#include <QtCore/QObject>
#include <QtCore/QVariant>
#include <QtDBus/QDBusConnection>
#include <QtGui/QImage>

#include <functional>

class RendererPeer final : public QObject
{
public:
    struct Result
    {
        QString error;
        qulonglong document = 0, layout = 0, revision = 0;
        QVariantMap state;
        QImage frame;
        bool cancelled = false;
    };
    using Completion = std::function<void(Result)>;

    // Supply a private peer established by the launcher outside the GUI thread.
    // A RendererPeer is single-session and all methods run on its owning thread.
    explicit RendererPeer(const QDBusConnection &connection, const QString &namedDirectory = {});
    ~RendererPeer() override;

    void hello(Completion);
    void open(const QString &kind, const QString &source, const QString &fingerprint,
              const QSize &, const QVariantMap &appearance, const QString &token, Completion);
    void turn(int delta, Completion);
    void navigate(const QString &kind, const QString &target, Completion);
    void setViewport(const QSize &, Completion);
    void setAppearance(const QVariantMap &, Completion);
    void query(const QString &kind, const QVariantMap &arguments, Completion);
    void close(Completion);
    void cancel(Completion);
    void shutdown(Completion);

private:
    enum class Reply { Hello, Open, Visual, Query, Close, Cancel, Shutdown };
    void foreground(const char *, const QVariantList &, Reply, Completion);
    void send(const char *, const QVariantList &, Reply, qulonglong, Completion, const QSize &expected = {});
    Result decode(const QDBusMessage &, Reply, qulonglong, const QSize &);
    void retire();
    void reject(Completion, const QString &);

    QDBusConnection peer;
    const QString namedDirectory;
    qulonglong nextRequest = 0, pending = 0, latestDocument = 0;
    qulonglong document = 0, layout = 0, revision = 0;
    QSize viewport;
    uint maxWidth = 0, maxHeight = 0, maxBytes = 0;
    bool ready = false, greeted = false, retired = false, closing = false;
};

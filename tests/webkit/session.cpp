#include "check.h"
#include "publication/local_resources.h"
#include "reading/reader_session.h"
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>

using namespace kobalt;

namespace {
QByteArray digest(const QImage &image)
{
    return QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(image.constBits()),
                                               image.bytesPerLine() * image.height()),
                                    QCryptographicHash::Sha256)
            .toHex();
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 3, "Expected accepted local EPUB and output directory");
    const auto path = QString::fromLocal8Bit(argv[1]);
    const auto output = QString::fromLocal8Bit(argv[2]);
    require(QDir().mkpath(output), "Cannot create output directory");
    QTimer::singleShot(0, &app, [&] {
        QEventLoop completion;
        QTimer deadline, heartbeat;
        int beats = 0;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++beats; });
        heartbeat.start(10);
        QObject::connect(&deadline, &QTimer::timeout, &app,
                         [] { require(false, "Firmware session operation timed out"); });
        deadline.setSingleShot(true);
        deadline.start(240000);
        auto lease = std::make_shared<SourceLease>();
        auto session = std::make_unique<ReaderSession>(EngineFontInputs{}, "serif");
        OpenRequest request{ "file://" + path,
                             path,
                             { 600, 800 },
                             {},
                             {},
                             {},
                             lease,
                             localResourceFactory(path, lease) };
        request.backend = "webkit";
        session->open(request, [&](OpeningResult result) {
            require(result.ready(), "Firmware ReaderSession opening");
            require(session->experimentalBackend(), "Firmware ReaderSession must use WebKit");
            completion.quit();
        });
        completion.exec();
        require(beats > 0, "GUI event loop continues during opening");
        require(session->pages() > 1 && !session->image().isNull(), "CPU page and pagination");
        const auto firstPage = session->page();
        const auto first = digest(session->image());
        require(session->image().save(output + "/first.png"), "Capture first reader frame");
        session->updated = [&] { completion.quit(); };
        require(session->turn(1), "Firmware forward turn queued");
        completion.exec();
        require(session->error().isEmpty() && session->page() > firstPage, "Firmware forward turn");
        require(digest(session->image()) != first, "Turn supplies new CPU pixels");
        require(session->image().save(output + "/next.png"), "Capture next reader frame");
        require(session->turn(-1), "Firmware backward turn queued");
        completion.exec();
        require(session->error().isEmpty() && digest(session->image()) == first,
                "Stable return frame");
        session->resize({ 800, 600 });
        session->resize({ 700, 900 });
        completion.exec();
        require(session->error().isEmpty() && session->image().size() == QSize(700, 900),
                "Stale resize result discarded");
        session->suspend();
        ReaderSession::drain([&] { completion.quit(); });
        completion.exec();
        require(session->turn(1), "Resume turn queued");
        completion.exec();
        require(session->error().isEmpty() && !session->image().isNull(),
                "Helper restarts after sleep");
        require(!session->state().contains("position"), "No provisional persistent position");
        QFile report(output + "/session.json");
        require(report.open(QIODevice::WriteOnly), "Open firmware report");
        report.write(QJsonDocument(QJsonObject{ { "backend", "webkit" },
                                                { "gui_heartbeats", beats },
                                                { "pages", session->pages() },
                                                { "first_sha256", QString::fromLatin1(first) },
                                                { "navigation", true },
                                                { "resize", true },
                                                { "sleep_resume", true } })
                             .toJson());
        lease->cancel();
        session.reset();
        ReaderSession::drain([&] { app.quit(); });
    });
    return app.exec();
}

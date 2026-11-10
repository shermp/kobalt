#include "check.h"
#include "publication/local_resources.h"
#include "reading/reader_session.h"
#include "reading/reading_book.h"
#include "reading/source_position.h"
#include "support/main_thread.h"
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#include <QTemporaryDir>

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
        require(!session->chapters().isEmpty(), "Publication table of contents exposed");
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
        require(!session->state().contains("position"), "CREngine position remains separate");
        const auto contents = session->chapters();
        require(session->goTo(contents.at(qMin(1, contents.size() - 1)).position),
                "TOC destination queued");
        completion.exec();
        require(session->error().isEmpty(), "TOC source destination resolves");
        require(session->turn(1), "Turn into chapter text");
        completion.exec();
        const auto anchor = session->state().value("source_position").toObject();
        require(sourceposition::valid(anchor), "Versioned source position captured");
        auto appearance = session->persistentState();
        appearance["font_pixels"] = 32;
        session->apply(appearance, { 800, 600 });
        completion.exec();
        require(session->error().isEmpty()
                        && session->state().value("source_position").toObject() == anchor,
                "Typography and viewport changes retain the exact source anchor");
        const auto fingerprint = session->sourceHash();
        const auto identity = request.identity;
        QTemporaryDir profile;
        require(profile.isValid(), "Create diagnostic profile");
        const storage::Paths paths{ profile.path() };
        const QJsonObject original{ { "schema", 1 },
                                    { "book", identity },
                                    { "position", "CREngine position retained" },
                                    { "fingerprint", "previous-CREngine-fingerprint" },
                                    { "future_field", "preserved" } };
        require(storage::writeBookState(paths, identity, original),
                "Prepare existing saved record");
        session->updated = {};
        {
            ReadingBook book(std::move(session), paths.bookState(identity), paths, identity,
                             fingerprint, nullptr, {}, {});
            mainthread::Interaction reading([] { return true; });
            book.checkpointForClose([&](bool saved) {
                require(saved, "Background source checkpoint saved");
                completion.quit();
            });
            completion.exec();
        }
        auto saved = storage::readBookState(paths, identity).object;
        auto preserved = saved;
        preserved.remove("backend_positions");
        require(preserved == original, "CREngine and unknown record fields preserved");
        session = std::make_unique<ReaderSession>(EngineFontInputs{}, "serif");
        lease = std::make_shared<SourceLease>();
        request.lease = lease;
        request.resources = localResourceFactory(path, lease);
        request.saved = saved;
        session->open(request, [&](OpeningResult result) {
            require(result.ready() && session->experimentalBackend(), "Cold WebKit reopening");
            require(session->state().value("source_position").toObject() == anchor,
                    "Source anchor restored after a cold open at another viewport");
            completion.quit();
        });
        completion.exec();
        QFile report(output + "/session.json");
        require(report.open(QIODevice::WriteOnly), "Open firmware report");
        report.write(QJsonDocument(QJsonObject{ { "backend", "webkit" },
                                                { "gui_heartbeats", beats },
                                                { "pages", session->pages() },
                                                { "first_sha256", QString::fromLatin1(first) },
                                                { "navigation", true },
                                                { "resize", true },
                                                { "sleep_resume", true },
                                                { "source_position", anchor },
                                                { "persistent_position", true },
                                                { "toc_navigation", true },
                                                { "saved_data_preserved", true } })
                             .toJson());
        lease->cancel();
        session.reset();
        ReaderSession::drain([&] { app.quit(); });
    });
    return app.exec();
}

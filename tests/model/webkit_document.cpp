#include "check.h"
#include "reader_inputs.h"
#include "engine/webkit/webkit_document.h"
#include "publication/local_resources.h"
#include "reading/reader_session.h"
#include "reading/reading_book.h"
#include "support/main_thread.h"
#include <QGuiApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <cerrno>
#include <unistd.h>

using namespace kobalt;

namespace {
volatile std::sig_atomic_t stopped = 0;

int helper()
{
    std::signal(SIGTERM, [](int) { stopped = 1; });
    QTemporaryDir runtime("/dev/shm/kobalt-webkit-XXXXXX");
    require(runtime.isValid(), "helper runtime");
    const auto mode = qgetenv("KOBALT_TEST_WEBKIT_MODE");
    std::cout << "KWRP/1 0 READY " << (mode == "bad-ready" ? "experiment" : "sandbox-required")
              << '\n'
              << std::flush;
    const auto pidPath = qgetenv("KOBALT_TEST_WEBKIT_PID_FILE");
    if (!pidPath.isEmpty()) {
        QFile pidFile(QString::fromLocal8Bit(pidPath));
        require(pidFile.open(QIODevice::WriteOnly), "helper PID file");
        pidFile.write(QByteArray::number(getpid()));
    }
    int width = 600, height = 800, spine = 0, page = 0;
    const int counts[] = { 2, 1, 3 };
    while (!stopped) {
        pollfd input{ 0, POLLIN, 0 };
        if (poll(&input, 1, 10) <= 0)
            continue;
        std::string line, version, action;
        if (!std::getline(std::cin, line))
            break;
        unsigned id = 0;
        std::istringstream request(line);
        request >> version >> id >> action;
        const auto prefix = "KWRP/1 " + std::to_string(id) + " ";
        if (action == "RELEASE") {
            unsigned frame;
            request >> frame;
            for (const auto *suffix : { ".bgra", ".json" })
                QFile::remove(runtime.filePath(QString("frame-%1").arg(frame) + suffix));
            std::cout << prefix << "OK RELEASED\n" << std::flush;
            continue;
        }
        if (mode == "hang")
            continue;
        if (action == "OPEN") {
            std::string path;
            request >> path >> width >> height;
            spine = page = 0;
        } else if (action == "RESIZE")
            request >> width >> height;
        else if (action == "GOTO") {
            request >> spine >> page;
            page = qMin(page, counts[spine] - 1);
        }
        const auto stem = runtime.filePath(QString("frame-%1").arg(id));
        QImage pixels(width, height, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(QColor(10 + spine, 20 + page, 30));
        QFile raw(stem + ".bgra");
        require(raw.open(QIODevice::WriteOnly), "helper raw frame");
        raw.write(reinterpret_cast<const char *>(pixels.constBits()),
                  pixels.bytesPerLine() * height);
        raw.close();
        QJsonObject frame{ { "version", 1 },
                           { "frame", double(id) },
                           { "spine", spine },
                           { "width", width },
                           { "height", height },
                           { "stride", pixels.bytesPerLine() },
                           { "format", "BGRA8-premultiplied" },
                           { "pixels", stem + ".bgra" },
                           { "document",
                             QJsonObject{ { "spine", QJsonArray{ "one", "aside", "two" } },
                                          { "linear", QJsonArray{ true, false, true } } } },
                           { "layout",
                             QJsonObject{ { "page", page }, { "pages", counts[spine] } } } };
        if (mode == "bad-size")
            frame["width"] = 999999;
        if (mode == "bad-path")
            frame["pixels"] = "/etc/passwd";
        QFile metadata(stem + ".json");
        require(metadata.open(QIODevice::WriteOnly), "helper metadata");
        metadata.write(QJsonDocument(frame).toJson(QJsonDocument::Compact));
        metadata.close();
        std::cout << (mode == "bad-id" ? "KWRP/2 999 " : prefix) << "OK PAGE " << stem.toStdString()
                  << ".json\n"
                  << std::flush;
    }
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && QByteArray(argv[1]) == "--memory-frames") {
        QCoreApplication app(argc, argv);
        return helper();
    }
    QGuiApplication app(argc, argv);
    qputenv("KOBALT_WEBKIT_LAUNCHER", QFile::encodeName(app.applicationFilePath()));
    qunsetenv("KOBALT_WEBKIT_MODE");
    qunsetenv("KOBALT_WEBKIT_QEMU_PREFIX");
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "proxy opening");
        require(document.pages() == 6, "whole-book page counts");
        require(document.turnTarget(1, 1) == 3 && document.turnTarget(3, -1) == 1,
                "turns skip non-linear spine entries");
        const auto first = document.image(0);
        require(document.go(3) && document.page() == 3, "cross chapter navigation");
        require(document.image(3).pixel(0, 0) != first.pixel(0, 0), "destination pixels");
        require(first.pixel(0, 0) == qRgb(10, 20, 30), "released frame remains immutable");
        require(document.applyAppearance({ { "font_pixels", 30 } }, { 800, 600 }), "resize/style");
        require(document.image(document.page()).size() == QSize(800, 600), "resized image");
        const auto position = document.page();
        document.close();
        require(!document.image(position).isNull() && document.page() == position,
                "sleep resume preserves in-memory location");
        require(!document.state().contains("position") && !document.state().contains("dom_version"),
                "provisional locations cannot overwrite CREngine positions");
    }
    for (const auto *mode : { "bad-size", "bad-path", "bad-id", "bad-ready" }) {
        qputenv("KOBALT_TEST_WEBKIT_MODE", mode);
        WebKitDocument document({}, "/unused.epub");
        require(!document.openResources({}, {}, { 600, 800 }, {}, nullptr), "reject invalid frame");
        require(!document.resourceError().isEmpty(), "invalid frame reports an error");
    }
    qputenv("KOBALT_TEST_WEBKIT_MODE", "hang");
    {
        WebKitDocument document({}, "/unused.epub");
        int polls = 0;
        document.setCurrent([&] { return ++polls < 5; });
        require(!document.openResources({}, {}, { 600, 800 }, {}, nullptr),
                "cancel hanging helper");
        require(document.resourceError().contains("cancelled"), "cancellation error");
    }
    qunsetenv("KOBALT_TEST_WEBKIT_MODE");
    QTemporaryDir helperState;
    const auto pidFilePath = helperState.filePath("helper.pid");
    qputenv("KOBALT_TEST_WEBKIT_PID_FILE", QFile::encodeName(pidFilePath));
    std::unique_ptr<ReaderSession> shutdownOwner;
    // A real ReaderSession must keep processing GUI events, discard stale work
    // and fall back through the unchanged CREngine opening path.
    QTimer::singleShot(0, &app, [&] {
        const QString path = QStringLiteral(KOBALT_TEST_BOOK);
        auto lease = std::make_shared<SourceLease>();
        auto session = std::make_unique<ReaderSession>(test::readerFonts(), "Libron");
        OpenRequest request{ "file://" + path,
                             path,
                             { 600, 800 },
                             {},
                             {},
                             {},
                             lease,
                             localResourceFactory(path, lease) };
        request.backend = "webkit";
        QEventLoop opened;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &opened,
                         [] { require(false, "session opening timed out"); });
        timeout.start(60000);
        session->open(request, [&](OpeningResult result) {
            require(result.ready() && session->experimentalBackend(), "session uses proxy");
            require(!session->image().isNull(), "session publishes CPU image");
            opened.quit();
        });
        opened.exec();
        int frames = 0;
        session->updated = [&] {
            ++frames;
            opened.quit();
        };
        require(session->turn(1), "session turns proxy page");
        opened.exec();
        require(frames == 1 && session->page() == 1, "proxy navigation frame");
        QFile pidFile(pidFilePath);
        require(pidFile.open(QIODevice::ReadOnly), "Read idle helper PID");
        const auto helperPid = pidFile.readAll().toInt();
        require(helperPid > 0, "Idle helper PID");
        session->revokeSource();
        ReaderSession::drain([&] { opened.quit(); });
        opened.exec();
        errno = 0;
        require(kill(helperPid, 0) == -1 && errno == ESRCH, "Revocation reaps idle helper");
        session->updated = {};
        {
            QTemporaryDir profile;
            storage::Paths paths{ profile.path() };
            const auto identity = request.identity;
            const QJsonObject saved{ { "book", identity },
                                     { "position", "CREngine XPointer" },
                                     { "margin_pixels", 40 },
                                     { "future_field", "preserve" } };
            require(storage::writeBookState(paths, identity, saved), "Prepare saved state");
            ReadingBook book(std::move(session), paths.bookState(identity), paths, identity, {},
                             nullptr, {}, {});
            book.observeReading();
            book.rememberAppearance();
            book.saveReadingActivity();
            QJsonObject snapshot;
            require(book.checkpoint(&snapshot), "Experimental checkpoint completes");
            bool closed = false;
            book.checkpointForClose([&](bool ok) { closed = ok; });
            require(closed, "Experimental close completes");
            require(storage::readBookState(paths, identity).object == saved,
                    "Experimental reading preserves all saved data");
            require(!QFile::exists(paths.preferences()),
                    "Experimental reading preserves preferences");
        }
        session = std::make_unique<ReaderSession>(test::readerFonts(), "Libron");
        qputenv("KOBALT_WEBKIT_LAUNCHER", "/missing-kobalt-webkit");
        lease = std::make_shared<SourceLease>();
        request.lease = lease;
        request.resources = localResourceFactory(path, lease);
        session->open(request, [&](OpeningResult result) {
            require(result.ready() && !session->experimentalBackend(), "CREngine fallback");
            require(!session->image().isNull(), "fallback publishes CREngine image");
            opened.quit();
        });
        opened.exec();
        qputenv("KOBALT_WEBKIT_LAUNCHER", QFile::encodeName(app.applicationFilePath()));
        lease = std::make_shared<SourceLease>();
        request.lease = lease;
        request.resources = localResourceFactory(path, lease);
        session->open(request, [&](OpeningResult result) {
            require(result.ready() && session->experimentalBackend(), "Shutdown proxy opening");
            opened.quit();
        });
        opened.exec();
        shutdownOwner = std::move(session);
        app.quit();
    });
    const int result = app.exec();
    {
        mainthread::Interaction stillRegistered([] { return true; });
        shutdownOwner.reset();
    }
    QFile lastPid(pidFilePath);
    require(lastPid.open(QIODevice::ReadOnly), "Shutdown helper PID");
    const auto shutdownPid = lastPid.readAll().toInt();
    require(shutdownPid > 0 && kill(shutdownPid, 0) == -1 && errno == ESRCH,
            "Shutdown reaps helper after worker join");
    return result;
}

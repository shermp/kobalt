#include "check.h"
#include "reader_inputs.h"
#include "engine/webkit/webkit_document.h"
#include "publication/local_resources.h"
#include "reading/reader_session.h"
#include "reading/reading_book.h"
#include "reading/source_position.h"
#include "support/main_thread.h"
#include "support/diagnostics.h"
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
    if (mode == "exit") {
        std::cerr << std::string(16384, 'x') << " fixture-helper-exit\n" << std::flush;
        return 23;
    }
    std::cout << "KWRP/1 0 READY " << (mode == "bad-ready" ? "experiment" : "sandbox-required")
              << '\n'
              << std::flush;
    const auto pidPath = qgetenv("KOBALT_TEST_WEBKIT_PID_FILE");
    if (!pidPath.isEmpty()) {
        QFile pidFile(QString::fromLocal8Bit(pidPath));
        require(pidFile.open(QIODevice::WriteOnly), "helper PID file");
        pidFile.write(QByteArray::number(getpid()));
    }
    int width = 600, height = 800, spine = 0, page = 0, offset = 0;
    const int lengths[] = { 200, 100, 300 };
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
            spine = page = offset = 0;
        } else if (action == "RESIZE")
            request >> width >> height;
        else if (action == "GOTO") {
            request >> spine >> page;
            offset = page * (width == 800 ? 50 : 100);
        } else if (action == "LOCATE") {
            std::string encoded;
            request >> spine >> encoded;
            const auto position =
                    QJsonDocument::fromJson(
                            QByteArray::fromBase64(QByteArray::fromStdString(encoded)))
                            .object();
            require(sourceposition::valid(position), "helper source position");
            offset = position.value("offset").toInt();
        } else if (action == "LINK") {
            std::string encoded;
            request >> encoded;
            const auto href = QByteArray::fromBase64(QByteArray::fromStdString(encoded));
            spine = href.contains("two") ? 2 : 0;
            offset = href.contains("#middle") ? 100 : 0;
        }
        const int units = width == 800 ? 50 : 100;
        const int pages = (lengths[spine] + units - 1) / units;
        offset = qBound(0, offset, lengths[spine] - 1);
        page = offset / units;
        const QJsonObject locator{
            { "version", mode == "bad-locator" ? 2 : 1 },
            { "cfi", QString("epubcfi(/6/%1!/4/2/1:%2)").arg((spine + 1) * 2).arg(offset) },
            { "offset", offset },
            { "before", offset ? "before" : "" },
            { "after", "after" }
        };
        const auto stem = runtime.filePath(QString("frame-%1").arg(id));
        QImage pixels(width, height, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(QColor(10 + spine, 20 + page, 30));
        QFile raw(stem + ".bgra");
        require(raw.open(QIODevice::WriteOnly), "helper raw frame");
        raw.write(reinterpret_cast<const char *>(pixels.constBits()),
                  pixels.bytesPerLine() * height);
        raw.close();
        QJsonObject frame{
            { "version", 1 },
            { "frame", double(id) },
            { "spine", spine },
            { "width", width },
            { "height", height },
            { "stride", pixels.bytesPerLine() },
            { "format", "BGRA8-premultiplied" },
            { "pixels", stem + ".bgra" },
            { "document",
              QJsonObject{
                      { "spine",
                        QJsonArray{ "one", mode == "duplicate-spine" ? "one" : "aside", "two" } },
                      { "spine_cfi", QJsonArray{ "/6/2", "/6/4", "/6/6" } },
                      { "linear", QJsonArray{ true, false, true } },
                      { "navigation",
                        QJsonArray{ QJsonObject{ { "title", "Opening" },
                                                 { "href", "kobalt-epub://fixture/one" } },
                                    QJsonObject{ { "title", "Middle" },
                                                 { "href", "kobalt-epub://fixture/one#middle" },
                                                 { "depth", 1 } },
                                    QJsonObject{ { "title", "Two" },
                                                 { "href", "kobalt-epub://fixture/two" } },
                                    QJsonObject{ { "title", "Duplicate" },
                                                 { "href", "kobalt-epub://fixture/two" } } } } } },
            { "layout",
              QJsonObject{
                      { "page", page },
                      { "pages", pages },
                      { "locator", locator },
                      { "location",
                        QJsonObject{ { "start", page * units }, { "end", (page + 1) * units } } },
                      { "title", "Fixture chapter " + QString::number(spine) } } }
        };
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
    QTemporaryDir diagnosticDirectory;
    require(diagnosticDirectory.isValid(), "Diagnostic directory");
    initializeDiagnostics(diagnosticDirectory.path());
    qputenv("KOBALT_WEBKIT_LAUNCHER", QFile::encodeName(app.applicationFilePath()));
    qunsetenv("KOBALT_WEBKIT_MODE");
    qunsetenv("KOBALT_WEBKIT_QEMU_PREFIX");
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "proxy opening");
        require(document.pages() == 6, "whole-book page counts");
        require(document.chapterProgress().title == "Opening",
                "Chapter header uses the navigation title rather than an archive path");
        require(document.chapterProgress().page == 1 && document.chapterProgress().pages == 1,
                "Chapter progress uses the reader's one-based page numbering");
        require(document.turnTarget(1, 1) == 3 && document.turnTarget(3, -1) == 1,
                "turns skip non-linear spine entries");
        const auto first = document.image(0);
        require(document.go(3) && document.page() == 3, "cross chapter navigation");
        require(document.image(3).pixel(0, 0) != first.pixel(0, 0), "destination pixels");
        require(first.pixel(0, 0) == qRgb(10, 20, 30), "released frame remains immutable");
        const auto contents = document.chapters();
        require(contents.size() == 4 && contents.at(1).depth == 1 && contents.at(1).page == 1,
                "TOC resolves fragments and preserves nesting");
        require(contents.at(2).page == contents.at(3).page,
                "Duplicate TOC destinations remain separate rows");
        require(document.goTo(contents.at(1).position) && document.page() == 1,
                "TOC uses layout-independent source positions");
        const auto anchor = document.state().value("source_position").toObject();
        require(sourceposition::fromToken(sourceposition::token(anchor)) == anchor,
                "Source position token round trip");
        for (const auto *key : { "version", "href", "cfi", "offset", "before", "after" }) {
            auto invalid = anchor;
            invalid.remove(key);
            require(!sourceposition::valid(invalid),
                    "Source locator requires its versioned fields");
        }
        auto future = anchor;
        future["version"] = 2;
        require(!sourceposition::valid(future), "Future source versions are unavailable");
        auto traversal = anchor;
        traversal["href"] = "../one";
        require(!sourceposition::valid(traversal), "Source locators cannot escape the publication");
        require(sourceposition::fromToken(sourceposition::token(anchor) + "!").isEmpty(),
                "Source token rejects noncanonical base64");
        require(document.applyAppearance({ { "font_pixels", 30 } }, { 800, 600 }), "resize/style");
        require(document.page() == 2
                        && document.state().value("source_position").toObject() == anchor,
                "Reflow restores the source character, not an old page number");
        require(document.chapters().at(1).page == 2, "TOC pages follow reflow");
        require(document.image(document.page()).size() == QSize(800, 600), "resized image");
        const auto position = document.page();
        document.close();
        require(!document.image(position).isNull() && document.page() == position,
                "sleep resume preserves in-memory location");
        require(!document.state().contains("position") && !document.state().contains("dom_version"),
                "Source positions cannot overwrite CREngine positions");
        WebKitDocument reopened({}, "/unused.epub");
        const QJsonObject saved{
            { "backend_positions",
              QJsonObject{ { "webkit", QJsonObject{ { "position", anchor } } } } }
        };
        require(reopened.openResources({}, {}, { 600, 800 }, saved, nullptr) && reopened.page() == 1
                        && reopened.state().value("source_position").toObject() == anchor,
                "Cold reopening restores a source position at another viewport");
        auto unsupported = saved;
        unsupported["backend_positions"] =
                QJsonObject{ { "webkit", QJsonObject{ { "position", future } } } };
        QString error;
        OpeningResult openingResult;
        require(!reopened.openResources({}, {}, { 600, 800 }, unsupported, &error, {},
                                        &openingResult)
                        && !error.isEmpty() && !openingResult.ready(),
                "Unsupported saved source positions cannot be silently replaced");
    }
    qputenv("KOBALT_TEST_WEBKIT_MODE", "duplicate-spine");
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr) && document.go(2),
                "Explicitly open a repeated spine resource");
        const auto position = document.state().value("source_position").toObject();
        require(position.value("href").toString() == "one"
                        && position.value("cfi").toString().startsWith("epubcfi(/6/4!"),
                "Repeated resources retain their package occurrence");
        WebKitDocument reopened({}, "/unused.epub");
        require(reopened.openResources(
                        {}, {}, { 600, 800 },
                        { { "backend_positions",
                            QJsonObject{
                                    { "webkit", QJsonObject{ { "position", position } } } } } },
                        nullptr)
                        && reopened.page() == 2,
                "Restoration distinguishes repeated spine entries with the same href");
    }
    qunsetenv("KOBALT_TEST_WEBKIT_MODE");
    for (const auto *mode : { "bad-size", "bad-path", "bad-id", "bad-ready", "bad-locator" }) {
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
    {
        QJsonObject exited;
        setDiagnosticObserver([&](const QJsonObject &entry) {
            if (entry.value("event").toString() == "webkit-helper-exited")
                exited = entry;
        });
        qputenv("KOBALT_TEST_WEBKIT_MODE", "exit");
        WebKitDocument document({}, "/unused.epub");
        require(!document.openResources({}, {}, { 600, 800 }, {}, nullptr), "Helper exit reported");
        require(exited.value("exit_code").toInt() == 23, "Helper exit status retained");
        const auto tail = exited.value("stderr").toString();
        require(tail.size() <= 4096 && tail.endsWith("fixture-helper-exit\n"),
                "Helper stderr is bounded and retains its final error");
        setDiagnosticObserver({});
        qunsetenv("KOBALT_TEST_WEBKIT_MODE");
    }
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
            const auto fingerprint = session->sourceHash();
            const auto position = session->state().value("source_position").toObject();
            const QJsonObject saved{ { "book", identity },
                                     { "position", "CREngine XPointer" },
                                     { "fingerprint", "older-CREngine-content" },
                                     { "margin_pixels", 40 },
                                     { "future_field", "preserve" },
                                     { "backend_positions",
                                       QJsonObject{ { "another", "preserve" } } } };
            require(storage::writeBookState(paths, identity, saved), "Prepare saved state");
            ReadingProgress baseline;
            baseline.valid = true;
            baseline.book = identity;
            baseline.status = 0;
            {
                ReadingBook book(std::move(session), paths.bookState(identity), paths, identity,
                                 fingerprint, nullptr, baseline, baseline);
                mainthread::Interaction reading([] { return true; });
                book.observeReading();
                book.rememberAppearance();
                book.saveReadingActivity();
                QJsonObject snapshot;
                require(book.checkpoint(&snapshot, true), "Background source checkpoint queued");
                bool closed = false;
                book.checkpointForClose([&](bool ok) {
                    require(ok, "Source checkpoint saved");
                    closed = true;
                    opened.quit();
                });
                if (!closed)
                    opened.exec();
                require(closed, "Source close checkpoint completes asynchronously");
            }
            auto persisted = storage::readBookState(paths, identity).object;
            const auto backendPositions = persisted.take("backend_positions").toObject();
            persisted.remove("schema");
            auto expected = saved;
            expected.remove("backend_positions");
            expected["nickel_progress"] = baseline.json();
            require(persisted == expected,
                    "WebKit checkpoints preserve CREngine and native fields");
            auto unread = persisted;
            require(!resetNativeUnread(unread, baseline) && unread == persisted,
                    "Unchanged native Unread status cannot discard the new backend checkpoint");
            require(backendPositions.value("another").toString() == "preserve",
                    "Other backend positions survive");
            const auto slot = backendPositions.value("webkit").toObject();
            require(slot.value("position").toObject() == position
                            && slot.value("fingerprint").toString().toLatin1() == fingerprint,
                    "WebKit saves its independently identified source position");
            require(!QFile::exists(paths.preferences()),
                    "Experimental reading preserves preferences");
            auto restored = std::make_unique<ReaderSession>(test::readerFonts(), "Libron");
            lease = std::make_shared<SourceLease>();
            request.lease = lease;
            request.resources = localResourceFactory(path, lease);
            request.saved = storage::readBookState(paths, identity).object;
            restored->open(request, [&](OpeningResult result) {
                require(result.ready() && restored->experimentalBackend() && restored->page() == 1,
                        "Session reopens WebKit despite a different preserved CREngine fingerprint");
                require(restored->state().value("source_position").toObject() == position,
                        "Session restores the saved source anchor");
                opened.quit();
            });
            opened.exec();
        }
        session = std::make_unique<ReaderSession>(test::readerFonts(), "Libron");
        request.saved = {};
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
    flushDiagnostics();
    return result;
}

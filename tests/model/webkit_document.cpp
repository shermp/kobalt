#include "check.h"
#include "reader_inputs.h"
#include "engine/webkit/webkit_document.h"
#include "publication/local_resources.h"
#include "publication/publication.h"
#include "reading/reader_session.h"
#include "reading/reading_book.h"
#include "reading/source_position.h"
#include "support/main_thread.h"
#include "support/diagnostics.h"
#include <QGuiApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
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
    const auto faultPath = qgetenv("KOBALT_TEST_WEBKIT_FAULT_FILE");
    if (!pidPath.isEmpty()) {
        QFile pidFile(QString::fromLocal8Bit(pidPath));
        require(pidFile.open(QIODevice::WriteOnly), "helper PID file");
        pidFile.write(QByteArray::number(getpid()));
    }
    int width = 600, height = 800, spine = 0, page = 0, offset = 0;
    const int lengths[] = { 200, 100, 300 };
    const auto locatorAt = [&](int selected) {
        return QJsonObject{
            { "version", mode == "bad-locator" ? 2 : 1 },
            { "cfi", QString("epubcfi(/6/%1!/4/2/1:%2)").arg((spine + 1) * 2).arg(selected) },
            { "offset", selected },
            { "before", selected ? "before" : "" },
            { "after", "after" }
        };
    };
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
        if (!faultPath.isEmpty()) {
            QFile fault(QString::fromLocal8Bit(faultPath));
            if (fault.open(QIODevice::ReadOnly)) {
                const auto parts = fault.readAll().simplified().split(' ');
                fault.close();
                if (parts.size() >= 2 && parts.first() == QByteArray::fromStdString(action)) {
                    if (!parts.contains("repeat"))
                        require(fault.remove(), "Consume a one-shot fault");
                    if (parts[1] == "terminated")
                        std::cout << prefix << "ERROR web-process-terminated\n" << std::flush;
                    if (parts[1] == "invalid")
                        std::cout << "KWRP/1 999999 OK PAGE invalid\n" << std::flush;
                    return 23;
                }
            }
        }
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
        if (action == "QUERY") {
            std::string encoded, fragment;
            request >> spine >> page >> encoded >> fragment;
            offset = page * (width == 800 ? 50 : 100);
            if (!fragment.empty()) {
                const auto target = QByteArray::fromBase64(QByteArray::fromStdString(fragment));
                if (target == "middle")
                    offset = 100;
                else {
                    std::cout << prefix << "ERROR missing-link-target\n" << std::flush;
                    continue;
                }
                page = offset / (width == 800 ? 50 : 100);
            }
            const auto query = QJsonDocument::fromJson(
                                       QByteArray::fromBase64(QByteArray::fromStdString(encoded)))
                                       .object();
            const auto kind = query.value("kind").toString();
            if (!qgetenv("KOBALT_TEST_WEBKIT_QUERY_LOG").isEmpty()) {
                QFile log(QString::fromLocal8Bit(qgetenv("KOBALT_TEST_WEBKIT_QUERY_LOG")));
                require(log.open(QIODevice::WriteOnly | QIODevice::Append), "Query budget log");
                log.write(kind.toUtf8() + '\n');
            }
            if (mode == "hang-query" && kind == "search") {
                QFile marker(QString::fromLocal8Bit(pidPath) + ".query");
                require(marker.open(QIODevice::WriteOnly), "Query arrival marker");
                marker.write("query");
                continue;
            }
            auto start = locatorAt(offset + 10), end = locatorAt(offset + 16);
            if (kind == "range") {
                start = query.value("start").toObject();
                end = query.value("end").toObject();
                if (start.value("offset").toInt() > end.value("offset").toInt())
                    std::swap(start, end);
            }
            if (mode == "bad-query-token")
                start["cfi"] = "epubcfi(/6/999!/4/2/1:0)";
            QJsonObject result{
                { "text", "source" },
                { "start", start },
                { "end", end },
                { "rectangles",
                  QJsonArray{ QJsonObject{ { "x", 10 },
                                           { "y", 20 },
                                           { "width", mode == "bad-query-rect" ? 1e30 : 60 },
                                           { "height", 20 } } } }
            };
            if (kind == "search") {
                const int selected = query.value("page").toInt();
                result = {
                    { "hits",
                      QJsonArray{ QJsonObject{
                              { "start", locatorAt(selected * (width == 800 ? 50 : 100) + 10) },
                              { "end", locatorAt(selected * (width == 800 ? 50 : 100) + 16) },
                              { "text", "source" },
                              { "before", "before " },
                              { "after", " after" },
                              { "page", selected } } } },
                    { "limited", false }
                };
            } else if (kind == "layout") {
                result = {
                    { "page", page },
                    { "pages",
                      mode == "bad-layout-pages" ? 0
                                                 : (lengths[spine] + (width == 800 ? 49 : 99))
                                      / (width == 800 ? 50 : 100) },
                    { "locator", mode == "bad-layout-locator" ? QJsonObject() : locatorAt(offset) },
                    { "location",
                      QJsonObject{ { "start", page * (width == 800 ? 50 : 100) },
                                   { "end", (page + 1) * (width == 800 ? 50 : 100) } } },
                    { "viewport",
                      QJsonObject{ { "width", mode == "bad-layout-viewport" ? width + 1 : width },
                                   { "height", height } } }
                };
            } else if (kind == "annotation-token" || kind == "drag-token") {
                result["punctuation"] = false;
            } else if (kind == "annotation-range") {
                result["chosenStart"] = start;
                result["chosenEnd"] = end;
                result["startPolicy"] = query.value("startPolicy");
                result["endPolicy"] = query.value("endPolicy");
            } else if (kind == "page-edges") {
                result["punctuation"] = false;
                result = {
                    { "first", result },
                    { "last", result },
                    { "before",
                      QJsonArray{ locatorAt(offset + 10), locatorAt(qMax(0, offset - 10)) } },
                    { "after", QJsonArray{ locatorAt(offset + 16), locatorAt(offset + 60) } }
                };
            } else if (kind == "resolve") {
                QJsonArray points;
                for (const auto &position : query.value("positions").toArray())
                    points.append(QJsonObject{ { "locator", position }, { "selectable", true } });
                result = { { "positions", points } };
                if (mode == "bad-resolve")
                    result = {};
            } else if (kind == "ranges") {
                QJsonArray ranges;
                for (const auto &range : query.value("ranges").toArray()) {
                    auto row = result;
                    row["start"] = range.toObject().value("start").isNull()
                            ? locatorAt(0)
                            : range.toObject().value("start");
                    row["end"] = range.toObject().value("end").isNull()
                            ? locatorAt(lengths[spine])
                            : range.toObject().value("end");
                    const auto edge = [&](int x, int normal) {
                        return QJsonObject{ { "visible", true },
                                            { "first", QJsonObject{ { "x", x }, { "y", 20 } } },
                                            { "last", QJsonObject{ { "x", x }, { "y", 40 } } },
                                            { "outward",
                                              QJsonObject{ { "x", normal }, { "y", 0 } } } };
                    };
                    row["startEdge"] = edge(10, mode == "bad-query-edge" ? 2 : -1);
                    row["endEdge"] = edge(70, 1);
                    ranges.append(row);
                }
                result = { { "ranges", ranges } };
            } else if (kind == "summary") {
                result = { { "start", locatorAt(offset) }, { "excerpt", "Source page excerpt" } };
            } else if (kind == "position") {
                const auto position = query.value("position").toObject();
                const int selected = position.value("offset").toInt();
                result = { { "locator", position },
                           { "page",
                             qMax(0, selected - (query.value("exclusive").toBool() ? 1 : 0))
                                     / (width == 800 ? 50 : 100) } };
            }
            const auto path = mode == "bad-query-source" && kind != "layout" ? "wrong"
                    : spine == 2                                             ? "two"
                    : spine == 1 && mode != "duplicate-spine"                ? "aside"
                                                                             : "one";
            const QJsonObject envelope{
                { "spine", spine },
                { "page", page },
                { "data",
                  QJsonObject{ { "source", "kobalt-epub://fixture/" + QString(path) },
                               { "result", result } } }
            };
            std::cout
                    << prefix << "OK JSON "
                    << QJsonDocument(envelope).toJson(QJsonDocument::Compact).toBase64().constData()
                    << '\n'
                    << std::flush;
            continue;
        }
        if (action == "OPEN") {
            std::string path;
            request >> path >> width >> height;
            spine = page = offset = 0;
        } else if (action == "RESIZE")
            request >> width >> height;
        else if (action == "GOTO") {
            request >> spine >> page;
            if (mode == "wrong-preview-page")
                page = 0;
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
            if (!href.startsWith('#'))
                spine = href.contains("two") ? 2 : 0;
            offset = href.contains("#middle") ? 100 : 0;
        }
        const int units = width == 800 ? 50 : 100;
        const int pages = (lengths[spine] + units - 1) / units;
        offset = qBound(0, offset, lengths[spine] - 1);
        page = offset / units;
        const auto locator = locatorAt(offset);
        const auto frameLog = qgetenv("KOBALT_TEST_WEBKIT_FRAME_LOG");
        if (!frameLog.isEmpty()) {
            QFile log(QString::fromLocal8Bit(frameLog));
            require(log.open(QIODevice::WriteOnly | QIODevice::Append), "Frame allocation log");
            log.write("frame\n");
        }
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
                      { "source",
                        QString("kobalt-epub://fixture/%1")
                                .arg(spine == 2           ? "two"
                                             : spine == 1 ? "aside"
                                                          : "one") },
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
    QTemporaryDir frameBudget;
    const auto frameLog = frameBudget.filePath("frames.log");
    const auto queryLog = frameBudget.filePath("queries.log");
    qputenv("KOBALT_TEST_WEBKIT_FRAME_LOG", QFile::encodeName(frameLog));
    qputenv("KOBALT_TEST_WEBKIT_QUERY_LOG", QFile::encodeName(queryLog));
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "proxy opening");
        require(QFileInfo(frameLog).size() <= 3 * 6,
                "Opening metadata scans do not allocate chapter or TOC page buffers");
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
        const auto firstPage = document.pageStart(0), markedPage = document.pageStart(1);
        const auto lastPage = document.pageStart(5);
        require(!markedPage.isEmpty() && document.pageExcerpt(1) == "Source page excerpt",
                "Page summaries expose source anchors and excerpts without image transfers");
        QVector<int> sourceOrder;
        require(document.pagesOf({ lastPage, markedPage, "/legacy", firstPage }, false,
                                 &sourceOrder)
                                == QVector<int>({ 5, 1, -1, 0 })
                        && sourceOrder == QVector<int>({ 2, 1, -1, 0 }),
                "Source bookmark pages and order reject legacy anchors");
        require(document.pagesOf({ firstPage, markedPage }, true) == QVector<int>({ 0, 0 }),
                "Exclusive range ends do not add an empty boundary page");
        auto earlier = sourceposition::fromToken(firstPage), later = earlier;
        earlier["offset"] = 2;
        earlier["cfi"] = "epubcfi(/6/2!/4/2/1:2)";
        later["offset"] = 10;
        later["cfi"] = "epubcfi(/6/2!/4/2/1:10)";
        require(document.pagesOf({ sourceposition::token(later), sourceposition::token(earlier) },
                                 false, &sourceOrder)
                                == QVector<int>({ 0, 0 })
                        && sourceOrder == QVector<int>({ 1, 0 }),
                "Source order compares CFI offsets numerically within one page");
        require(document.state().value("source_position").toObject() == anchor,
                "Bookmark resolution preserves the displayed source point");
        const auto displayedBeforePreview = document.image(document.page());
        const auto distantPreview = document.image(5);
        require(!distantPreview.isNull() && distantPreview.pixel(0, 0) == qRgb(12, 22, 30),
                "Preview returns the requested off-screen page pixels");
        require(document.page() == 1
                        && document.state().value("source_position").toObject() == anchor
                        && document.image(1).cacheKey() == displayedBeforePreview.cacheKey()
                        && document.cachedPages() == 1 && document.cachedImageKey(5) == 0,
                "Preview preserves the reading frame and retains no off-screen bitmap");
        require(document.goTo("#middle") && document.page() == 1,
                "A relative link still resolves from the displayed spine after a preview");
        const auto queryImage = document.image(document.page());
        const auto word = document.wordAt({ 30, 30 });
        require(word.text == "source" && !word.start.isEmpty() && !word.end.isEmpty()
                        && word.rectangles == QVector<QRect>{ QRect(10, 20, 60, 20) },
                "Word queries bind source anchors and viewport rectangles");
        const auto range = document.textRange(word.end, word.start);
        require(range.text == word.text && range.start == word.start && range.end == word.end,
                "Source ranges support reverse endpoints without treating them as XPointers");
        require(document.rangeRects(word.start, word.end) == word.rectangles,
                "Selection geometry shares the source range");
        const auto geometry = document.annotationGeometry(word.start, word.end);
        require(geometry.start.visible && geometry.end.visible
                        && geometry.start.first == QPoint(10, 20)
                        && geometry.start.outward == QPoint(-1, 0)
                        && geometry.end.outward == QPoint(1, 0),
                "Annotation handles use validated terminal edges");
        const auto tokenAt = [&](int selected, int chapter = 0) {
            auto position = sourceposition::fromToken(word.start);
            position["offset"] = selected;
            position["cfi"] =
                    QString("epubcfi(/6/%1!/4/2/1:%2)").arg((chapter + 1) * 2).arg(selected);
            position["href"] = chapter == 2 ? "two" : "one";
            return sourceposition::token(position);
        };
        const QVector<QPair<QString, QString>> ranges{ { word.start, word.end },
                                                       { word.end, tokenAt(130) },
                                                       { tokenAt(10), tokenAt(16) },
                                                       { word.end, word.start },
                                                       { word.start, tokenAt(16, 2) } };
        require(document.intersectingRanges(word.start, word.end, ranges) == QVector<int>({ 0, 4 }),
                "Overlap checks are half-open, forward and aware of spine order");
        require(document.visibleRanges(ranges) == QVector<int>({ 0, 1, 3, 4 })
                        && !document.rangeVisible(tokenAt(10), tokenAt(16)),
                "Visibility resolves source spans against the displayed page");
        const auto queried = QFileInfo(queryLog).size();
        for (const auto &range : ranges)
            document.rangeRects(range.first, range.second);
        require(QFileInfo(queryLog).size() == queried,
                "Painting a visibility batch reuses geometry without per-highlight IPC");
        require(document.pageRangeRects(0, ranges).at(2) == word.rectangles
                        && document.pageRangeRects(0, ranges).at(0).isEmpty(),
                "Preview geometry uses its target page rather than the visible page");
        require(!document.annotationGeometry(word.start, tokenAt(16, 2)).end.visible,
                "Cross-spine passages never invent a handle at the page boundary");
        require(document.wordAt({ -1, 30 }).text.isEmpty()
                        && document.textRange("/legacy", word.end).text.isEmpty(),
                "Invalid points and legacy anchors are not sent as source queries");
        const auto token = document.annotationTokenAt({ 30, 30 });
        require(token.start == word.start && token.end == word.end && !token.punctuation,
                "The editor obtains its token from the active WebKit spine");
        const auto selected = document.annotationRange(token.start, token.end, token.start,
                                                       token.end, false, true);
        require(selected.chosenStart == token.start && selected.chosenEnd == token.end
                        && selected.sentenceStartPolicy && !selected.sentenceEndPolicy,
                "The proxy keeps the chosen endpoints and punctuation policies");
        require(document.annotationRange(token.start, token.end, tokenAt(10, 2), tokenAt(16, 2))
                        .text.isEmpty(),
                "The editor cannot extend a selection across spine entries");
        const auto edges = document.annotationPageEdges();
        require(!edges.first.text.isEmpty() && !edges.continuesBefore && !edges.continuesAfter,
                "TOC and spine boundaries stop automatic selection turns");
        QVector<SearchHit> hits;
        for (int selected = 0; selected < document.pages(); ++selected) {
            const auto batch = document.searchPage("source", selected);
            require(batch.nextPage == selected + 1 && batch.hits.size() == 1
                            && batch.hits.first().page == selected,
                    "Search streams matches in whole-book page order");
            hits += batch.hits;
        }
        require(document.state().value("source_position").toObject() == anchor
                        && document.page() == 1
                        && document.image(document.page()).cacheKey() == queryImage.cacheKey(),
                "Cross-spine text queries preserve the source position and immutable frame");
        require(document.goTo(hits.last().start) && document.page() == 5,
                "Search hits navigate through source positions");
        require(document.goTo(sourceposition::token(anchor)), "Return after source query");
        require(document.searchPage("source", 5).hits.size() == 1,
                "Leave the helper on a different spine before reflow");
        require(document.goTo("#middle") && document.page() == 1,
                "Relative links use the displayed source after metadata/search scans");
        require(document.searchPage("source", 5).hits.size() == 1,
                "Inspect another spine before checking reflow restoration");
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
        const auto beforeReflow = QFileInfo(frameLog).size();
        require(document.applyAppearance({ { "font_pixels", 30 } }, { 800, 600 }), "resize/style");
        require(QFileInfo(frameLog).size() - beforeReflow <= 5 * 6,
                "Reflow metadata scans retain a bounded visible-frame allocation budget");
        require(document.page() == 2
                        && document.state().value("source_position").toObject() == anchor,
                "Reflow restores the source character, not an old page number");
        require(document.chapters().at(1).page == 2, "TOC pages follow reflow");
        require(document.pagesOf({ markedPage }) == QVector<int>{ 2 }
                        && sourceposition::fromToken(document.pageStart(1)).value("offset").toInt()
                                == 50,
                "Bookmarks reflow from their source anchors and summary caches invalidate");
        require(document.image(document.page()).size() == QSize(800, 600), "resized image");
        require(document.image(5).size() == QSize(800, 600) && document.page() == 2,
                "The same preview index uses the new viewport after reflow");
        require(document.annotationPageEdges().continuesAfter,
                "An adjacent page without a chapter marker remains selectable");
        require(!document.rangeRects(word.start, word.end).isEmpty(), "Prime reflowed geometry");
        require(document.applyAppearance({ { "font_pixels", 31 } }, { 800, 600 })
                        && document.page() == 2,
                "Typography can change while retaining the same page number");
        const auto styled = QFileInfo(queryLog).size();
        require(!document.rangeRects(word.start, word.end).isEmpty()
                        && QFileInfo(queryLog).size() > styled,
                "Typography invalidates cached source visibility and handle geometry");
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
    qunsetenv("KOBALT_TEST_WEBKIT_FRAME_LOG");
    qunsetenv("KOBALT_TEST_WEBKIT_QUERY_LOG");
    qputenv("KOBALT_TEST_WEBKIT_MODE", "wrong-preview-page");
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "Preview guard fixture");
        const auto anchor = document.state().value("source_position");
        const auto visible = document.image(0);
        require(document.image(1).isNull() && document.page() == 0
                        && document.state().value("source_position") == anchor
                        && document.image(0).cacheKey() == visible.cacheKey()
                        && document.resourceError().isEmpty(),
                "A frame for the wrong preview page cannot replace the displayed frame");
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
    qputenv("KOBALT_TEST_WEBKIT_MODE", "bad-resolve");
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "Overlap guard fixture");
        const auto word = document.wordAt({ 30, 30 });
        require(!document.intersectingRanges(word.start, word.end, { { word.start, word.end } })
                         .isEmpty(),
                "A failed source lookup cannot authorize an overlapping highlight save");
    }
    qputenv("KOBALT_TEST_WEBKIT_MODE", "bad-query-edge");
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "Handle guard fixture");
        const auto word = document.wordAt({ 30, 30 });
        const auto geometry = document.annotationGeometry(word.start, word.end);
        require(!geometry.start.visible && geometry.end.visible && !geometry.rectangles.isEmpty(),
                "An invalid endpoint normal cannot become a visible handle");
    }
    for (const auto *mode : { "bad-query-source", "bad-query-token", "bad-query-rect" }) {
        qputenv("KOBALT_TEST_WEBKIT_MODE", mode);
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "Query guard fixture");
        const auto position = document.state().value("source_position");
        require(document.wordAt({ 30, 30 }).text.isEmpty(),
                "Reject malformed query geometry or anchors");
        require(document.state().value("source_position") == position && document.go(1),
                "Rejected queries preserve the reading position and helper lifecycle");
    }
    for (const auto *mode : { "bad-size", "bad-path", "bad-id", "bad-ready", "bad-locator",
                              "bad-layout-pages", "bad-layout-locator", "bad-layout-viewport" }) {
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
    const auto faultFilePath = helperState.filePath("fault");
    qputenv("KOBALT_TEST_WEBKIT_FAULT_FILE", QFile::encodeName(faultFilePath));
    const auto fault = [&](const QByteArray &request) {
        QFile file(faultFilePath);
        require(file.open(QIODevice::WriteOnly) && file.write(request) == request.size(),
                "Arm renderer failure");
    };
    const auto helperPid = [&] {
        QFile file(pidFilePath);
        require(file.open(QIODevice::ReadOnly), "Read owned helper PID");
        return file.readAll().toInt();
    };
    {
        WebKitDocument document({}, "/unused.epub");
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr), "Recovery fixture");
        int retries = 0;
        bool current = true, cancelRetry = false;
        document.setCurrent([&] { return current; });
        setDiagnosticObserver([&](const QJsonObject &entry) {
            if (entry.value("event").toString() == "webkit-recovery-start") {
                ++retries;
                if (cancelRetry)
                    current = false;
            }
        });
        const int originalPid = helperPid();
        fault("RELEASE exit");
        require(document.go(1) && document.page() == 1 && document.resourceError().isEmpty()
                        && retries == 1 && helperPid() != originalPid,
                "Death after a page reply restarts once and retries the same turn");
        const auto intraPage = document.wordAt({ 30, 30 }).start;
        require(!intraPage.isEmpty() && document.goTo(intraPage) && document.page() == 1,
                "Resume from a logical anchor inside the displayed page");
        const auto displayed = document.image(1);
        const auto state = document.state();
        fault("QUERY terminated");
        require(document.searchPage("source", 5).hits.size() == 1 && retries == 2
                        && document.state() == state
                        && document.image(1).cacheKey() == displayed.cacheKey(),
                "A query interrupted in another spine recovers without replacing the frame");
        fault("GOTO exit repeat");
        require(!document.go(3) && retries == 3 && document.page() == 1 && document.state() == state
                        && document.image(1).cacheKey() == displayed.cacheKey(),
                "A second crash preserves the displayed page and stops the retry");
        const auto failed = document.resourceError();
        require(!failed.isEmpty() && document.wordAt({ 30, 30 }).text.isEmpty() && retries == 3
                        && document.resourceError() == failed,
                "Frame capture cannot start another recovery or hide the error");
        require(QFile::remove(faultFilePath) && document.go(3) && document.resourceError().isEmpty()
                        && retries == 3,
                "A later navigation can restart after a bounded failure");
        const auto beforeLayout = document.state();
        const auto beforeImage = document.image(3);
        const int beforePages = document.pages();
        fault("STYLE exit repeat");
        require(!document.applyAppearance({ { "font_pixels", 32 } }, { 800, 600 })
                        && document.state() == beforeLayout
                        && document.viewport() == QSize(600, 800) && document.pages() == beforePages
                        && document.image(3).cacheKey() == beforeImage.cacheKey() && retries == 4,
                "A failed reflow restores the prior viewport, pagination, pixels and anchor");
        require(QFile::remove(faultFilePath) && document.go(3), "Restore after failed reflow");
        fault("GOTO invalid");
        require(!document.go(4) && retries == 4 && document.page() == 3,
                "Invalid protocol replies are rejected without retry");
        require(document.go(3), "Restore after protocol rejection");
        const auto anchor = document.state().value("source_position");
        fault("RESIZE exit");
        require(document.applyAppearance({ { "font_pixels", 32 } }, { 800, 600 })
                        && document.viewport() == QSize(800, 600) && document.page() == 6
                        && document.state().value("source_position") == anchor && retries == 5,
                "A recoverable resize replays the complete reflow at its source anchor");
        const int beforeCancellation = helperPid();
        fault("GOTO exit");
        cancelRetry = true;
        require(!document.go(7) && retries == 6 && document.page() == 6
                        && helperPid() == beforeCancellation
                        && document.resourceError().contains("cancelled"),
                "Revocation before recovery prevents a new helper from opening");
        setDiagnosticObserver({});
        current = true;
        require(document.go(6), "Open a fresh helper with active access");
        const auto retained = document.image(6);
        current = false;
        require(document.searchPage("source", 0).hits.isEmpty()
                        && document.resourceError().isEmpty(),
                "An abandoned background query does not become a reader error");
        current = true;
        require(!document.wordAt({ 30, 30 }).text.isEmpty()
                        && document.image(6).cacheKey() == retained.cacheKey(),
                "A later selection can reopen the helper on the same page");
    }
    {
        const auto path = helperState.filePath("replaced.epub");
        require(QFile::copy(QStringLiteral(KOBALT_TEST_BOOK), path), "Own the reopen fixture");
        QFile original(path);
        Publication publication;
        require(original.open(QIODevice::ReadOnly) && publication.inspect(original, nullptr),
                "Inspect the verified publication");
        original.close();
        WebKitDocument document({}, path);
        document.setFingerprint(publication.fingerprint());
        require(document.openResources({}, {}, { 600, 800 }, {}, nullptr),
                "Open verified publication");
        const auto anchor = document.state().value("source_position");
        const int pid = helperPid();
        document.close();
        require(original.open(QIODevice::WriteOnly | QIODevice::Append)
                        && original.write("changed") == 7,
                "Replace publication content while the renderer is closed");
        original.close();
        require(!document.go(1) && document.state().value("source_position") == anchor
                        && document.resourceError().contains("publication changed")
                        && helperPid() == pid,
                "A restart does not reopen changed content or publish a new anchor");
    }
    qunsetenv("KOBALT_TEST_WEBKIT_FAULT_FILE");
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
        require(sourceposition::fromToken(session->navigationPosition())
                        == session->state().value("source_position").toObject(),
                "Transient navigation positions carry source tokens without exposing a legacy field");
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
        {
            qputenv("KOBALT_TEST_WEBKIT_MODE", "hang-query");
            auto searching = std::make_unique<ReaderSession>(test::readerFonts(), "Libron");
            auto searchRequest = request;
            searchRequest.saved = {};
            searchRequest.lease = std::make_shared<SourceLease>();
            searchRequest.resources = localResourceFactory(path, searchRequest.lease);
            searching->open(searchRequest, [&](OpeningResult result) {
                require(result.ready() && searching->experimentalBackend(),
                        "Search cancellation fixture");
                opened.quit();
            });
            opened.exec();
            bool published = false, cancelled = false;
            QElapsedTimer cancelTime;
            QTimer arrival;
            QObject::connect(&arrival, &QTimer::timeout, &opened, [&] {
                if (!QFile::exists(pidFilePath + ".query"))
                    return;
                arrival.stop();
                cancelTime.start();
                cancelled = true;
                searching->cancelSearch();
                searching->updated = [&] { opened.quit(); };
                require(searching->turn(1), "A page turn can follow a cancelled helper query");
            });
            searching->search("source", [&](SearchBatch) { published = true; });
            arrival.start(10);
            opened.exec();
            require(cancelled && !published && searching->page() == 1
                            && searching->error().isEmpty() && cancelTime.elapsed() < 5000,
                    "Cancellation interrupts IPC, drops stale search output and restores reading");
            qunsetenv("KOBALT_TEST_WEBKIT_MODE");
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

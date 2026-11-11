#include "check.h"
#include "publication/local_resources.h"
#include "reading/reader_session.h"
#include "reading/reading_book.h"
#include "reading/source_position.h"
#include "reading/bookmark_session.h"
#include "reading/highlight_session.h"
#include "storage/profile_database.h"
#include "support/main_thread.h"
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QSqlQuery>
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
        const auto totalWords = session->totalWords();
        qint64 chapterWords = 0;
        const auto graph = session->chapterGraph();
        for (const auto &chapter : graph) {
            require(chapter.words >= 0, "TOC destination has a word count");
            chapterWords += chapter.words;
        }
        require(graph.size() == session->chapters().size() && chapterWords > 0
                        && totalWords >= chapterWords,
                "Chapter word intervals are bounded by the publication count");
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
        require(session->totalWords() == totalWords
                        && session->chapterGraph().last().words == graph.last().words,
                "Reflow retains logical word counts");
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
        require(sourceposition::fromToken(session->navigationPosition()) == anchor,
                "Transient return navigation carries the backend source point");
        QTemporaryDir annotations;
        const storage::Paths annotationPaths{ annotations.path() };
        const auto fingerprint = session->sourceHash();
        BookmarkStore legacy(annotationPaths.annotations(), request.identity, fingerprint, "Book",
                             annotationPaths.database());
        require(legacy.load(), "Prepare legacy bookmark fixture");
        const Bookmark oldMark{
            "/legacy/retained", "Legacy chapter", "Legacy text",
            QDateTime::fromString("2026-11-11T00:00:00Z", Qt::ISODate).toUTC()
        };
        legacy.setBookmarks({ oldMark });
        require(legacy.save(), "Save legacy bookmark fixture");
        auto bookmarks = std::make_unique<BookmarkSession>(
                *session,
                [&] {
                    return bookmarkLayoutKey(session->pages(), session->state(),
                                             session->image().size());
                },
                annotationPaths.annotations(), request.identity, fingerprint, "Book",
                annotationPaths.database());
        require(bookmarks->sync() && bookmarks->bookmarks().isEmpty(),
                "Source bookmark namespace starts independently");
        bookmarks->failed = [] { require(false, "Source bookmark save failed"); };
        QTimer bookmarkReady;
        QObject::connect(&bookmarkReady, &QTimer::timeout, &completion, [&] {
            if (!bookmarks->saving() && bookmarks->resolved() && bookmarks->bookmarks().size() == 1)
                completion.quit();
        });
        {
            mainthread::Interaction reading([] { return true; });
            require(bookmarks->toggle(session->page()) == BookmarkSession::Toggle::Adding,
                    "Source bookmark creation queued");
            bookmarkReady.start(10);
            completion.exec();
            bookmarkReady.stop();
            require(bookmarks->marked(session->page()), "Source page is bookmarked");
        }
        const auto markedSource = bookmarks->bookmarks().first().position;
        require(!sourceposition::fromToken(markedSource).isEmpty(),
                "Bookmark uses a versioned source point");
        require(legacy.load() && legacy.bookmarks() == QVector<Bookmark>{ oldMark },
                "Source bookmark save preserves the original CREngine list");
        const auto beforeSearch = digest(session->image());
        QVector<SearchHit> hits;
        const int searchBeats = beats;
        session->search("Trevelyan", [&](SearchBatch batch) {
            hits += batch.hits;
            if (batch.done)
                completion.quit();
        });
        completion.exec();
        require(!hits.isEmpty() && beats > searchBeats,
                "Source search streams without blocking the GUI");
        require(session->state().value("source_position").toObject() == anchor
                        && digest(session->image()) == beforeSearch,
                "Cross-spine search preserves the displayed frame and logical source anchor");
        require(session->goTo(hits.first().start), "Open a source search result");
        completion.exec();
        QVector<QRect> boxes;
        session->rangeRects(hits.first().start, hits.first().end, [&](QVector<QRect> result) {
            boxes = result;
            completion.quit();
        });
        completion.exec();
        require(!boxes.isEmpty(), "Search result has visible DOM range geometry");
        session->wordAt(boxes.first().center(), [&](WordSelection word) {
            require(word.text.contains("Trevelyan", Qt::CaseInsensitive)
                            && !sourceposition::fromToken(word.start).isEmpty(),
                    "Hit testing resolves the actual displayed word");
            completion.quit();
        });
        completion.exec();
        session->textRange(hits.first().start, hits.first().end, [&](TextRange range) {
            require(range.text.compare("Trevelyan", Qt::CaseInsensitive) == 0,
                    "Source range extracts search text");
            completion.quit();
        });
        completion.exec();
        AnnotationToken token;
        session->annotationTokenAt(boxes.first().center(), [&](AnnotationToken result) {
            token = result;
            completion.quit();
        });
        completion.exec();
        require(token.text.contains("Trevelyan", Qt::CaseInsensitive) && !token.punctuation,
                "The reader's pointer path resolves a WebKit annotation token");
        TextRange selected;
        session->annotationRange(
                token.start, token.end, token.start, token.end,
                [&](TextRange range) {
                    selected = range;
                    completion.quit();
                },
                true, true);
        completion.exec();
        require(selected.text.contains(token.text) && selected.chosenStart == token.start
                        && selected.chosenEnd == token.end && selected.sentenceStartPolicy
                        && selected.sentenceEndPolicy,
                "The reader's selection range retains the chosen source boundaries");
        session->annotationGeometry(selected.start, selected.end, [&](AnnotationGeometry geometry) {
            require(!geometry.rectangles.isEmpty() && geometry.start.visible
                            && geometry.end.visible,
                    "Source highlight exposes actual terminal handles");
            completion.quit();
        });
        completion.exec();
        HighlightStore legacyHighlights(annotationPaths.annotations(), request.identity,
                                        fingerprint, "Book", annotationPaths.database());
        require(legacyHighlights.load(), "Prepare legacy highlight fixture");
        const Highlight oldHighlight{ "legacy-id", "/legacy/start", "/legacy/end", "Legacy excerpt",
                                      {},          oldMark.created };
        legacyHighlights.setHighlights({ oldHighlight });
        require(legacyHighlights.save(), "Save legacy highlight fixture");
        auto highlights = std::make_unique<HighlightSession>(
                *session,
                [&] {
                    return bookmarkLayoutKey(session->pages(), session->state(),
                                             session->image().size());
                },
                annotationPaths.annotations(), request.identity, fingerprint, "Book",
                annotationPaths.database());
        require(highlights->sync() && highlights->highlights().isEmpty(),
                "Source highlights start independently of CREngine");
        QTimer highlightReady;
        QObject::connect(&highlightReady, &QTimer::timeout, &completion, [&] {
            if (!highlights->saving() && highlights->sync() && highlights->resolved())
                completion.quit();
        });
        {
            mainthread::Interaction reading([] { return true; });
            highlights->save(
                    selected.start, selected.end, {},
                    [&](bool ok) {
                        require(ok, "Source highlight save queued");
                        completion.quit();
                    },
                    selected.chosenEnd, selected.sentenceEndPolicy, selected.chosenStart,
                    selected.sentenceStartPolicy);
            completion.exec();
            if (highlights->saving() || !highlights->sync()) {
                highlightReady.start(10);
                completion.exec();
                highlightReady.stop();
            }
            require(highlights->highlights().size() == 1 && highlights->error().isEmpty(),
                    "Background source highlight save completed");
            highlights->save(selected.start, selected.end, {}, [&](bool ok) {
                require(!ok && highlights->error() == "overlap"
                                && highlights->highlights().size() == 1,
                        "Overlapping source highlights are rejected without replacing the list");
                completion.quit();
            });
            completion.exec();
        }
        require(legacyHighlights.load() && legacyHighlights.highlights().size() == 1
                        && highlightJson(legacyHighlights.highlights().first())
                                == highlightJson(oldHighlight),
                "Source highlight save preserves the original CREngine list");
        ReaderMarkupRequest markup;
        markup.saved.append({ selected.start, selected.end, "gray", "shaded" });
        const int previewPage = session->page();
        const QSize previewSize{ 180, 240 };
        const auto plainPreview =
                session->image().scaled(previewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        session->setMarkup(markup);
        session->captureMarkup([&](bool ok) {
            require(ok && session->markup().saved.size() == 1
                            && !session->markup().saved.first().rectangles.isEmpty(),
                    "Production markup captures saved source highlight geometry");
            completion.quit();
        });
        completion.exec();
        require(session->goTo(sourceposition::token(anchor)), "Return from search result");
        completion.exec();
        require(session->page() != previewPage, "Use an off-screen preview target");
        const auto beforePreview = digest(session->image());
        const auto thumbnail = [&] {
            QImage image;
            session->previewPage(previewPage, previewSize, [&](QImage result) {
                image = std::move(result);
                completion.quit();
            });
            completion.exec();
            return image;
        };
        const auto markedPreview = thumbnail();
        require(markedPreview.size() == plainPreview.size() && markedPreview != plainPreview,
                "The off-screen thumbnail includes its saved source highlight");
        session->setMarkup({});
        require(thumbnail() == plainPreview,
                "Removing preview markup exposes the same page pixels");
        require(session->state().value("source_position").toObject() == anchor
                        && digest(session->image()) == beforePreview,
                "Thumbnail preparation preserves the published frame and position");
        bool obsoletePreview = false;
        session->previewPage(previewPage, previewSize, [&](QImage) { obsoletePreview = true; });
        session->cancelPagePreview();
        ReaderSession::drain([&] { completion.quit(); });
        completion.exec();
        require(!obsoletePreview, "A cancelled preview is discarded");
        session->setMarkup(markup);
        auto appearance = session->persistentState();
        appearance["font_pixels"] = 32;
        session->apply(appearance, { 800, 600 });
        completion.exec();
        require(session->error().isEmpty()
                        && session->state().value("source_position").toObject() == anchor,
                "Typography and viewport changes retain the exact source anchor");
        if (!bookmarks->sync()) {
            bookmarkReady.start(10);
            completion.exec();
            bookmarkReady.stop();
        }
        require(bookmarks->resolved() && bookmarks->page(0) >= 0,
                "Source bookmark resolves after reflow");
        require(session->goTo(markedSource), "Bookmark source destination queued");
        completion.exec();
        require(session->error().isEmpty() && session->page() == bookmarks->page(0),
                "Source bookmark navigation uses the reflowed page");
        require(session->goTo(sourceposition::token(anchor)), "Return after source bookmark");
        completion.exec();
        require(session->goTo(selected.start), "Open source highlight after reflow");
        completion.exec();
        require(session->markup().saved.size() == 1
                        && !session->markup().saved.first().rectangles.isEmpty(),
                "Source highlight is painted on its reflowed page");
        if (!highlights->sync()) {
            highlightReady.start(10);
            completion.exec();
            highlightReady.stop();
        }
        require(highlights->page(0) == session->page(), "Source highlight list follows reflow");
        highlights.reset();
        HighlightStore coldHighlights(annotationPaths.annotations(), request.identity, fingerprint,
                                      "Book", annotationPaths.database(),
                                      AnnotationPositionFormat::SourceV1);
        require(coldHighlights.load() && coldHighlights.highlights().size() == 1
                        && coldHighlights.highlights().first().start == selected.start
                        && coldHighlights.highlights().first().text == selected.text
                        && coldHighlights.highlights().first().chosenStart == selected.chosenStart
                        && coldHighlights.highlights().first().chosenEnd == selected.chosenEnd
                        && coldHighlights.highlights().first().sentenceStartPolicy
                        && coldHighlights.highlights().first().sentenceEndPolicy,
                "Cold store restores source highlight text, anchors and punctuation provenance");
        require(session->goTo(sourceposition::token(anchor)), "Return after source highlight");
        completion.exec();
        {
            const auto size = session->image().size();
            const auto original = digest(session->image());
            const int originalMargin = session->margin();
            const int changedMargin = originalMargin + 20;
            ReaderSession draft(EngineFontInputs{}, "serif");
            draft.openPreview(*session, size, [&](QString error) {
                require(error.isEmpty(), "Open the WebKit appearance preview");
                completion.quit();
            });
            completion.exec();
            require(draft.experimentalBackend() && digest(draft.image()) == original
                            && draft.navigationPosition() == session->navigationPosition(),
                    "Preview initially shares the displayed WebKit frame");
            draft.updated = [&] { completion.quit(); };
            require(draft.style(draft.fontSize(), draft.spacing(), draft.fontFace(), changedMargin),
                    "Queue a real WebKit margin preview");
            completion.exec();
            require(draft.error().isEmpty() && draft.margin() == changedMargin
                            && draft.navigationPosition() == sourceposition::token(anchor)
                            && digest(draft.image()) != original
                            && session->margin() == originalMargin
                            && digest(session->image()) == original,
                    "Draft reflow changes the preview and preserves the original source frame");
            session->apply(draft.requestedState(), size);
            completion.exec();
            require(session->error().isEmpty() && session->margin() == changedMargin
                            && session->navigationPosition() == draft.navigationPosition()
                            && digest(session->image()) == digest(draft.image()),
                    "Applying the draft uses the same layout at the same source point");
        }
        bookmarks.reset();
        BookmarkStore coldMarks(annotationPaths.annotations(), request.identity, fingerprint,
                                "Book", annotationPaths.database(),
                                AnnotationPositionFormat::SourceV1);
        require(coldMarks.load() && coldMarks.bookmarks().size() == 1
                        && coldMarks.bookmarks().first().position == markedSource,
                "Cold annotation store restores source bookmarks");
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
        const QJsonObject standalonePace{
            { "schema", 1 }, { "words", 40 }, { "milliseconds", 60000 }, { "samples", 1 }
        };
        require(storage::writeReadingSpeed(paths, storage::ReadingSpeedRecord::Standalone,
                                           standalonePace),
                "Prepare the separate standalone pace record");
        const auto paceFrame = [](const ReaderSession &reader) -> ReadingActivity {
            return { reader.navigationPosition(),
                     0,
                     reader.page(),
                     reader.pages(),
                     reader.turnDirection(),
                     reader.totalWords(),
                     reader.chapterGraph(),
                     reader.graphSelection() };
        };
        require(session->goTo(contents.first().position), "Open the start of the reading interval");
        completion.exec();
        const auto intervalStart = session->navigationPosition();
        const auto intervalEnd = sourceposition::token(anchor);
        require(!intervalStart.isEmpty() && intervalStart != intervalEnd,
                "History fixture has two distinct source anchors");
        session->updated = {};
        qint64 activeMilliseconds = 0;
        {
            ReadingBook book(std::move(session), paths.bookState(identity), paths, identity,
                             fingerprint, nullptr, {}, {});
            bool awake = true;
            mainthread::Interaction reading([&] { return awake; });
            book.observeReading();
            require(book.readingActive() && book.tracksReadingHistory(),
                    "WebKit starts an active source reading interval");
            require(book.estimates().bookMilliseconds < 0,
                    "A fresh WebKit profile does not use the standalone pace");
            awake = false;
            book.sleepReading({});
            const auto pausedAt = book.readingMilliseconds();
            ReaderSession::drain([&] { completion.quit(); });
            completion.exec();
            require(!book.readingActive() && book.readingMilliseconds() == pausedAt,
                    "Sleep freezes the WebKit history interval");
            awake = true;
            book.observeReading();
            require(book.readingActive(), "A short sleep resumes the WebKit history interval");
            const int words = wordEstimates::pageWords(paceFrame(*book.session()));
            require(words >= 2 && words < 800, "The first chapter supports a bounded pace sample");
            const auto target = pausedAt + qMax<qint64>(2100, words * 100);
            QTimer sampleTimer;
            QObject::connect(&sampleTimer, &QTimer::timeout, &completion, [&] {
                if (book.readingMilliseconds() >= target)
                    completion.quit();
            });
            sampleTimer.start(10);
            completion.exec();
            sampleTimer.stop();
            book.session()->updated = [&] { completion.quit(); };
            require(book.session()->turn(1), "Turn to finish a timed WebKit page");
            completion.exec();
            book.observeReading();
            require(book.estimates().bookMilliseconds > 0 && book.estimates().learningPace,
                    "A timed WebKit page enables an estimate");
            require(book.session()->goTo(intervalEnd), "Navigate during the recorded interval");
            completion.exec();
            book.observeReading();
            book.session()->updated = {};
            require(book.session()->navigationPosition() == intervalEnd,
                    "The final anchor is displayed");
            book.saveReadingActivity();
            book.stopReading(true);
            activeMilliseconds = book.readingMilliseconds();
            require(!book.readingActive(), "Hiding WebKit stops the session clock");
            book.checkpointForClose([&](bool saved) {
                require(saved, "Background source checkpoint saved");
                completion.quit();
            });
            completion.exec();
        }
        const auto historyBook =
                BookmarkStore::bookKey(identity, fingerprint, AnnotationPositionFormat::SourceV1);
        require(activeMilliseconds > 0
                        && storage::profileTransaction(
                                profile.path(),
                                [&](QSqlDatabase &db) {
                                    QSqlQuery row(db);
                                    return row.exec(
                                                   "SELECT book, start_position, end_position, active_ms, status FROM reading_sessions")
                                            && row.next() && row.value(0).toString() == historyBook
                                            && row.value(1).toString() == intervalStart
                                            && row.value(2).toString() == intervalEnd
                                            && row.value(3).toLongLong() == activeMilliseconds
                                            && row.value(4).toString() == "completed"
                                            && !row.next();
                                }),
                "Close persists a completed interval with source anchors and active duration");
        auto saved = storage::readBookState(paths, identity).object;
        auto preserved = saved;
        preserved.remove("backend_positions");
        require(preserved == original, "CREngine and unknown record fields preserved");
        const auto webkitPace =
                storage::readReadingSpeed(paths, storage::ReadingSpeedRecord::WebKit).object;
        require(webkitPace.value("samples").toInt() == 1
                        && storage::readReadingSpeed(paths, storage::ReadingSpeedRecord::Standalone)
                                        .object
                                == standalonePace,
                "Only the timed WebKit turn changed its separate pace record");
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
        ReadingSpeed restoredPace;
        require(restoredPace.restore(webkitPace), "Restore the WebKit speed sample");
        const auto expected = restoredPace.estimates(paceFrame(*session));
        const int reopenedPages = session->pages();
        {
            ReadingBook reopened(std::move(session), paths.bookState(identity), paths, identity,
                                 fingerprint, nullptr, {}, {});
            const auto restored = reopened.estimates();
            require(expected.bookMilliseconds > 0
                            && restored.bookMilliseconds == expected.bookMilliseconds
                            && restored.learningPace,
                    "A cold WebKit reader reuses its own saved pace");
        }
        QFile report(output + "/session.json");
        require(report.open(QIODevice::WriteOnly), "Open firmware report");
        report.write(QJsonDocument(QJsonObject{ { "backend", "webkit" },
                                                { "gui_heartbeats", beats },
                                                { "pages", reopenedPages },
                                                { "first_sha256", QString::fromLatin1(first) },
                                                { "navigation", true },
                                                { "resize", true },
                                                { "sleep_resume", true },
                                                { "source_position", anchor },
                                                { "persistent_position", true },
                                                { "toc_navigation", true },
                                                { "total_words", double(totalWords) },
                                                { "chapter_words", double(chapterWords) },
                                                { "session_clock", true },
                                                { "source_history", true },
                                                { "source_pace_restored", true },
                                                { "appearance_preview", true },
                                                { "source_search", true },
                                                { "word_hit_testing", true },
                                                { "text_ranges", true },
                                                { "source_bookmarks", true },
                                                { "source_highlights", true },
                                                { "pointer_selection_provenance", true },
                                                { "highlight_overlap_guard", true },
                                                { "highlight_reflow_and_reopen", true },
                                                { "annotation_terminal_edges", true },
                                                { "page_previews", true },
                                                { "highlighted_thumbnail", true },
                                                { "legacy_bookmarks_preserved", true },
                                                { "search_preserves_frame", true },
                                                { "saved_data_preserved", true } })
                             .toJson());
        lease->cancel();
        ReaderSession::drain([&] { app.quit(); });
    });
    return app.exec();
}

#include "webkit_document.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include "support/diagnostics.h"
#include "reading/source_position.h"
#include "publication/publication.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QUrl>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kobalt {
namespace {
QJsonObject queryPosition(const QJsonObject &position)
{
    QJsonObject result;
    // href is bound by the proxy's spine, and preserved optional fields are
    // irrelevant to DOM resolution. Keep bounded queries inside KWRP's limit.
    for (const auto *key : { "version", "cfi", "offset", "before", "after" })
        result[key] = position.value(key);
    return result;
}

constexpr int maximumReply = 1024 * 1024;
constexpr int maximumChapterPages = 100001; // M2 bounds GOTO to 100000.
constexpr int maximumWordCountOffsets = 1024; // Keep QUERY below its 16 KiB request limit.

QByteArray encoded(const QString &text)
{
    return text.toUtf8().toBase64();
}
} // namespace

WebKitDocument::WebKitDocument(EngineFontInputs fonts, QString path)
    : fonts_(std::move(fonts)), path_(std::move(path))
{
}

WebKitDocument::~WebKitDocument()
{
    close();
}

bool WebKitDocument::fail(QString error)
{
    error_ = std::move(error);
    return false;
}

void WebKitDocument::collectProcessError()
{
    if (!process_)
        return;
    processError_ += process_->readAllStandardError();
    if (processError_.size() > 4096)
        processError_.remove(0, processError_.size() - 4096);
}

bool WebKitDocument::processExited()
{
    collectProcessError();
    diagnostic("webkit-helper-exited",
               qt::object({ { "exit_code", process_ ? process_->exitCode() : -1 },
                            { "stderr", QString::fromUtf8(processError_) } }));
    return rendererFailed("WebKit helper exited");
}

bool WebKitDocument::rendererFailed(QString error)
{
    close();
    restartableFailure_ = true;
    return fail(std::move(error));
}

bool WebKitDocument::start()
{
    mainthread::blocking("start WebKit helper");
    if (process_ && process_->state() != QProcess::NotRunning)
        return true;
    if (current_ && !current_())
        return fail("WebKit opening cancelled");
    if (auto *previous = rendererOwner_->document)
        previous->close();
    process_ = std::make_unique<QProcess>();
    rendererOwner_->document = this;
    // Never inherit Nickel's library injection into the supervisor. The
    // supervisor constructs the worker's private environment itself.
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto *name : { "LD_PRELOAD", "LD_LIBRARY_PATH", "QT_PLUGIN_PATH" })
        environment.remove(name);
    process_->setProcessEnvironment(environment);
    QString launcher = QString::fromLocal8Bit(qgetenv("KOBALT_WEBKIT_LAUNCHER"));
    if (launcher.isEmpty())
        launcher = "/mnt/onboard/.adds/kobalt-webkit/bin/kobalt-webkit";
    QStringList arguments{ "--memory-frames" };
    const auto mode = qgetenv("KOBALT_WEBKIT_MODE");
    if (mode == "experiment")
        arguments << "--allow-unsandboxed-experiment";
    else if (mode == "device-unsandboxed")
        arguments << "--allow-unsandboxed-device";
    else if (!mode.isEmpty() && mode != "required")
        return fail("Invalid WebKit sandbox mode");
    const auto prefix = QString::fromLocal8Bit(qgetenv("KOBALT_WEBKIT_QEMU_PREFIX"));
    if (!prefix.isEmpty()) {
        arguments << "--qemu-prefix" << prefix;
        environment.insert("QEMU_LD_PREFIX", prefix);
        process_->setProcessEnvironment(environment);
    }
    input_.clear();
    processError_.clear();
    runtime_.clear();
    ready_ = "KWRP/1 0 READY "
            + (mode.isEmpty() || mode == "required" ? QByteArray("sandbox-required") : mode);
    readySeen_ = false;
    request_ = 0;
    process_->start(launcher, arguments);
    if (!process_->waitForStarted(5000))
        return fail("Cannot start WebKit helper: " + process_->errorString());
    return true;
}

void WebKitDocument::close()
{
    if (!process_) {
        // A book's sleep or close also stops the helper while its draft
        // owns it. Disposing of an unused draft leaves the book warm.
        if (!appearancePreview_ && rendererOwner_->document && rendererOwner_->document != this)
            rendererOwner_->document->close();
        return;
    }
    mainthread::blocking("stop WebKit helper");
    if (process_->state() != QProcess::NotRunning) {
        // SIGTERM reaches the supervisor; it reaps the complete WebKit tree.
        process_->terminate();
        if (!process_->waitForFinished(5000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
    process_.reset();
    rendererOwner_->document = nullptr;
    opened_ = false;
    input_.clear();
}

bool WebKitDocument::command(const QByteArray &body, RenderedFrame *pageResult, QJsonObject *json)
{
    mainthread::blocking("WebKit IPC");
    if (current_ && !current_()) {
        close();
        return fail("WebKit request cancelled");
    }
    if (!process_ || process_->state() == QProcess::NotRunning)
        return processExited();
    const unsigned id = ++request_;
    const QByteArray prefix = "KWRP/1 " + QByteArray::number(id) + ' ';
    const auto message = prefix + body + '\n';
    if (message.size() > 8192)
        return fail("Oversized WebKit request");
    if (process_->write(message) != message.size())
        return rendererFailed("Cannot send WebKit request");
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 60000) {
        if (current_ && !current_()) {
            close();
            return fail("WebKit request cancelled");
        }
        if (!input_.contains('\n')) {
            process_->waitForReadyRead(50);
            input_ += process_->readAllStandardOutput();
        }
        collectProcessError();
        if (input_.size() > maximumReply) {
            close();
            return fail("Oversized WebKit reply");
        }
        const int end = input_.indexOf('\n');
        if (end >= 0) {
            const auto line = input_.left(end);
            input_.remove(0, end + 1);
            // READY is the only unsolicited message in KWRP/1.
            if (line.startsWith("KWRP/1 0 READY ")) {
                if (line != ready_ || readySeen_) {
                    close();
                    return fail("Unexpected WebKit sandbox handshake");
                }
                readySeen_ = true;
                continue;
            }
            if (readySeen_ && line == "KWRP/1 0 ERROR web-process-terminated")
                return rendererFailed("WebKit: web-process-terminated");
            if (!readySeen_ || !line.startsWith(prefix)) {
                close();
                return fail("Invalid WebKit reply identity/version");
            }
            const auto reply = line.mid(prefix.size());
            if (reply == "ERROR web-process-terminated" || reply == "ERROR render-timeout")
                return rendererFailed("WebKit: " + QString::fromUtf8(reply.mid(6)));
            if (reply.startsWith("ERROR "))
                return fail("WebKit: " + QString::fromUtf8(reply.mid(6)));
            if (json) {
                if (!reply.startsWith("OK JSON "))
                    return fail("Invalid WebKit query reply");
                const auto base64 = reply.mid(8), bytes = QByteArray::fromBase64(base64);
                QJsonParseError parse;
                const auto document = QJsonDocument::fromJson(bytes, &parse);
                if (bytes.toBase64() != base64 || parse.error != QJsonParseError::NoError
                    || !document.isObject())
                    return fail("Invalid WebKit query JSON");
                *json = document.object();
                return true;
            }
            if (pageResult) {
                if (!reply.startsWith("OK PAGE "))
                    return fail("Invalid WebKit page reply");
                RenderedFrame decoded;
                const bool valid = readFrame(QString::fromUtf8(reply.mid(8)), id, decoded);
                const auto error = error_;
                const bool released = command("RELEASE " + QByteArray::number(id));
                if (valid && released) {
                    *pageResult = std::move(decoded);
                    return true;
                }
                close();
                if (!valid) {
                    restartableFailure_ = false;
                    error_ = error;
                }
                return false;
            }
            if (reply != "OK RELEASED" && reply != "OK CLOSED")
                return fail("Invalid WebKit acknowledgement");
            return true;
        }
        if (process_->state() == QProcess::NotRunning)
            return processExited();
    }
    return rendererFailed("WebKit request timed out");
}

bool WebKitDocument::readFrame(const QString &path, unsigned id, RenderedFrame &result)
{
    // The supervisor creates a private tmpfs directory. Refuse arbitrary paths,
    // symlinks, devices and dimensions before mapping any helper-owned bytes.
    const QFileInfo info(path);
    const auto directory = info.absolutePath();
    const auto stem = QString("frame-%1").arg(id);
    if (!directory.startsWith("/dev/shm/kobalt-webkit-")
        || directory.mid(QString("/dev/shm/").size()).contains('/')
        || info.fileName() != stem + ".json" || info.isSymLink() || info.canonicalFilePath() != path
        || (!runtime_.isEmpty() && runtime_ != directory))
        return fail("Invalid WebKit frame path");
    QFile metadata(path);
    if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > maximumReply)
        return fail("Cannot read WebKit metadata");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(metadata.read(maximumReply + 1), &parse);
    const auto frame = document.object();
    const int width = frame.value("width").toInt(), height = frame.value("height").toInt();
    const int stride = frame.value("stride").toInt();
    const auto pixels = frame.value("pixels").toString();
    if (parse.error != QJsonParseError::NoError || frame.value("version").toInt() != 1
        || frame.value("frame").toDouble() != id
        || frame.value("format").toString() != "BGRA8-premultiplied"
        || QSize(width, height) != size_ || width < 100 || height < 100 || width > 2048
        || height > 2048 || stride < width * 4 || stride > width * 4 + 4096
        || pixels != directory + '/' + stem + ".bgra"
        || QSysInfo::ByteOrder != QSysInfo::LittleEndian)
        return fail("Invalid WebKit frame metadata");
    const int descriptor =
            ::open(QFile::encodeName(pixels).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
        return fail("Cannot open WebKit pixels");
    struct stat status;
    const bool valid = !fstat(descriptor, &status) && S_ISREG(status.st_mode)
            && status.st_uid == getuid() && status.st_size == qint64(stride) * height;
    QFile raw;
    if (!valid || !raw.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return fail("Invalid WebKit pixel storage");
    }
    auto *bytes = raw.map(0, status.st_size);
    if (!bytes)
        return fail("Cannot map WebKit pixels");
    // Detach before RELEASE. Every ReaderFrame keeps its own immutable pixels.
    auto image = QImage(bytes, width, height, stride, QImage::Format_ARGB32_Premultiplied).copy();
    raw.unmap(bytes);
    const auto layout = frame.value("layout").toObject();
    const int count = layout.value("pages").toInt(), selected = layout.value("page").toInt(-1);
    const auto spine = frame.value("document").toObject().value("spine").toArray();
    const int index = frame.value("spine").toInt(-1);
    if (image.isNull() || count < 1 || count > maximumChapterPages || selected < 0
        || selected >= count || index < 0 || index >= spine.size() || spine.size() > 4096)
        return fail("Invalid WebKit layout");
    auto position = layout.value("locator").toObject();
    position["href"] = spine.at(index).toString();
    const auto prefixes = frame.value("document").toObject().value("spine_cfi").toArray();
    if (!sourceposition::valid(position) || prefixes.size() != spine.size()
        || !position.value("cfi").toString().startsWith("epubcfi(" + prefixes.at(index).toString()
                                                        + '!'))
        return fail("Invalid WebKit source position");
    runtime_ = directory;
    result.metadata = frame;
    result.image = std::move(image);
    result.position = std::move(position);
    return true;
}

bool WebKitDocument::render(const QByteArray &request)
{
    RenderedFrame result;
    if (!command(request, &result))
        return false;
    frame_ = std::move(result.metadata);
    image_ = std::move(result.image);
    position_ = std::move(result.position);
    helperMoved_ = false;
    error_.clear();
    return true;
}

bool WebKitDocument::openResources(std::shared_ptr<ResourceCache>, const QMap<QString, qint64> &,
                                   QSize size, const QJsonObject &saved, QString *error,
                                   OpeningProgress progress, OpeningResult *result,
                                   std::function<void()>)
{
    close();
    rendererOwner_ = std::make_shared<RendererOwner>();
    appearancePreview_ = false;
    frame_ = {};
    image_ = {};
    offsets_.clear();
    linear_.clear();
    chapters_.clear();
    chapterGraphOrder_.clear();
    counts_ = {};
    chapterStartsAtPage_.clear();
    position_ = {};
    summaries_.clear();
    sourcePoints_.clear();
    geometryPage_ = -1;
    queriesBlocked_ = false;
    restartableFailure_ = false;
    bool supported = true;
    const auto positions = saved.value("backend_positions");
    if (!positions.isUndefined() && !positions.isObject())
        supported = fail("Unsupported saved source positions");
    const auto slot = positions.toObject().value("webkit");
    if (!slot.isUndefined()) {
        if (!slot.isObject()
            || !sourceposition::valid(slot.toObject().value("position").toObject()))
            supported = fail("Unsupported saved WebKit source position");
        else
            position_ = slot.toObject().value("position").toObject();
    }
    appearance_ = saved;
    appearance_["font_pixels"] =
            qBound(8, saved.value("font_pixels").toInt(defaultFontPixels()), 96);
    appearance_["line_spacing"] =
            qBound(100, saved.value("line_spacing").toInt(defaultLineSpacing), 300);
    appearance_["line_spacing_version"] = 1;
    const auto face = saved.value("font_face").toString();
    appearance_["font_face"] =
            face == "serif" || face == "sans-serif" || face == "monospace" ? face : QString();
    appearance_["text_alignment"] = QStringLiteral("publisher");
    appearance_["margin_pixels"] = qBound(
            0, saved.value("margin_pixels").toInt(defaultMarginPixels()), maximumMarginPixels());
    const int vertical = defaultVerticalMarginPixels(appearance_.value("margin_pixels").toInt());
    for (const auto *key : { "top_margin_pixels", "bottom_margin_pixels" })
        appearance_[key] = qBound(0, saved.value(key).toInt(vertical), maximumMarginPixels());
    size_ = size;
    if (progress)
        progress(OpeningStage::Pages, 0);
    const bool opened = supported && ensureOpen();
    if (!opened)
        close();
    if (error)
        *error = error_;
    if (!opened && result)
        *result =
                OpeningResult::failed(OpeningFailureKind::InvalidContent, "opening.webkit", error_);
    if (opened && progress)
        progress(OpeningStage::Ready, 100);
    return opened;
}

bool WebKitDocument::openPreview(Document &source, QSize size, QString *error)
{
    auto *book = dynamic_cast<WebKitDocument *>(&source);
    if (!book || book == this || book->appearancePreview_ || book->image_.isNull()) {
        if (error)
            *error = QStringLiteral("No WebKit page to preview");
        return false;
    }
    close();
    appearancePreview_ = true;
    rendererOwner_ = book->rendererOwner_;
    path_ = book->path_;
    fingerprint_ = book->fingerprint_;
    size_ = book->size_;
    appearance_ = book->appearance_;
    frame_ = book->frame_;
    image_ = book->image_;
    position_ = book->position_;
    offsets_ = book->offsets_;
    linear_ = book->linear_;
    chapters_.clear();
    chapterGraphOrder_.clear();
    counts_ = {};
    chapterStartsAtPage_.clear();
    summaries_.clear();
    sourcePoints_.clear();
    geometryPage_ = -1;
    queriesBlocked_ = false;
    error_.clear();
    const bool opened = size == size_ || applyAppearance(appearance_, size);
    if (error)
        *error = opened ? QString() : error_;
    return opened;
}

bool WebKitDocument::takeWarmRenderer()
{
    auto *previous = rendererOwner_->document;
    if (!previous || previous == this || !previous->opened_ || !previous->process_
        || previous->process_->state() != QProcess::Running || previous->size_ != size_
        || previous->position_ != position_
        || !appearance::same(previous->appearance_, appearance_))
        return false;
    process_ = std::move(previous->process_);
    input_ = std::move(previous->input_);
    ready_ = std::move(previous->ready_);
    processError_ = std::move(previous->processError_);
    runtime_ = std::move(previous->runtime_);
    readySeen_ = previous->readySeen_;
    request_ = previous->request_;
    helperMoved_ = previous->helperMoved_;
    previous->opened_ = false;
    rendererOwner_->document = this;
    opened_ = true;
    return true;
}

bool WebKitDocument::ensureOpen()
{
    if (opened_)
        return process_ && process_->state() != QProcess::NotRunning ? true : processExited();
    if (takeWarmRenderer())
        return true;
    const auto restore = position_;
    const auto previousFrame = frame_;
    const auto previousImage = image_;
    const int previousPage = page(), previousCount = chapterPages();
    if (!previousFrame.isEmpty() && !checkPublication())
        return false;
    if (!start()
        || !render("OPEN " + encoded(QFileInfo(path_).absoluteFilePath()) + ' '
                   + QByteArray::number(size_.width()) + ' ' + QByteArray::number(size_.height())))
        return false;
    if (!style())
        return false;
    if (offsets_.isEmpty() && !scanBook(true))
        return false;
    if (!restore.isEmpty() && !locate(restore))
        return false;
    if (!previousFrame.isEmpty()) {
        // Cached whole-book pagination is valid only for the same layout.
        if (position_ != restore || page() != previousPage || chapterPages() != previousCount
            || (!previousImage.isNull() && image_ != previousImage))
            return fail("WebKit could not restore the displayed page");
        if (!previousImage.isNull())
            image_ = previousImage;
    }
    opened_ = true;
    return true;
}

bool WebKitDocument::checkPublication()
{
    if (fingerprint_.isEmpty())
        return true;
    QFile file(path_);
    Publication publication;
    if (!file.open(QIODevice::ReadOnly) || !publication.inspect(file, nullptr)
        || publication.fingerprint() != fingerprint_)
        return fail("WebKit publication changed while reading");
    return true;
}

bool WebKitDocument::runOperation(const std::function<bool()> &operation, bool changesLayout)
{
    // Commands can render temporary frames during OPEN, STYLE, RESIZE or
    // restoration. Keep the last complete display until the operation succeeds.
    const auto frame = frame_, position = position_, appearance = appearance_;
    const auto image = image_;
    const auto size = size_;
    const auto offsets = offsets_;
    const auto linear = linear_;
    const auto chapters = chapters_;
    const auto chapterGraphOrder = chapterGraphOrder_;
    const auto boundaries = chapterStartsAtPage_;
    const auto run = [&] {
        restartableFailure_ = false;
        if (current_ && !current_()) {
            close();
            return fail("WebKit request cancelled");
        }
        if (!ensureOpen() || !operation())
            return false;
        if (current_ && !current_()) {
            close();
            return fail("WebKit request cancelled");
        }
        queriesBlocked_ = false;
        error_.clear();
        return true;
    };
    const auto rollback = [&] {
        if (changesLayout || restartableFailure_ || !opened_)
            close();
        queriesBlocked_ = !opened_;
        frame_ = frame;
        position_ = position;
        appearance_ = appearance;
        image_ = image;
        size_ = size;
        offsets_ = offsets;
        linear_ = linear;
        chapters_ = chapters;
        chapterGraphOrder_ = chapterGraphOrder;
        chapterStartsAtPage_ = boundaries;
        helperMoved_ = opened_;
    };
    if (run())
        return true;
    const bool retry = restartableFailure_ && !frame.isEmpty() && (!current_ || current_());
    rollback();
    if (!retry)
        return false;
    diagnostic("webkit-recovery-start", { { "error", error_ } });
    const bool completed = run();
    if (!completed)
        rollback();
    diagnostic("webkit-recovery-end",
               qt::object({ { "completed", completed }, { "page", page() }, { "error", error_ } }),
               completed ? DiagnosticSeverity::Activity : DiagnosticSeverity::Error);
    return completed;
}

bool WebKitDocument::locate(const QJsonObject &position)
{
    const int index = sourceSpine(position);
    if (index < 0)
        return fail("Invalid source position");
    return render("LOCATE " + QByteArray::number(index) + ' '
                  + QJsonDocument(position).toJson(QJsonDocument::Compact).toBase64());
}

int WebKitDocument::sourceSpine(const QJsonObject &position) const
{
    if (!sourceposition::valid(position))
        return -1;
    const auto paths = frame_.value("document").toObject().value("spine").toArray();
    const auto prefixes = frame_.value("document").toObject().value("spine_cfi").toArray();
    for (int index = 0; index < paths.size(); ++index)
        if (paths.at(index).toString() == position.value("href").toString()
            && position.value("cfi").toString().startsWith("epubcfi("
                                                           + prefixes.at(index).toString() + '!'))
            return index;
    return -1;
}

bool WebKitDocument::scanBook(bool countWords)
{
    const auto description = frame_.value("document").toObject();
    const auto navigation = description.value("navigation").toArray();
    const auto paths = description.value("spine").toArray();
    const auto linear = description.value("linear").toArray();
    const QUrl origin(frame_.value("layout").toObject().value("source").toString());
    if (navigation.size() > 4096)
        return fail("Oversized WebKit table of contents");
    QVector<QVector<int>> targets(paths.size());
    QVector<QString> fragments(navigation.size());
    for (int i = 0; i < navigation.size(); ++i) {
        const QUrl destination(navigation.at(i).toObject().value("href").toString());
        if (destination.scheme() != origin.scheme() || destination.host() != origin.host())
            continue;
        const auto path = destination.path().mid(1);
        for (int index = 0; index < paths.size(); ++index) {
            if (paths.at(index).toString() == path) {
                targets[index].append(i);
                fragments[i] = destination.fragment(QUrl::FullyDecoded);
                break;
            }
        }
    }

    QVector<int> offsets{ 0 };
    QVector<bool> flags;
    QVector<ChapterLink> links(navigation.size());
    QMap<QString, bool> boundaries;
    QVector<int> orderedTargets;
    QVector<QPair<int, qint64>> prefixes;
    qint64 total = 0;
    for (int index = 0; index < paths.size(); ++index) {
        const auto inspected = layout(index);
        if (inspected.isEmpty())
            return false;
        int loadedPage = 0;
        QVector<QPair<int, int>> anchors;
        for (const int i : targets.at(index)) {
            const auto fragment = fragments.at(i);
            const auto target = fragment.isEmpty() ? inspected : layout(index, fragment);
            if (target.isEmpty()) {
                if (!error_.endsWith("missing-link-target"))
                    return false;
                error_.clear();
                continue;
            }
            const auto item = navigation.at(i).toObject();
            const auto locator = target.value("locator").toObject();
            const auto position = sourceToken(locator, index);
            const int offset = locator.value("offset").toInt();
            const int selected = target.value("page").toInt();
            if (!fragment.isEmpty())
                loadedPage = selected;
            boundaries[position] =
                    offset <= target.value("location").toObject().value("start").toInt();
            links[i] = { item.value("title").toString().simplified().left(512), position,
                         offsets.last() + selected, qBound(0, item.value("depth").toInt(), 256) };
            anchors.append({ offset, i });
        }
        // Order by the source offset, retaining TOC order at one destination.
        std::sort(anchors.begin(), anchors.end());
        for (const auto &anchor : anchors)
            orderedTargets.append(anchor.second);
        // The helper still has this spine loaded, so counting needs no extra
        // document load, layout or frame. An older helper can omit the counts.
        if (countWords && !countSpineWords(index, loadedPage, anchors, prefixes, total)) {
            if (restartableFailure_)
                return false;
            countWords = false;
            error_.clear();
        }
        offsets.append(offsets.last() + inspected.value("pages").toInt());
        flags.append(linear.size() == paths.size() ? linear.at(index).toBool() : true);
    }
    QVector<ChapterLink> contents;
    QVector<int> contentsIndex(links.size(), -1);
    DocumentWordCounts counts;
    QVector<qint64> chapterWords(navigation.size(), -1);
    if (countWords) {
        counts.total = total;
        for (int i = 0; i < prefixes.size(); ++i) {
            const auto prefix = prefixes.at(i);
            const auto end = i + 1 < prefixes.size() ? prefixes.at(i + 1).second : total;
            chapterWords[prefix.first] = end - prefix.second;
        }
    }
    for (int i = 0; i < links.size(); ++i) {
        if (links.at(i).position.isEmpty())
            continue;
        if (countWords)
            counts.chapters.append({ links.at(i).position, chapterWords.at(i) });
        contentsIndex[i] = contents.size();
        contents.append(links.at(i));
    }
    if (countWords)
        counts_ = std::move(counts);
    offsets_ = std::move(offsets);
    linear_ = std::move(flags);
    chapters_ = std::move(contents);
    chapterGraphOrder_.clear();
    for (const int target : orderedTargets)
        chapterGraphOrder_.append(contentsIndex.at(target));
    chapterStartsAtPage_ = std::move(boundaries);
    return true;
}

bool WebKitDocument::countSpineWords(int index, int page, const QVector<QPair<int, int>> &anchors,
                                     QVector<QPair<int, qint64>> &prefixes, qint64 &total)
{
    int spineTotal = -1, previousPrefix = 0, first = 0;
    do {
        const int end = qMin(first + maximumWordCountOffsets, int(anchors.size()));
        QJsonArray offsets;
        for (int i = first; i < end; ++i)
            offsets.append(anchors.at(i).first);
        QJsonObject reply;
        if (!queryPage(index, page,
                       qt::object({ { "kind", "word-counts" }, { "offsets", offsets } }), {},
                       reply))
            return false;
        const int words = reply.value("total").toInt(-1);
        const auto values = reply.value("prefixes").toArray();
        if (words < 0 || (spineTotal >= 0 && spineTotal != words)
            || !reply.value("prefixes").isArray() || values.size() != end - first)
            return false;
        spineTotal = words;
        for (int i = first; i < end; ++i) {
            const int prefix = values.at(i - first).toInt(-1);
            if (prefix < previousPrefix || prefix > spineTotal)
                return false;
            prefixes.append({ anchors.at(i).second, total + prefix });
            previousPrefix = prefix;
        }
        first = end;
    } while (first < anchors.size());
    total += spineTotal;
    return true;
}

bool WebKitDocument::style(QSize legacyMarginViewport)
{
    const bool independentMargins = frame_.value("vertical_margins").toBool();
    const int side = qBound(0, appearance_.value("margin_pixels").toInt(defaultMarginPixels()),
                            qMin(maximumMarginPixels(), 2048));
    const auto viewport = legacyMarginViewport.isValid() ? legacyMarginViewport : size_;
    const int margin = independentMargins
            ? side
            : qMin(side, (qMin(viewport.width(), viewport.height()) - 21) / 2);
    const auto family = appearance_.value("font_face").toString();
    const QByteArray generic = family == "serif" || family == "sans-serif" || family == "monospace"
            ? family.toLatin1()
            : QByteArray("publisher");
    QByteArray command =
            "STYLE "
            + QByteArray::number(
                    qBound(8, appearance_.value("font_pixels").toInt(defaultFontPixels()), 96))
            + ' '
            + QByteArray::number(
                    qBound(100, appearance_.value("line_spacing").toInt(defaultLineSpacing), 300))
            + ' ' + QByteArray::number(margin) + " 0 "
            + QByteArray::number(qBound(0, appearance_.value("word_spacing").toInt(100) - 100, 100))
            + ' ' + generic + ' ' + (appearance_.value("dark_mode").toBool() ? "dark" : "light");
    if (independentMargins) {
        const int vertical = defaultVerticalMarginPixels(side);
        for (const auto *key : { "top_margin_pixels", "bottom_margin_pixels" }) {
            const int inset = qBound(0, appearance_.value(key).toInt(vertical),
                                     qMin(maximumMarginPixels(), 2048));
            command += ' ' + QByteArray::number(inset);
        }
    }
    return render(command);
}

bool WebKitDocument::applyAppearance(const QJsonObject &settings, QSize size, OpeningProgress)
{
    return runOperation([&] { return applyLayout(settings, size); }, true);
}

bool WebKitDocument::applyLayout(const QJsonObject &settings, QSize size)
{
    if (helperMoved_ && !locate(position_))
        return false;
    const auto restore = position_;
    const int previewSpine = spine();
    const int previousPages = chapterPages();
    const bool resized = size.isValid() && size != size_;
    if (resized) {
        summaries_.clear();
        sourcePoints_.clear();
        geometryPage_ = -1;
        if (size.width() < 100 || size.height() < 100 || size.width() > 2048
            || size.height() > 2048)
            return fail("WebKit viewport must be between 100 and 2048 pixels");
        const auto smaller = size_.boundedTo(size);
        const int limit = (qMin(smaller.width(), smaller.height()) - 21) / 2;
        if (!frame_.value("vertical_margins").toBool()
            && appearance_.value("margin_pixels").toInt(defaultMarginPixels()) > limit
            && !style(smaller))
            return false;
        size_ = size;
        if (!render("RESIZE " + QByteArray::number(size.width()) + ' '
                    + QByteArray::number(size.height())))
            return false;
    }
    if (appearance::same(appearance_, settings) && !resized)
        return true;
    summaries_.clear();
    sourcePoints_.clear();
    geometryPage_ = -1;
    appearance_ = settings;
    if (!style())
        return false;
    if (!appearancePreview_)
        return scanBook(false) && locate(restore);
    if (!locate(restore) || spine() != previewSpine)
        return false;
    // The draft only paginates the displayed spine. Keep its page indices
    // usable for source geometry; the book scans all spines on apply.
    const int changed = chapterPages() - previousPages;
    for (int index = previewSpine + 1; index < offsets_.size(); ++index)
        offsets_[index] += changed;
    return true;
}

int WebKitDocument::spine() const
{
    return frame_.value("spine").toInt();
}

int WebKitDocument::chapterPage() const
{
    return frame_.value("layout").toObject().value("page").toInt();
}

int WebKitDocument::chapterPages() const
{
    return frame_.value("layout").toObject().value("pages").toInt();
}

int WebKitDocument::page() const
{
    return offsets_.value(spine()) + chapterPage();
}

int WebKitDocument::pages() const
{
    return offsets_.isEmpty() ? chapterPages() : offsets_.last();
}

int WebKitDocument::turnTarget(int from, int delta) const
{
    if (!delta)
        return from;
    int target = from;
    for (int step = 0; step < qAbs(delta); ++step) {
        const int candidate = target + (delta < 0 ? -1 : 1);
        if (candidate < 0 || candidate >= pages())
            break;
        const int sourceIndex = int(std::upper_bound(offsets_.begin(), offsets_.end(), candidate)
                                    - offsets_.begin())
                - 1;
        int index = sourceIndex;
        while (index >= 0 && index < linear_.size() && !linear_.at(index))
            index += delta < 0 ? -1 : 1;
        if (index < 0 || index >= linear_.size())
            break;
        target = linear_.at(sourceIndex)
                ? candidate
                : (delta < 0 ? offsets_.at(index + 1) - 1 : offsets_.at(index));
    }
    return target;
}

QByteArray WebKitDocument::pageCommand(int target) const
{
    const int index =
            int(std::upper_bound(offsets_.begin(), offsets_.end(), target) - offsets_.begin()) - 1;
    return "GOTO " + QByteArray::number(index) + ' '
            + QByteArray::number(target - offsets_.at(index));
}

bool WebKitDocument::go(int target)
{
    return runOperation([&] {
        if (target < 0 || target >= pages())
            return false;
        if (target == page())
            return true;
        const bool moved = render(pageCommand(target));
        if (!moved && (error_.endsWith("end-of-book") || error_.endsWith("start-of-book")))
            error_.clear();
        return moved;
    });
}

bool WebKitDocument::goTo(const QString &href)
{
    return runOperation([&] {
        if (href.startsWith("kobalt-source:"))
            return locate(sourceposition::fromToken(href));
        // Relative links belong to the displayed spine, even after a query
        // that loaded a different content document in the helper.
        if (helperMoved_ && !locate(position_))
            return false;
        return render("LINK " + encoded(href));
    });
}

QImage WebKitDocument::image(int selected)
{
    if (selected == page()) {
        // Frame capture after a failed turn must retain the last page and error.
        if (image_.isNull() && !queriesBlocked_)
            runOperation([&] { return !image_.isNull() || locate(position_); });
        return image_;
    }
    if (queriesBlocked_ || selected < 0 || selected >= pages() || offsets_.size() < 2)
        return {};
    const auto previousError = error_;
    RenderedFrame preview;
    const bool ready = runOperation([&] {
        helperMoved_ = true;
        if (!command(pageCommand(selected), &preview))
            return false;
        const int index = preview.metadata.value("spine").toInt(-1);
        const auto layout = preview.metadata.value("layout").toObject();
        if (index < 0 || index + 1 >= offsets_.size() || selected < offsets_.at(index)
            || selected >= offsets_.at(index + 1)
            || layout.value("page").toInt(-1) != selected - offsets_.at(index)
            || layout.value("pages").toInt() != offsets_.at(index + 1) - offsets_.at(index))
            return fail("WebKit preview does not match the requested page");
        return true;
    });
    // The scrubber owns the scaled thumbnails. A failed preview does not
    // replace the reading frame or its error with a thumbnail result.
    error_ = previousError;
    return ready ? std::move(preview.image) : QImage();
}

void WebKitDocument::releaseImages()
{
    image_ = {};
}

qint64 WebKitDocument::cachedImageKey(int selected) const
{
    return selected == page() ? image_.cacheKey() : 0;
}

QStringList WebKitDocument::fontFaces()
{
    return { "", "serif", "sans-serif", "monospace" };
}

QString WebKitDocument::title() const
{
    return QFileInfo(path_).completeBaseName();
}

QJsonObject WebKitDocument::state() const
{
    auto result = appearance_;
    result.remove("position");
    result.remove("dom_version");
    result["backend"] = QStringLiteral("webkit");
    result["source_position"] = position_;
    return result;
}

QJsonObject WebKitDocument::resourceStats() const
{
    return qt::object({ { "allocated_bytes", double(image_.bytesPerLine()) * image_.height() },
                        { "pending_images", 0 } });
}

QVector<PageLink> WebKitDocument::pageLinks()
{
    QVector<PageLink> links;
    for (const auto &value : frame_.value("layout").toObject().value("links").toArray()) {
        const auto link = value.toObject();
        const QRect rectangle(
                qFloor(link.value("x").toDouble()), qFloor(link.value("y").toDouble()),
                qCeil(link.value("width").toDouble()), qCeil(link.value("height").toDouble()));
        links.append({ link.value("href").toString(),
                       { rectangle.intersected(QRect(QPoint(), size_)) },
                       LinkKind::Passage });
    }
    return links;
}

LinkDetails WebKitDocument::linkDetails(const QString &href, bool)
{
    LinkDetails result;
    result.kind = LinkKind::Passage;
    result.target = href;
    return result;
}

ChapterProgress WebKitDocument::chapterProgress() const
{
    const auto document = frame_.value("document").toObject();
    const auto documents = document.value("spine").toArray();
    const auto path = spine() < documents.size() ? documents.at(spine()).toString() : QString();
    QString chapter = frame_.value("layout").toObject().value("title").toString();
    for (const auto &entry : document.value("navigation").toArray()) {
        const auto item = entry.toObject();
        if (QUrl(item.value("href").toString()).path().mid(1) == path
            && !item.value("title").toString().trimmed().isEmpty()) {
            chapter = item.value("title").toString();
            break;
        }
    }
    if (chapter.trimmed().isEmpty())
        chapter = title();
    const auto selected = chapterSelection(chapters_);
    if (selected.current >= 0 && !chapters_.at(selected.current).title.isEmpty())
        chapter = chapters_.at(selected.current).title;
    return { chapter.simplified().left(512),
             selected.current >= 0 ? selected.page : chapterPage() + 1,
             selected.current >= 0 ? selected.pages : chapterPages() };
}

ChapterSelection WebKitDocument::chapterSelection(const QVector<ChapterLink> &contents) const
{
    auto selected = selectContents(contents, position_);
    if (selected.current >= 0) {
        const int first = contents.at(selected.current).page;
        const auto next = selected.next >= 0 ? contents.at(selected.next) : ChapterLink();
        const int end = selected.next >= 0
                ? next.page + (chapterStartsAtPage_.value(next.position, true) ? 0 : 1)
                : pages();
        selected.pages = qMax(1, end - first);
        selected.page = qBound(1, page() - first + 1, selected.pages);
    }
    return selected;
}

QString WebKitDocument::chapterTitle(const QString &position) const
{
    const auto selected = selectContents(chapters_, sourceposition::fromToken(position));
    return selected.current >= 0 ? chapters_.at(selected.current).title : QString();
}

DocumentWordCounts WebKitDocument::wordCounts(std::function<bool()> current)
{
    if (current && !current())
        return {};
    const auto result = counts_;
    counts_.cached = counts_.total >= 0;
    return result;
}

QVector<ChapterLink> WebKitDocument::chapterGraph() const
{
    if (counts_.total < 0)
        return chapterPageLengths(chapters_, pages());
    QVector<ChapterLink> graph;
    graph.reserve(chapters_.size());
    for (const int i : chapterGraphOrder_) {
        auto chapter = chapters_.at(i);
        chapter.words =
                i < counts_.chapters.size() && counts_.chapters.at(i).position == chapter.position
                ? counts_.chapters.at(i).words
                : -1;
        graph.append(std::move(chapter));
    }
    return graph;
}

QJsonObject WebKitDocument::query(int index, int selected, const QJsonObject &request,
                                  const QString &fragment)
{
    if (queriesBlocked_ || (appearancePreview_ && !process_))
        return {};
    const auto previousError = error_;
    QJsonObject result;
    if (runOperation([&] { return queryPage(index, selected, request, fragment, result); }))
        return result;
    if (current_ && !current_()) {
        // Abandon a superseded query without blocking later hits on this page.
        queriesBlocked_ = false;
        error_ = previousError;
    }
    return {};
}

bool WebKitDocument::queryPage(int index, int selected, const QJsonObject &request,
                               const QString &fragment, QJsonObject &result)
{
    if (index < 0 || index >= frame_.value("document").toObject().value("spine").toArray().size()
        || selected < 0 || selected >= maximumChapterPages)
        return false;
    QJsonObject reply;
    // Queries can load another spine without publishing a frame. Absolute page
    // and source navigation still work; appearance changes first restore the
    // visible source anchor before using the helper's relative reflow state.
    helperMoved_ = true;
    if (!command("QUERY " + QByteArray::number(index) + ' ' + QByteArray::number(selected) + ' '
                         + QJsonDocument(request).toJson(QJsonDocument::Compact).toBase64()
                         + (fragment.isEmpty() ? QByteArray() : ' ' + encoded(fragment)),
                 nullptr, &reply))
        return false;
    const auto data = reply.value("data").toObject();
    QUrl expected(frame_.value("layout").toObject().value("source").toString());
    expected.setPath(
            '/'
            + frame_.value("document").toObject().value("spine").toArray().at(index).toString());
    if (reply.value("spine").toDouble(-1) != index
        || (fragment.isEmpty() ? reply.value("page").toDouble(-1) != selected
                               : reply.value("page").toDouble(-1)
                            != data.value("result").toObject().value("page").toDouble(-2))
        || QUrl(data.value("source").toString()) != expected) {
        return fail("Invalid WebKit query source");
    }
    if (data.contains("error")) {
        return fail("WebKit query: " + data.value("error").toString());
    }
    error_.clear();
    result = data.value("result").toObject();
    return true;
}

QJsonObject WebKitDocument::layout(int index, const QString &fragment)
{
    QJsonObject result;
    if (!queryPage(index, 0, qt::object({ { "kind", "layout" } }), fragment, result))
        return {};
    const double count = result.value("pages").toDouble(-1),
                 page = result.value("page").toDouble(-1);
    const auto viewport = result.value("viewport").toObject();
    if (count < 1 || count > maximumChapterPages || std::floor(count) != count || page < 0
        || page >= count || std::floor(page) != page
        || viewport.value("width").toDouble(-1) != size_.width()
        || viewport.value("height").toDouble(-1) != size_.height()
        || sourceToken(result.value("locator").toObject(), index).isEmpty()) {
        if (error_.isEmpty())
            fail("Invalid WebKit layout query");
        return {};
    }
    return result;
}

QString WebKitDocument::sourceToken(QJsonObject position, int index) const
{
    const auto paths = frame_.value("document").toObject().value("spine").toArray();
    if (index < 0 || index >= paths.size())
        return {};
    position["href"] = paths.at(index).toString();
    return sourceSpine(position) == index ? sourceposition::token(position) : QString();
}

QVector<QRect> WebKitDocument::queryRectangles(const QJsonObject &result) const
{
    const auto boxes = result.value("rectangles").toArray();
    if (boxes.size() > 512)
        return {};
    QVector<QRect> rectangles;
    for (const auto &item : boxes) {
        const auto box = item.toObject();
        const double x = box.value("x").toDouble(-1), y = box.value("y").toDouble(-1);
        const double width = box.value("width").toDouble(-1);
        const double height = box.value("height").toDouble(-1);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width)
            || !std::isfinite(height) || x < 0 || y < 0 || width <= 0 || height <= 0
            || x + width > size_.width() || y + height > size_.height())
            return {};
        const auto rect =
                QRectF(x, y, width, height).toAlignedRect().intersected(QRect(QPoint(), size_));
        if (!rect.isEmpty())
            rectangles.append(rect);
    }
    return rectangles;
}

WordSelection WebKitDocument::wordAt(QPoint point)
{
    if (!QRect(QPoint(), size_).contains(point))
        return {};
    const int index = spine();
    const auto result =
            query(index, chapterPage(),
                  qt::object({ { "kind", "word" }, { "x", point.x() }, { "y", point.y() } }));
    const auto start = sourceToken(result.value("start").toObject(), index);
    const auto end = sourceToken(result.value("end").toObject(), index);
    const auto text = result.value("text").toString();
    const auto boxes = queryRectangles(result);
    if (start.isEmpty() || end.isEmpty() || text.isEmpty() || text.size() > 8192 || boxes.isEmpty())
        return {};
    return { text, start, end, boxes };
}

AnnotationToken WebKitDocument::queryAnnotationToken(const QJsonObject &result, int index) const
{
    AnnotationToken token;
    if (!result.value("punctuation").isBool())
        return {};
    const auto read = [&](const char *key) {
        const auto position = sourceToken(result.value(key).toObject(), index);
        return position.size() <= 4096 ? position : QString();
    };
    token.start = read("start");
    token.end = read("end");
    token.text = result.value("text").toString();
    token.rectangles = queryRectangles(result);
    if (token.start.isEmpty() || token.end.isEmpty() || token.text.isEmpty()
        || token.text.size() > 8192 || token.rectangles.isEmpty())
        return {};
    token.punctuation = result.value("punctuation").toBool();
    token.startBeforePunctuation = read("startBeforePunctuation");
    token.endAfterPunctuation = read("endAfterPunctuation");
    return token;
}

AnnotationToken WebKitDocument::annotationTokenAt(QPoint point)
{
    if (!QRect(QPoint(), size_).contains(point))
        return {};
    const int index = spine();
    return queryAnnotationToken(query(index, chapterPage(),
                                      qt::object({ { "kind", "annotation-token" },
                                                   { "x", point.x() },
                                                   { "y", point.y() } })),
                                index);
}

AnnotationToken WebKitDocument::annotationDragTokenAt(QPoint point, const QString &boundary,
                                                      bool start)
{
    if (boundary.isEmpty())
        return annotationTokenAt(point);
    if (!QRect(QPoint(), size_).contains(point))
        return {};
    const int index = spine();
    const auto locator = sourceposition::fromToken(boundary);
    if (sourceSpine(locator) != index || index < 0)
        return {};
    return queryAnnotationToken(query(index, chapterPage(),
                                      qt::object({ { "kind", "drag-token" },
                                                   { "x", point.x() },
                                                   { "y", point.y() },
                                                   { "boundary", queryPosition(locator) },
                                                   { "start", start } })),
                                index);
}

AnnotationPageEdges WebKitDocument::annotationPageEdges()
{
    const int index = spine();
    const auto reply = query(index, chapterPage(), qt::object({ { "kind", "page-edges" } }));
    AnnotationPageEdges result;
    result.first = queryAnnotationToken(reply.value("first").toObject(), index);
    result.last = queryAnnotationToken(reply.value("last").toObject(), index);
    if (result.first.text.isEmpty() || result.last.text.isEmpty())
        return {};
    result.start = queryEndpoint(reply.value("startEdge").toObject());
    result.end = queryEndpoint(reply.value("endEdge").toObject());
    const auto continues = [&](const char *key, int direction) {
        if (chapterPage() + direction < 0 || chapterPage() + direction >= chapterPages())
            return false;
        const auto boundary = reply.value(key).toArray();
        if (boundary.size() != 2)
            return false;
        const auto inner = boundary.at(0).toObject(), outer = boundary.at(1).toObject();
        if (sourceToken(inner, index).isEmpty() || sourceToken(outer, index).isEmpty())
            return false;
        const int from = inner.value("offset").toInt(), to = outer.value("offset").toInt();
        if (direction > 0 ? from >= to : to >= from)
            return false;
        // The helper checks semantic sections. The proxy already owns the
        // resolved TOC, including chapter boundaries within a spine entry.
        for (const auto &chapter : chapters_) {
            if (chapter.page != page() && chapter.page != page() + direction)
                continue;
            const auto marker = sourceposition::fromToken(chapter.position);
            const int offset = marker.value("offset").toInt(-1);
            if (offset > qMin(from, to) && offset <= qMax(from, to) && sourceSpine(marker) == index)
                return false;
        }
        return true;
    };
    result.continuesBefore = continues("before", -1);
    result.continuesAfter = continues("after", 1);
    return result;
}

TextRange WebKitDocument::annotationRange(const QString &start, const QString &end)
{
    return textRange(start, end);
}

TextRange WebKitDocument::annotationRange(const QString &firstStart, const QString &firstEnd,
                                          const QString &lastStart, const QString &lastEnd,
                                          bool endPolicy, bool startPolicy,
                                          std::function<bool()> current)
{
    if (current && !current())
        return {};
    const QStringList tokens{ firstStart, firstEnd, lastStart, lastEnd };
    const int index = sourceSpine(sourceposition::fromToken(firstStart));
    if (index < 0)
        return {};
    QJsonArray positions;
    for (const auto &token : tokens) {
        const auto locator = sourceposition::fromToken(token);
        if (token.size() > 4096 || sourceSpine(locator) != index)
            return {};
        positions.append(queryPosition(locator));
    }
    auto request = qt::object({ { "kind", "annotation-range" },
                                { "first", qt::array({ positions.at(0), positions.at(1) }) },
                                { "last", qt::array({ positions.at(2), positions.at(3) }) },
                                { "startPolicy", startPolicy },
                                { "endPolicy", endPolicy } });
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 6000) {
        // Four deeply nested CFIs can exceed a single KWRP line. Resolve them
        // in bounded batches before sending their verified source offsets.
        const auto resolved = resolvePositions(tokens);
        if (resolved.size() != tokens.size()
            || std::any_of(resolved.begin(), resolved.end(),
                           [index](const SourcePoint &point) { return point.spine != index; }))
            return {};
        request["first"] = qt::array({ resolved[0].offset, resolved[1].offset });
        request["last"] = qt::array({ resolved[2].offset, resolved[3].offset });
    }
    if (current && !current())
        return {};
    const auto reply = query(index, index == spine() ? chapterPage() : 0, request);
    if ((current && !current()) || !reply.value("startPolicy").isBool()
        || !reply.value("endPolicy").isBool())
        return {};
    const auto start = reply.value("start").toObject(), end = reply.value("end").toObject();
    const auto chosenStart = reply.value("chosenStart").toObject(),
               chosenEnd = reply.value("chosenEnd").toObject();
    const auto offset = [](const QJsonObject &locator) {
        return locator.value("offset").toInt(-1);
    };
    if (offset(start) > offset(chosenStart) || offset(chosenStart) >= offset(chosenEnd)
        || offset(chosenEnd) > offset(end))
        return {};
    TextRange result{ sourceToken(start, index),        sourceToken(end, index),
                      reply.value("text").toString(),   sourceToken(chosenEnd, index),
                      sourceToken(chosenStart, index),  reply.value("startPolicy").toBool(),
                      reply.value("endPolicy").toBool() };
    if (result.text.isEmpty() || result.text.size() > 8192)
        return {};
    for (const auto &token : { result.start, result.end, result.chosenStart, result.chosenEnd })
        if (token.isEmpty() || token.size() > 4096)
            return {};
    return result;
}

QJsonObject WebKitDocument::queryRange(const QString &start, const QString &end)
{
    const auto first = sourceposition::fromToken(start), last = sourceposition::fromToken(end);
    const int index = sourceSpine(first);
    if (index < 0 || sourceSpine(last) != index)
        return {};
    return query(index, index == spine() ? chapterPage() : 0,
                 qt::object({ { "kind", "range" },
                              { "start", queryPosition(first) },
                              { "end", queryPosition(last) } }));
}

TextRange WebKitDocument::textRange(const QString &start, const QString &end)
{
    const auto result = queryRange(start, end);
    const int index = sourceSpine(sourceposition::fromToken(start));
    const auto first = sourceToken(result.value("start").toObject(), index);
    const auto last = sourceToken(result.value("end").toObject(), index);
    const auto text = result.value("text").toString();
    if (first.isEmpty() || last.isEmpty() || text.isEmpty() || text.size() > 8192)
        return {};
    return { first, last, text };
}

QVector<QRect> WebKitDocument::rangeRects(const QString &start, const QString &end, bool)
{
    return annotationGeometry(start, end).rectangles;
}

SelectionEndpoint WebKitDocument::queryEndpoint(const QJsonObject &value) const
{
    SelectionEndpoint edge;
    if (!value.value("visible").toBool())
        return edge;
    const auto point = [&](const char *key, QPoint *out) {
        const auto object = value.value(key).toObject();
        const double x = object.value("x").toDouble(-1), y = object.value("y").toDouble(-1);
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > size_.width()
            || y > size_.height())
            return false;
        *out = QPoint(qRound(x), qRound(y));
        return true;
    };
    const auto outward = value.value("outward").toObject();
    const double x = outward.value("x").toDouble(2), y = outward.value("y").toDouble(2);
    // The current helper supports horizontal pages. Reject malformed normals
    // and spans instead of manufacturing a handle from clipped rectangles.
    if (!point("first", &edge.first) || !point("last", &edge.last) || (x != -1 && x != 1) || y != 0
        || edge.first.x() != edge.last.x() || edge.first.y() >= edge.last.y())
        return {};
    edge.outward = QPoint(int(x), 0);
    edge.visible = true;
    return edge;
}

QVector<WebKitDocument::SourcePoint> WebKitDocument::resolvePositions(const QStringList &tokens)
{
    if (tokens.size() > 4002)
        return {};
    QMap<int, QStringList> pending;
    QSet<QString> scheduled;
    for (const auto &token : tokens) {
        if (sourcePoints_.contains(token) || scheduled.contains(token))
            continue;
        const int index = sourceSpine(sourceposition::fromToken(token));
        if (index >= 0 && token.size() <= 4096) {
            pending[index].append(token);
            scheduled.insert(token);
        }
    }
    QMap<QString, SourcePoint> resolved;
    for (auto group = pending.cbegin(); group != pending.cend(); ++group) {
        int next = 0;
        while (next < group.value().size()) {
            if (current_ && !current_())
                return {};
            QJsonArray positions;
            QStringList keys;
            // Keep every encoded KWRP request inside its existing line bound.
            while (next < group.value().size() && keys.size() < 64) {
                const auto &key = group.value().at(next);
                auto candidate = positions;
                candidate.append(queryPosition(sourceposition::fromToken(key)));
                if (QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() > 6000)
                    break;
                positions = std::move(candidate);
                keys.append(key);
                ++next;
            }
            if (keys.isEmpty())
                return {};
            const auto reply =
                    query(group.key(), 0,
                          qt::object({ { "kind", "resolve" }, { "positions", positions } }));
            const auto values = reply.value("positions").toArray();
            if (!reply.value("positions").isArray() || values.size() != keys.size())
                return {};
            for (int i = 0; i < keys.size(); ++i) {
                SourcePoint point;
                if (!values[i].isNull()) {
                    const auto value = values[i].toObject();
                    const auto locator = value.value("locator").toObject();
                    if (!value.value("selectable").isBool()
                        || sourceToken(locator, group.key()).isEmpty())
                        return {};
                    if (value.value("selectable").toBool())
                        point = { group.key(), locator.value("offset").toInt(), locator };
                }
                resolved.insert(keys[i], point);
            }
        }
    }
    QVector<SourcePoint> result;
    for (const auto &key : tokens)
        result.append(resolved.contains(key) ? resolved.value(key) : sourcePoints_.value(key));
    // Bounded, layout-owned cache: repeated highlight paints and overlap checks
    // reuse verified points without another chapter scan.
    for (auto point = resolved.cbegin(); point != resolved.cend() && sourcePoints_.size() < 4002;
         ++point)
        sourcePoints_.insert(point.key(), point.value());
    return result;
}

QVector<int>
WebKitDocument::intersectingRanges(const QString &start, const QString &end,
                                   const QVector<QPair<QString, QString>> &ranges) const
{
    if (ranges.isEmpty() || ranges.size() > 2000)
        return {};
    QStringList tokens{ start, end };
    for (const auto &range : ranges)
        tokens << range.first << range.second;
    auto *self = const_cast<WebKitDocument *>(this);
    const auto points = self->resolvePositions(tokens);
    // A failed helper query must not authorize an edit across a saved passage.
    if (points.size() != tokens.size())
        return { 0 };
    QVector<int> hits;
    const auto &first = points[0], &last = points[1];
    if (first.spine < 0 || last.spine < 0 || first.order() >= last.order())
        return hits;
    for (int i = 0; i < ranges.size(); ++i) {
        const auto &a = points[2 + i * 2], &b = points[3 + i * 2];
        if (a.spine >= 0 && b.spine >= 0 && a.order() < b.order() && a.order() < last.order()
            && first.order() < b.order())
            hits.append(i);
    }
    return hits;
}

QList<AnnotationGeometry>
WebKitDocument::rangeGeometry(int selected, const QVector<QPair<QString, QString>> &ranges)
{
    if (selected < 0 || selected >= pages() || ranges.size() > 2001)
        return {};
    QList<AnnotationGeometry> result;
    for (int i = 0; i < ranges.size(); ++i)
        result.append(AnnotationGeometry{});
    const auto empty = result;
    if (ranges.isEmpty())
        return result;
    if (geometryPage_ != selected) {
        geometryPage_ = selected;
        geometryRectangles_ = 0;
        geometries_.clear();
    }
    QStringList tokens;
    for (const auto &range : ranges)
        tokens << range.first << range.second;
    const auto points = resolvePositions(tokens);
    if (points.size() != tokens.size())
        return result;
    const int index =
            int(std::upper_bound(offsets_.begin(), offsets_.end(), selected) - offsets_.begin())
            - 1;
    const int localPage = selected - offsets_.at(index);
    const auto layout = selected == page()
            ? frame_.value("layout").toObject()
            : query(index, localPage, qt::object({ { "kind", "layout" } }));
    const auto location = layout.value("location").toObject();
    const int begin = location.value("start").toInt(-1), finish = location.value("end").toInt(-1);
    if (begin < 0 || finish < begin)
        return result;
    QJsonArray batch;
    QVector<int> indices;
    QSet<int> truncated;
    QSet<int> completed;
    int remaining = 65536;
    const auto flush = [&] {
        if (indices.isEmpty())
            return true;
        const auto reply =
                query(index, localPage, qt::object({ { "kind", "ranges" }, { "ranges", batch } }));
        const auto rows = reply.value("ranges").toArray();
        if (!reply.value("ranges").isArray() || rows.size() != indices.size())
            return false;
        for (int j = 0; j < rows.size(); ++j) {
            completed.insert(indices[j]);
            if (rows[j].isNull())
                continue;
            const auto row = rows[j].toObject();
            if (sourceToken(row.value("start").toObject(), index).isEmpty()
                || sourceToken(row.value("end").toObject(), index).isEmpty())
                return false;
            const int i = indices[j];
            auto &geometry = result[i];
            geometry.rectangles = queryRectangles(row);
            if (geometry.rectangles.size() > remaining) {
                truncated.insert(i);
                geometry.rectangles.resize(remaining);
            }
            remaining -= geometry.rectangles.size();
            const auto &a = points[i * 2], &b = points[i * 2 + 1];
            const auto first = a.order() <= b.order() ? a : b;
            const auto last = a.order() <= b.order() ? b : a;
            if (first.spine == index)
                geometry.start = queryEndpoint(row.value("startEdge").toObject());
            if (last.spine == index)
                geometry.end = queryEndpoint(row.value("endEdge").toObject());
        }
        batch = {};
        indices.clear();
        return true;
    };
    for (int i = 0; i < ranges.size(); ++i) {
        if (current_ && !current_())
            return {};
        const auto cached = geometries_.constFind(ranges[i]);
        if (cached != geometries_.cend()) {
            result[i] = cached.value();
            if (result[i].rectangles.size() > remaining) {
                truncated.insert(i);
                result[i].rectangles.resize(remaining);
            }
            remaining -= result[i].rectangles.size();
            continue;
        }
        const auto &a = points[i * 2], &b = points[i * 2 + 1];
        if (a.spine < 0 || b.spine < 0 || a.order() == b.order()) {
            completed.insert(i);
            continue;
        }
        const auto first = a.order() <= b.order() ? a : b;
        const auto last = a.order() <= b.order() ? b : a;
        if (last.order() <= qMakePair(index, begin) || first.order() >= qMakePair(index, finish)) {
            completed.insert(i);
            continue;
        }
        const auto range = qt::object(
                { { "start", first.spine == index ? QJsonValue(first.locator) : QJsonValue() },
                  { "end", last.spine == index ? QJsonValue(last.locator) : QJsonValue() } });
        auto candidate = batch;
        candidate.append(range);
        if (candidate.size() > 4
            || QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() > 6000) {
            if (!flush())
                return empty;
            candidate = {};
            candidate.append(range);
        }
        if (QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() > 6000)
            continue;
        batch = std::move(candidate);
        indices.append(i);
        if (remaining <= 0)
            break;
    }
    if (!flush())
        return empty;
    // Markup first asks for visibility, then paints individual passages. Reuse
    // that batch's geometry rather than repeating IPC for every visible mark.
    for (int i = 0; i < ranges.size(); ++i) {
        if (!completed.contains(i) || geometries_.contains(ranges[i]) || truncated.contains(i)
            || geometries_.size() >= 2001
            || geometryRectangles_ + result[i].rectangles.size() > 65536)
            continue;
        geometryRectangles_ += result[i].rectangles.size();
        geometries_.insert(ranges[i], result[i]);
    }
    return result;
}

AnnotationGeometry WebKitDocument::annotationGeometry(const QString &start, const QString &end)
{
    return rangeGeometry(page(), { { start, end } }).value(0);
}

QVector<int> WebKitDocument::visibleRanges(const QVector<QPair<QString, QString>> &ranges) const
{
    const auto geometry = const_cast<WebKitDocument *>(this)->rangeGeometry(page(), ranges);
    QVector<int> indices;
    for (int i = 0; i < geometry.size(); ++i)
        if (!geometry[i].rectangles.isEmpty())
            indices.append(i);
    return indices;
}

bool WebKitDocument::rangeVisible(const QString &start, const QString &end) const
{
    return !visibleRanges({ { start, end } }).isEmpty();
}

QVector<QVector<QRect>>
WebKitDocument::pageRangeRects(int selected, const QVector<QPair<QString, QString>> &ranges)
{
    const auto geometry = rangeGeometry(selected, ranges);
    QVector<QVector<QRect>> result;
    for (const auto &range : geometry)
        result.append(range.rectangles);
    return result;
}

SearchBatch WebKitDocument::searchPage(const QString &text, int selected)
{
    SearchBatch batch;
    batch.nextPage = selected + 1;
    batch.done = selected < 0 || selected + 1 >= pages() || text.trimmed().isEmpty()
            || text.size() > 256;
    if (selected < 0 || selected >= pages() || text.trimmed().isEmpty() || text.size() > 256)
        return batch;
    const int index =
            int(std::upper_bound(offsets_.begin(), offsets_.end(), selected) - offsets_.begin())
            - 1;
    const int localPage = selected - offsets_.at(index);
    const auto result =
            query(index, 0,
                  qt::object({ { "kind", "search" }, { "text", text }, { "page", localPage } }));
    if (!result.value("hits").isArray() || !result.value("limited").isBool()
        || result.value("hits").toArray().size() > 100) {
        batch.done = true;
        return batch;
    }
    batch.limited = result.value("limited").toBool();
    batch.done |= batch.limited;
    for (const auto &value : result.value("hits").toArray()) {
        const auto hit = value.toObject();
        const auto start = sourceToken(hit.value("start").toObject(), index);
        const auto end = sourceToken(hit.value("end").toObject(), index);
        const auto match = hit.value("text").toString();
        const auto before = hit.value("before").toString(), after = hit.value("after").toString();
        if (start.isEmpty() || end.isEmpty() || match.isEmpty() || match.size() > 2048
            || before.size() > 192 || after.size() > 384
            || hit.value("page").toDouble(-1) != localPage) {
            fail("Invalid WebKit search match");
            batch.hits.clear();
            batch.done = true;
            break;
        }
        batch.hits.append({ start, end, before, match, after, selected });
    }
    return batch;
}

QJsonObject WebKitDocument::summary(int selected)
{
    if (selected < 0 || selected >= pages())
        return {};
    // A draft only needs a text sample for the resize placeholder. A
    // paint-triggered summary must not reclaim the helper after apply.
    if (appearancePreview_)
        return selected == page()
                ? qt::object({ { "excerpt", frame_.value("layout").toObject().value("excerpt") } })
                : QJsonObject();
    if (summaries_.contains(selected))
        return summaries_.value(selected);
    const int index =
            int(std::upper_bound(offsets_.begin(), offsets_.end(), selected) - offsets_.begin())
            - 1;
    const auto result =
            query(index, selected - offsets_.at(index), qt::object({ { "kind", "summary" } }));
    const auto start = sourceToken(result.value("start").toObject(), index);
    if (start.isEmpty() || !result.value("excerpt").isString()
        || result.value("excerpt").toString().size() > 200)
        return {};
    auto stored = result;
    stored["token"] = start;
    if (summaries_.size() >= 8)
        summaries_.erase(summaries_.begin());
    summaries_[selected] = stored;
    return stored;
}

QString WebKitDocument::pageStart(int selected) const
{
    // Worker-owned lookups may move the helper DOM, but never the logical
    // source point or published frame. The shared interface is logically const.
    return const_cast<WebKitDocument *>(this)->summary(selected).value("token").toString();
}

QString WebKitDocument::pageExcerpt(int selected) const
{
    return const_cast<WebKitDocument *>(this)->summary(selected).value("excerpt").toString();
}

QVector<int> WebKitDocument::pagesOf(const QStringList &positions, bool endpoints,
                                     QVector<int> *order, std::function<bool()> current) const
{
    auto &self = *const_cast<WebKitDocument *>(this);
    if (order)
        order->fill(-1, positions.size());
    const auto previous = self.current_;
    self.current_ = [previous, current] {
        return (!previous || previous()) && (!current || current());
    };
    const auto resolve = [&]() -> QVector<int> {
        QVector<int> result(positions.size(), -1), indices;
        QVector<QVector<qulonglong>> paths(positions.size());
        QVector<QJsonObject> points;
        for (int i = 0; i < positions.size(); ++i)
            points.append(sourceposition::fromToken(positions.at(i)));
        for (int i = 0; i < positions.size(); ++i)
            if (sourceSpine(points.at(i)) >= 0)
                indices.append(i);
        std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
            return sourceSpine(points.at(a)) < sourceSpine(points.at(b));
        });
        const QRegularExpression digits("[0-9]+");
        for (int i : indices) {
            if (current && !current())
                return {};
            const int index = sourceSpine(points.at(i));
            const auto located =
                    self.query(index, 0,
                               qt::object({ { "kind", "position" },
                                            { "position", points.at(i) },
                                            { "exclusive", endpoints && i % 2 == 1 } }));
            const int local = located.value("page").toInt(-1);
            const auto canonical = located.value("locator").toObject();
            if (local < 0 || local >= offsets_.at(index + 1) - offsets_.at(index)
                || sourceToken(canonical, index).isEmpty())
                continue;
            result[i] = offsets_.at(index) + local;
            auto matched = digits.globalMatch(canonical.value("cfi").toString());
            while (matched.hasNext())
                paths[i].append(matched.next().captured().toULongLong());
        }
        if (order) {
            indices.erase(std::remove_if(indices.begin(), indices.end(),
                                         [&](int i) { return result.at(i) < 0; }),
                          indices.end());
            std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
                return std::lexicographical_compare(paths.at(a).begin(), paths.at(a).end(),
                                                    paths.at(b).begin(), paths.at(b).end());
            });
            for (int i = 0; i < indices.size(); ++i)
                (*order)[indices.at(i)] = i;
        }
        return current && !current() ? QVector<int>() : result;
    };
    const auto result = resolve();
    self.current_ = previous;
    return result;
}

ChapterSelection WebKitDocument::selectContents(const QVector<ChapterLink> &contents,
                                                const QJsonObject &source) const
{
    ChapterSelection result;
    const auto paths = frame_.value("document").toObject().value("spine").toArray();
    const auto prefixes = frame_.value("document").toObject().value("spine_cfi").toArray();
    const auto rank = [&](const QJsonObject &position) {
        for (int index = 0; index < paths.size(); ++index)
            if (paths.at(index).toString() == position.value("href").toString()
                && position.value("cfi").toString().startsWith(
                        "epubcfi(" + prefixes.at(index).toString() + '!'))
                return qMakePair(index, position.value("offset").toInt());
        return qMakePair(-1, -1);
    };
    const auto anchor = rank(source);
    if (anchor.first < 0)
        return result;
    auto previous = qMakePair(-1, -1), next = qMakePair(int(paths.size()), 0);
    for (int index = 0; index < contents.size(); ++index) {
        const auto candidate = rank(sourceposition::fromToken(contents.at(index).position));
        if (candidate.first < 0)
            continue;
        if (candidate <= anchor && candidate >= previous) {
            previous = candidate;
            result.current = index;
        } else if (candidate > anchor && candidate < next) {
            next = candidate;
            result.next = index;
        }
    }
    return result;
}
} // namespace kobalt

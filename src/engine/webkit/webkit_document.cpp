#include "webkit_document.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include "support/diagnostics.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSysInfo>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kobalt {
namespace {
constexpr int maximumReply = 1024 * 1024;
constexpr int maximumChapterPages = 100001; // M2 bounds GOTO to 100000.

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
    return fail("WebKit helper exited");
}

bool WebKitDocument::start()
{
    mainthread::blocking("start WebKit helper");
    if (process_ && process_->state() != QProcess::NotRunning)
        return true;
    if (current_ && !current_())
        return fail("WebKit opening cancelled");
    process_ = std::make_unique<QProcess>();
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
    if (!process_)
        return;
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
    opened_ = false;
    input_.clear();
}

bool WebKitDocument::command(const QByteArray &body, bool pageResult)
{
    mainthread::blocking("WebKit IPC");
    if (!process_ || process_->state() == QProcess::NotRunning)
        return processExited();
    const unsigned id = ++request_;
    const QByteArray prefix = "KWRP/1 " + QByteArray::number(id) + ' ';
    const auto message = prefix + body + '\n';
    if (message.size() > 8192 || process_->write(message) != message.size())
        return fail("Cannot send WebKit request");
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
            if (!readySeen_ || !line.startsWith(prefix)) {
                close();
                return fail("Invalid WebKit reply identity/version");
            }
            const auto reply = line.mid(prefix.size());
            if (reply.startsWith("ERROR "))
                return fail("WebKit: " + QString::fromUtf8(reply.mid(6)));
            if (pageResult) {
                if (!reply.startsWith("OK PAGE "))
                    return fail("Invalid WebKit page reply");
                const bool valid = readFrame(QString::fromUtf8(reply.mid(8)), id);
                const auto error = error_;
                const bool released = command("RELEASE " + QByteArray::number(id));
                if (!valid)
                    error_ = error;
                return valid && released;
            }
            if (reply != "OK RELEASED" && reply != "OK CLOSED")
                return fail("Invalid WebKit acknowledgement");
            return true;
        }
        if (process_->state() == QProcess::NotRunning)
            return processExited();
    }
    close();
    return fail("WebKit request timed out");
}

bool WebKitDocument::readFrame(const QString &path, unsigned id)
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
    runtime_ = directory;
    frame_ = frame;
    image_ = std::move(image);
    error_.clear();
    return true;
}

bool WebKitDocument::openResources(std::shared_ptr<ResourceCache>, const QMap<QString, qint64> &,
                                   QSize size, const QJsonObject &saved, QString *error,
                                   OpeningProgress progress, OpeningResult *result,
                                   std::function<void()>)
{
    close();
    frame_ = {};
    offsets_.clear();
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
    appearance_["margin_pixels"] =
            qBound(0, saved.value("margin_pixels").toInt(defaultMarginPixels()),
                   (qMin(size.width(), size.height()) - 21) / 2);
    size_ = size;
    if (progress)
        progress(OpeningStage::Pages, 0);
    const bool opened = ensureOpen();
    if (error)
        *error = error_;
    if (!opened && result)
        *result =
                OpeningResult::failed(OpeningFailureKind::InvalidContent, "opening.webkit", error_);
    if (opened && progress)
        progress(OpeningStage::Ready, 100);
    return opened;
}

bool WebKitDocument::ensureOpen()
{
    if (opened_)
        return true;
    const bool restore = !frame_.isEmpty();
    const int index = spine(), selected = chapterPage();
    if (!start()
        || !command("OPEN " + encoded(QFileInfo(path_).absoluteFilePath()) + ' '
                            + QByteArray::number(size_.width()) + ' '
                            + QByteArray::number(size_.height()),
                    true))
        return false;
    opened_ = true;
    if (!style())
        return false;
    if (offsets_.isEmpty() && !paginateBook())
        return false;
    return !restore
            || command("GOTO " + QByteArray::number(index) + ' ' + QByteArray::number(selected),
                       true);
}

bool WebKitDocument::paginateBook()
{
    const int index = spine(), selected = chapterPage();
    const auto description = frame_.value("document").toObject();
    const int count = description.value("spine").toArray().size();
    const auto linear = description.value("linear").toArray();
    QVector<int> offsets{ 0 };
    QVector<bool> flags;
    for (int i = 0; i < count; ++i) {
        if (!command("GOTO " + QByteArray::number(i) + " 0", true))
            return false;
        offsets.append(offsets.last() + chapterPages());
        flags.append(linear.size() == count ? linear.at(i).toBool() : true);
    }
    if (!command("GOTO " + QByteArray::number(index) + ' ' + QByteArray::number(selected), true))
        return false;
    offsets_ = std::move(offsets);
    linear_ = std::move(flags);
    return true;
}

bool WebKitDocument::style()
{
    const int margin = qBound(0, appearance_.value("margin_pixels").toInt(defaultMarginPixels()),
                              (qMin(size_.width(), size_.height()) - 21) / 2);
    const auto family = appearance_.value("font_face").toString();
    const QByteArray generic = family == "serif" || family == "sans-serif" || family == "monospace"
            ? family.toLatin1()
            : QByteArray("publisher");
    return command(
            "STYLE "
                    + QByteArray::number(qBound(
                            8, appearance_.value("font_pixels").toInt(defaultFontPixels()), 96))
                    + ' '
                    + QByteArray::number(qBound(
                            100, appearance_.value("line_spacing").toInt(defaultLineSpacing), 300))
                    + ' ' + QByteArray::number(margin) + " 0 "
                    + QByteArray::number(
                            qBound(0, appearance_.value("word_spacing").toInt(100) - 100, 100))
                    + ' ' + generic + ' '
                    + (appearance_.value("dark_mode").toBool() ? "dark" : "light"),
            true);
}

bool WebKitDocument::applyAppearance(const QJsonObject &settings, QSize size, OpeningProgress)
{
    if (!ensureOpen())
        return false;
    const bool resized = size.isValid() && size != size_;
    if (resized) {
        if (size.width() < 100 || size.height() < 100 || size.width() > 2048
            || size.height() > 2048)
            return fail("WebKit viewport must be between 100 and 2048 pixels");
        const int limit = (qMin(size.width(), size.height()) - 21) / 2;
        if (appearance_.value("margin_pixels").toInt() > limit) {
            appearance_["margin_pixels"] = limit;
            if (!style())
                return false;
        }
        size_ = size;
        if (!command("RESIZE " + QByteArray::number(size.width()) + ' '
                             + QByteArray::number(size.height()),
                     true))
            return false;
    }
    if (appearance::same(appearance_, settings) && !resized)
        return true;
    appearance_ = settings;
    return style() && paginateBook();
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

bool WebKitDocument::go(int target)
{
    if (!ensureOpen())
        return false;
    if (target < 0 || target >= pages())
        return false;
    const int index =
            int(std::upper_bound(offsets_.begin(), offsets_.end(), target) - offsets_.begin()) - 1;
    const auto action = "GOTO " + QByteArray::number(index) + ' '
            + QByteArray::number(target - offsets_.at(index));
    const bool moved = target == page() || command(action, true);
    if (!moved && (error_.endsWith("end-of-book") || error_.endsWith("start-of-book"))) {
        error_.clear();
        return false;
    }
    return moved;
}

bool WebKitDocument::goTo(const QString &href)
{
    return ensureOpen() && command("LINK " + encoded(href), true);
}

QImage WebKitDocument::image(int selected)
{
    if (!ensureOpen() || selected != page())
        return {};
    if (image_.isNull())
        command("GOTO " + QByteArray::number(spine()) + ' ' + QByteArray::number(chapterPage()),
                true);
    return image_;
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
        if (item.value("href").toString().section('#', 0, 0) == path
            && !item.value("title").toString().trimmed().isEmpty()) {
            chapter = item.value("title").toString();
            break;
        }
    }
    if (chapter.trimmed().isEmpty())
        chapter = title();
    return { chapter.simplified().left(512), chapterPage() + 1, chapterPages() };
}
} // namespace kobalt

#include "peer.h"

#include <QtCore/QTimer>
#include <QtCore/QFile>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusUnixFileDescriptor>

#include <fcntl.h>
#include <limits>
#include <unistd.h>
#include <sys/vfs.h>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>

namespace {
const char path[] = "/org/kobalt/Renderer1";
const char interfaceName[] = "org.kobalt.Renderer1";
const char protocolError[] = "InvalidRendererReply";

struct Mapping
{
    void *address;
    size_t size;
};

void unmap(void *opaque)
{
    std::unique_ptr<Mapping> mapping(static_cast<Mapping *>(opaque));
    munmap(mapping->address, mapping->size);
}

QImage readFrame(const QVariantList &args, uint maxWidth, uint maxHeight, uint maxBytes,
                 const QString &namedDirectory)
{
    if (QSysInfo::ByteOrder != QSysInfo::LittleEndian
        || args[9].toString() != QStringLiteral("bgra8888-premultiplied"))
        return {};
    // Qt/libdbus resolves the wire h index against this message's FD list.
    // Require the decoded descriptor type and reject invalid handles.
    const auto fd = qvariant_cast<QDBusUnixFileDescriptor>(args[5]);
    const uint width = args[6].toUInt(), height = args[7].toUInt(), stride = args[8].toUInt();
    const quint64 bytes = quint64(stride) * height;
    if (!width || !height || width > maxWidth || height > maxHeight
        || stride % 4 || stride < quint64(width) * 4 || bytes > maxBytes
        || bytes > std::numeric_limits<int>::max() || bytes > std::numeric_limits<size_t>::max())
        return {};

    int raw = fd.isValid() ? fd.fileDescriptor() : -1;
    if (!namedDirectory.isEmpty()) {
        const auto name = args[5].toString().toLatin1();
        if (name.size() != 12 || !name.startsWith("frame-"))
            return {};
        for (int i = 6; i < name.size(); ++i) {
            const char c = name.at(i);
            if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z'))
                return {};
        }
        const auto directory = QFile::encodeName(namedDirectory);
        const int dir = open(directory.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dir < 0)
            return {};
        struct stat info {};
        struct statfs filesystem {};
        if (fstat(dir, &info) == 0 && info.st_uid == getuid() && (info.st_mode & 0777) == 0700
            && fstatfs(dir, &filesystem) == 0 && filesystem.f_type == 0x01021994)
            raw = openat(dir, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
    }
    if (raw < 0)
        return {};
    struct stat info {};
    void *pixels = MAP_FAILED;
    if (fstat(raw, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0
        && quint64(info.st_size) == bytes
        && (namedDirectory.isEmpty() || (info.st_uid == getuid() && (info.st_mode & 0777) == 0400)))
        pixels = mmap(nullptr, size_t(bytes), PROT_READ, MAP_SHARED, raw, 0);
    if (!namedDirectory.isEmpty())
        close(raw);
    if (pixels == MAP_FAILED)
        return {};
    auto *mapping = new Mapping{pixels, size_t(bytes)};
    // The const-data overload detaches before writable access. Image copies
    // share the map and release it through this callback on last reference.
    QImage image(static_cast<const uchar *>(pixels), int(width), int(height), int(stride),
                 QImage::Format_ARGB32_Premultiplied, unmap, mapping);
    if (image.isNull())
        unmap(mapping);
    return image;
}

bool readMap(const QVariant &value, QVariantMap &map)
{
    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return false;
    const auto argument = qvariant_cast<QDBusArgument>(value);
    argument.beginMap();
    while (!argument.atEnd()) {
        QString key;
        QDBusVariant entry;
        argument.beginMapEntry();
        argument >> key >> entry;
        argument.endMapEntry();
        if (map.contains(key))
            return false;
        map.insert(key, entry.variant());
    }
    argument.endMap();
    return true;
}

bool validState(const QVariantMap &state)
{
    const QVariant token = state.value(QStringLiteral("sourceToken"));
    const QVariant precision = state.value(QStringLiteral("locationPrecision"));
    if (token.userType() != QMetaType::QString || precision.userType() != QMetaType::QString
        || state.value(QStringLiteral("canTurnBack")).userType() != QMetaType::Bool
        || state.value(QStringLiteral("canTurnForward")).userType() != QMetaType::Bool)
        return false;
    const auto kind = precision.toString();
    if (kind == QStringLiteral("unresolved") ? !token.toString().isEmpty()
        : ((kind != QStringLiteral("exact") && kind != QStringLiteral("approximate"))
           || token.toString().isEmpty()))
        return false;
    const auto index = state.value(QStringLiteral("pageIndex"));
    const auto count = state.value(QStringLiteral("pageCount"));
    return (!index.isValid() || index.userType() == QMetaType::UInt)
            && (!count.isValid() || (count.userType() == QMetaType::UInt && count.toUInt()
                                     && (!index.isValid() || index.toUInt() < count.toUInt())));
}

QVariant id(qulonglong value)
{
    return QVariant::fromValue(value);
}
} // namespace

RendererPeer::RendererPeer(const QDBusConnection &connection, const QString &directory)
    : peer(connection), namedDirectory(directory)
{
}

RendererPeer::~RendererPeer()
{
    retire();
}

void RendererPeer::retire()
{
    if (!retired) {
        retired = true;
        QDBusConnection::disconnectFromPeer(peer.name());
    }
}

void RendererPeer::reject(Completion done, const QString &reason)
{
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [timer, done, reason] {
        timer->deleteLater();
        Result result;
        result.error = reason;
        done(result);
    });
    timer->start(0);
}

void RendererPeer::hello(Completion done)
{
    if (retired || closing || greeted)
        return reject(done, QStringLiteral("PeerUnavailable"));
    if (!peer.isConnected() || (namedDirectory.isEmpty() && !QDBusUnixFileDescriptor::isSupported()))
        return reject(done, QStringLiteral("UnixFdUnavailable"));
    greeted = true;
    send("Hello", {1u}, Reply::Hello, 0, done);
}

void RendererPeer::foreground(const char *method, const QVariantList &tail, Reply reply,
                              Completion done)
{
    if (retired || closing || !ready)
        return reject(done, QStringLiteral("PeerUnavailable"));
    if (pending)
        return reject(done, QStringLiteral("Busy"));
    if (nextRequest == std::numeric_limits<qulonglong>::max()) {
        retire();
        return reject(done, QStringLiteral("RequestIdsExhausted"));
    }
    pending = ++nextRequest;
    QVariantList args{id(pending)};
    if (reply != Reply::Open) {
        args << id(document);
        if (reply != Reply::Close)
            args << id(revision);
    }
    args << tail;
    QSize expected = viewport;
    if (reply == Reply::Open)
        expected = QSize(int(tail[3].toUInt()), int(tail[4].toUInt()));
    else if (qstrcmp(method, "SetViewport") == 0)
        expected = QSize(int(tail[0].toUInt()), int(tail[1].toUInt()));
    send(method, args, reply, pending, done, expected);
}

void RendererPeer::open(const QString &kind, const QString &source, const QString &fingerprint,
                        const QSize &size, const QVariantMap &appearance, const QString &token,
                        Completion done)
{
    foreground("Open", {kind, source, fingerprint, uint(size.width()), uint(size.height()),
                         appearance, token}, Reply::Open, done);
}

void RendererPeer::turn(int delta, Completion done)
{
    foreground("Turn", {delta}, Reply::Visual, done);
}

void RendererPeer::navigate(const QString &kind, const QString &target, Completion done)
{
    foreground("Navigate", {kind, target}, Reply::Visual, done);
}

void RendererPeer::setViewport(const QSize &size, Completion done)
{
    foreground("SetViewport", {uint(size.width()), uint(size.height())}, Reply::Visual, done);
}

void RendererPeer::setAppearance(const QVariantMap &appearance, Completion done)
{
    foreground("SetAppearance", {appearance}, Reply::Visual, done);
}

void RendererPeer::query(const QString &kind, const QVariantMap &arguments, Completion done)
{
    foreground("Query", {kind, arguments}, Reply::Query, done);
}

void RendererPeer::close(Completion done)
{
    foreground("Close", {}, Reply::Close, done);
}

void RendererPeer::cancel(Completion done)
{
    if (retired || closing || !ready || !pending)
        return reject(done, QStringLiteral("NoPendingRequest"));
    send("Cancel", {id(pending)}, Reply::Cancel, 0, done);
}

void RendererPeer::shutdown(Completion done)
{
    if (retired || closing || !ready)
        return reject(done, QStringLiteral("PeerUnavailable"));
    closing = true;
    send("Shutdown", {}, Reply::Shutdown, 0, done);
}

void RendererPeer::send(const char *method, const QVariantList &args, Reply kind,
                        qulonglong request, Completion done, const QSize &expected)
{
    const bool named = !namedDirectory.isEmpty();
    auto message = QDBusMessage::createMethodCall(
            QString(), named ? QStringLiteral("/org/kobalt/RendererShm1") : QString::fromLatin1(path),
            named ? QStringLiteral("org.kobalt.RendererShm1") : QString::fromLatin1(interfaceName),
            QString::fromLatin1(method));
    message.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(peer.asyncCall(message, 35000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, kind, request, done, expected](QDBusPendingCallWatcher *finished) {
        const auto reply = finished->reply();
        finished->deleteLater();
        Result result;
        if (retired || (request && request != pending)) {
            result.error = QStringLiteral("PeerRetired");
        } else {
            if (request)
                pending = 0;
            result = decode(reply, kind, request, expected);
            if (kind == Reply::Shutdown || (kind == Reply::Hello && !result.error.isEmpty())
                || result.error == QString::fromLatin1(protocolError)
                || result.error == QStringLiteral("UnixFdUnavailable")
                || (!result.error.isEmpty()
                    && !result.error.startsWith(QStringLiteral("org.kobalt.Renderer1.Error."))
                    && !result.error.startsWith(QStringLiteral("org.kobalt.RendererShm1.Error."))))
                retire();
        }
        done(result);
    });
}

RendererPeer::Result RendererPeer::decode(const QDBusMessage &message, Reply kind,
                                          qulonglong request, const QSize &expected)
{
    Result result;
    if (message.type() == QDBusMessage::ErrorMessage) {
        result.error = message.errorName();
        return result;
    }
    result.error = QString::fromLatin1(protocolError);
    if (message.type() != QDBusMessage::ReplyMessage)
        return result;
    const auto args = message.arguments();
    const auto signature = message.signature();
    if (kind == Reply::Hello) {
        QVariantMap caps;
        if (signature != QStringLiteral("ua{sv}")
            || args[0].toUInt() != 1
            || !readMap(args[1], caps))
            return result;
        const auto width = caps.value(QStringLiteral("maxWidth"));
        const auto height = caps.value(QStringLiteral("maxHeight"));
        const auto bytes = caps.value(QStringLiteral("maxFrameBytes"));
        const auto sources = caps.value(QStringLiteral("sourceKinds"));
        if (width.userType() != QMetaType::UInt || height.userType() != QMetaType::UInt
            || bytes.userType() != QMetaType::UInt || sources.userType() != QMetaType::QStringList
            || !width.toUInt() || width.toUInt() > 4096 || !height.toUInt()
            || height.toUInt() > 4096 || bytes.toUInt() < 4 || bytes.toUInt() > 32u * 1024 * 1024)
            return result;
        maxWidth = width.toUInt();
        maxHeight = height.toUInt();
        maxBytes = bytes.toUInt();
        ready = true;
        result.state = caps;
    } else if (kind == Reply::Cancel) {
        if (signature != QStringLiteral("b"))
            return result;
        result.cancelled = args[0].toBool();
    } else if (kind == Reply::Close || kind == Reply::Shutdown) {
        if (!signature.isEmpty())
            return result;
        document = layout = revision = 0;
        viewport = {};
    } else {
        const bool visual = kind != Reply::Query;
        const auto visualSignature = namedDirectory.isEmpty() ? QStringLiteral("tttta{sv}huuus")
                                                              : QStringLiteral("tttta{sv}suuus");
        if (signature != (visual ? visualSignature : QStringLiteral("tttta{sv}"))
            || args[0].toULongLong() != request)
            return result;
        const auto newDoc = args[1].toULongLong();
        const auto newLayout = args[2].toULongLong();
        const auto newRevision = args[3].toULongLong();
        if (kind == Reply::Open) {
            if (newDoc <= latestDocument || newLayout != 1 || newRevision != 1)
                return result;
        } else if (!document || newDoc != document || newLayout < layout
                   || (visual ? (revision == std::numeric_limits<qulonglong>::max()
                                 || newRevision != revision + 1)
                              : (newRevision != revision || newLayout != layout))) {
            return result;
        }
        QVariantMap state;
        if (!readMap(args[4], state) || !validState(state))
            return result;
        if (visual) {
            if (namedDirectory.isEmpty() && args[5].userType() != qMetaTypeId<QDBusUnixFileDescriptor>()) {
                result.error = QStringLiteral("UnixFdUnavailable");
                return result;
            }
            if (args[6].toUInt() != uint(expected.width())
                || args[7].toUInt() != uint(expected.height()))
                return result;
            auto image = readFrame(args, maxWidth, maxHeight, maxBytes, namedDirectory);
            if (image.isNull())
                return result;
            document = latestDocument = newDoc;
            layout = newLayout;
            revision = newRevision;
            viewport = expected;
            result.frame = image;
        }
        result.state = state;
        result.document = newDoc;
        result.layout = newLayout;
        result.revision = newRevision;
    }
    result.error.clear();
    return result;
}

#include "zip_directory.h"
#include "support/strings.h"
#include "publication.h"
#include "image_policy.h"
#include "zip_resource.h"
#include "web_resources.h"
#include "source_lease.h"
#include "source_position.h"
#include <QXmlStreamReader>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QSet>
#include <QBuffer>
#include <QUrl>
#include <QtEndian>
#include "engine/font_defaults.h"
#include <zlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>

namespace kobalt {
namespace {
bool fail(QString *error, const QString &message)
{
    *error = message;
    return false;
}

bool inspectionFailure(QString *error, OpeningResult *result, OpeningFailureKind kind,
                       const char *reason)
{
    if (result)
        *result = OpeningResult::failed(kind, reason);
    if (error)
        *error = strings::get(reason); // Compatibility for existing archive callers.
    return false;
}

void u16(QByteArray &out, quint16 n)
{
    const auto v = qToLittleEndian(n);
    out.append(reinterpret_cast<const char *>(&v), 2);
}

void u32(QByteArray &out, quint32 n)
{
    const auto v = qToLittleEndian(n);
    out.append(reinterpret_cast<const char *>(&v), 4);
}

bool safeName(const QString &name)
{
    if (name.isEmpty() || name.size() > 4096 || name.contains(QChar(0)) || name.contains('\\')
        || name.startsWith('/') || name.contains(':'))
        return false;
    for (const auto &part : name.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    // These prefixes belong to Nickel's other HTTP handlers.
    return !name.startsWith("TENG/") && !name.startsWith("FONT_ROUTE/")
            && !name.startsWith("EPUBPATH");
}

bool readableFont(QByteArray bytes, const QByteArray &key, int prefixBytes)
{
    if (bytes.isEmpty() || key.isEmpty())
        return false;
    for (int i = 0; i < qMin(prefixBytes, int(bytes.size())); ++i)
        bytes[i] = char(uchar(bytes[i]) ^ uchar(key[i % key.size()]));
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library))
        return false;
    FT_Face face = nullptr;
    const bool opened = !FT_New_Memory_Face(
            library, reinterpret_cast<const FT_Byte *>(bytes.constData()), bytes.size(), 0, &face);
    if (face)
        FT_Done_Face(face);
    FT_Done_FreeType(library);
    return opened;
}

bool declaredFontType(const QString &type)
{
    return type.startsWith("font/") || type.startsWith("application/x-font-")
            || type == "application/font-sfnt" || type == "application/vnd.ms-opentype"
            || type == "application/font-woff" || type == "application/vnd.ms-fontobject";
}

bool cleanXml(const QXmlStreamReader &xml)
{
    return xml.tokenType() != QXmlStreamReader::DTD
            && xml.tokenType() != QXmlStreamReader::EntityReference;
}

struct PackageItem
{
    QString path, media, properties;
};

struct Package
{
    QString path, identifier, title, author, toc, legacyCoverId;
    QByteArray adobeKey;
    int matchingIdentifiers = 0;
    QMap<QString, PackageItem> items;
    QSet<QString> paths, contentIds;
    QMap<QString, QString> declaredFonts;
    QStringList spine;
};

using ReadPackageResource = std::function<QByteArray(const QString &, qint64)>;
constexpr qint64 packageLimit = 2 * 1024 * 1024;

QString packageMember(const QString &packagePath, const QString &href, bool scoped)
{
    const QUrl url(href);
    if (href.isEmpty() || !url.isValid() || !url.scheme().isEmpty() || !url.authority().isEmpty()
        || url.hasQuery() || url.hasFragment() || href.contains('\\') || href.startsWith('/')
        || (!scoped && href.contains('%')))
        return {};
    QString relative = href;
    if (scoped && href.contains('%')) {
        const QRegularExpression malformed(QStringLiteral("%(?![0-9a-fA-F]{2})"));
        if (href.contains(malformed))
            return {};
        QStringList parts;
        for (const auto &segment : href.split('/')) {
            const auto bytes = QByteArray::fromPercentEncoding(segment.toUtf8());
            const auto decoded = QString::fromUtf8(bytes);
            if (decoded.isEmpty() || decoded.toUtf8() != bytes || decoded.contains('/')
                || decoded.contains('\\') || decoded.contains(QChar(0))
                || (segment.contains('%') && (decoded == "." || decoded == "..")))
                return {};
            parts.append(decoded);
        }
        relative = parts.join('/');
    }
    const auto path = QDir::cleanPath(QFileInfo(packagePath).path() + '/' + relative);
    return safeName(path) ? path : QString();
}

bool readPackage(const ReadPackageResource &read, Package &result, bool scoped)
{
    QXmlStreamReader descriptor(read(QStringLiteral("META-INF/container.xml"), packageLimit));
    QString packagePath;
    int rootfiles = 0, descriptorDepth = 0;
    while (!descriptor.atEnd()) {
        descriptor.readNext();
        if (!cleanXml(descriptor))
            return false;
        if (descriptor.isEndElement()) {
            --descriptorDepth;
            continue;
        }
        if (!descriptor.isStartElement())
            continue;
        ++descriptorDepth;
        if (descriptor.namespaceUri()
                    != QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:container")
            || (descriptorDepth == 1 && descriptor.name() != QStringLiteral("container"))
            || (descriptorDepth == 2 && descriptor.name() != QStringLiteral("rootfiles")))
            return false;
        if (descriptorDepth == 3 && descriptor.name() == QStringLiteral("rootfile")) {
            if (++rootfiles != 1)
                return false;
            packagePath = descriptor.attributes().value("full-path").toString();
            if (descriptor.attributes().value("media-type")
                != QStringLiteral("application/oebps-package+xml"))
                return false;
        }
    }
    if (descriptor.hasError() || !safeName(packagePath))
        return false;
    QXmlStreamReader package(read(packagePath, packageLimit));
    result.path = packagePath;
    QString identifierId, identifier;
    QByteArray adobeKey;
    const QRegularExpression adobeIdentifier(
            "^(?:urn:uuid:)?(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$");
    const QRegularExpression propertySeparator("\\s+");
    bool spineSeen = false, manifestSeen = false;
    int matchingIdentifiers = 0, depth = 0, metadataDepth = -1, manifestDepth = -1, spineDepth = -1;

    while (!package.atEnd()) {
        package.readNext();
        if (!cleanXml(package))
            return false;
        if (package.isEndElement()) {
            if (depth == metadataDepth)
                metadataDepth = -1;
            if (depth == manifestDepth)
                manifestDepth = -1;
            if (depth == spineDepth)
                spineDepth = -1;
            --depth;
            continue;
        }
        if (!package.isStartElement())
            continue;
        ++depth;
        if (scoped
            && package.attributes().hasAttribute(
                    QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("base")))
            return false;
        if (depth == 1) {
            if (package.name() != QStringLiteral("package")
                || package.namespaceUri() != QStringLiteral("http://www.idpf.org/2007/opf"))
                return false;
            identifierId = package.attributes().value("unique-identifier").toString();
        } else if (depth == 2
                   && package.namespaceUri() == QStringLiteral("http://www.idpf.org/2007/opf")
                   && package.name() == QStringLiteral("metadata"))
            metadataDepth = depth;
        else if (depth == 2
                 && package.namespaceUri() == QStringLiteral("http://www.idpf.org/2007/opf")
                 && package.name() == QStringLiteral("manifest")) {
            if (scoped && manifestSeen)
                return false;
            manifestSeen = true;
            manifestDepth = depth;
        } else if (depth == 2 && package.name() == QStringLiteral("spine")) {
            spineDepth = depth;
            if (scoped
                && (spineSeen
                    || package.namespaceUri() != QStringLiteral("http://www.idpf.org/2007/opf")))
                return false;
            spineSeen = true;
            result.toc = package.attributes().value("toc").toString();
            result.contentIds.insert(result.toc);
        } else if (depth == spineDepth + 1 && spineDepth == 2
                   && package.name() == QStringLiteral("itemref")) {
            if (scoped && package.namespaceUri() != QStringLiteral("http://www.idpf.org/2007/opf"))
                return false;
            const auto id = package.attributes().value("idref").toString();
            result.spine.append(id);
            result.contentIds.insert(id);
        } else if (scoped && depth == metadataDepth + 1 && metadataDepth == 2
                   && package.namespaceUri() == QStringLiteral("http://purl.org/dc/elements/1.1/")
                   && (package.name() == QStringLiteral("title")
                       || package.name() == QStringLiteral("creator"))) {
            auto &metadata =
                    package.name() == QStringLiteral("title") ? result.title : result.author;
            const auto text = package.readElementText().simplified().left(1024);
            if (metadata.isEmpty())
                metadata = text;
            --depth;
        } else if (scoped && depth == metadataDepth + 1 && metadataDepth == 2
                   && package.name() == QStringLiteral("meta")
                   && package.namespaceUri() == QStringLiteral("http://www.idpf.org/2007/opf")
                   && package.attributes().value("name") == QStringLiteral("cover")
                   && result.legacyCoverId.isEmpty()) {
            result.legacyCoverId = package.attributes().value("content").toString();
        } else if (depth == metadataDepth + 1 && metadataDepth == 2
                   && package.name() == QStringLiteral("identifier")
                   && package.namespaceUri()
                           == QStringLiteral("http://purl.org/dc/elements/1.1/")) {
            const bool unique =
                    package.attributes().value("id") == identifierId && !identifierId.isEmpty();
            const auto text = package.readElementText();
            if (unique) {
                ++matchingIdentifiers;
                identifier = text;
            }
            // Adobe uses a UUID identifier, which can differ from the IDPF
            // unique identifier. CREngine checks every metadata identifier.
            if (adobeKey.isEmpty() && adobeIdentifier.match(text.trimmed()).hasMatch()) {
                auto uuid = text.trimmed();
                uuid.remove(QStringLiteral("urn:uuid:"));
                uuid.remove('-');
                adobeKey = QByteArray::fromHex(uuid.toLatin1());
            }
            --depth;
        } else if (depth == manifestDepth + 1 && manifestDepth == 2
                   && package.namespaceUri() == QStringLiteral("http://www.idpf.org/2007/opf")
                   && package.name() == QStringLiteral("item")) {
            const auto attributes = package.attributes();
            const auto id = attributes.value("id").toString();
            const auto path =
                    packageMember(packagePath, attributes.value("href").toString(), scoped);
            if (id.isEmpty() || result.items.contains(id) || path.isEmpty()
                || result.paths.contains(path))
                return false;
            const auto media = attributes.value("media-type").toString().trimmed().toLower();
            const auto properties = attributes.value("properties").toString();
            result.items.insert(id, { path, media, properties });
            result.paths.insert(path);
            result.contentIds.insert(attributes.value("fallback").toString());
            if (declaredFontType(media)
                && !properties.split(propertySeparator).contains(QStringLiteral("nav")))
                result.declaredFonts.insert(path, id);
        }
    }
    identifier.remove(QRegularExpression("[ \t\r\n]"));
    result.identifier = identifier;
    result.matchingIdentifiers = matchingIdentifiers;
    result.adobeKey = adobeKey;
    return !package.hasError()
            && (!scoped || (manifestSeen && spineSeen && !result.spine.isEmpty()));
}

// Font obfuscation is an EPUB embedding convention, not content encryption.
bool supportedFontMetadata(QIODevice &source, const QMap<QString, qint64> &sizes,
                           QSet<QString> &skippedFonts, QSet<QString> &obfuscatedFonts)
{
    ZipResourceIndex index;
    auto read = [&](const QString &name, qint64 maximum = 2 * 1024 * 1024) -> QByteArray {
        ZipResource stream;
        if (sizes.value(name, -1) < 0 || sizes.value(name) > maximum
            || !stream.open(source, name, index, maximum))
            return {};
        QByteArray bytes;
        while (stream.valid() && !stream.finished())
            bytes += stream.next();
        return stream.valid() && stream.finished() ? bytes : QByteArray();
    };
    Package package;
    if (!readPackage(read, package, false) || package.matchingIdentifiers != 1
        || package.identifier.isEmpty())
        return false;
    const auto idpfKey =
            QCryptographicHash::hash(package.identifier.toUtf8(), QCryptographicHash::Sha1);
    const auto metadata = read("META-INF/encryption.xml");
    QXmlStreamReader check(metadata);
    while (!check.atEnd()) {
        check.readNext();
        if (!cleanXml(check))
            return false;
    }
    if (check.hasError())
        return false;
    QXmlStreamReader xml(metadata);
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("encryption")
        || xml.namespaceUri() != QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:container"))
        return false;

    struct Font
    {
        QString path;
        qint64 size;
        bool idpf;
    };

    QSet<QString> targets;
    QVector<Font> fonts;
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("EncryptedData")
            || xml.namespaceUri() != QStringLiteral("http://www.w3.org/2001/04/xmlenc#"))
            return false;
        QString algorithm, target;
        int methods = 0, cipherData = 0, references = 0;
        while (xml.readNextStartElement()) {
            if (xml.namespaceUri() != QStringLiteral("http://www.w3.org/2001/04/xmlenc#"))
                return false;
            if (xml.name() == QStringLiteral("EncryptionMethod")) {
                if (++methods != 1)
                    return false;
                algorithm = xml.attributes().value("Algorithm").toString();
                if (xml.readNextStartElement())
                    return false;
            } else if (xml.name() == QStringLiteral("CipherData")) {
                if (++cipherData != 1)
                    return false;
                while (xml.readNextStartElement()) {
                    if (xml.name() != QStringLiteral("CipherReference")
                        || xml.namespaceUri() != QStringLiteral("http://www.w3.org/2001/04/xmlenc#")
                        || ++references != 1)
                        return false;
                    target = xml.attributes().value("URI").toString();
                    if (xml.readNextStartElement())
                        return false;
                }
            } else
                return false;
        }
        const bool idpf = algorithm == QStringLiteral("http://www.idpf.org/2008/embedding");
        const bool adobe = algorithm == QStringLiteral("http://ns.adobe.com/pdf/enc#RC")
                && !package.adobeKey.isEmpty();
        const auto size = sizes.value(target, -1);
        if (methods != 1 || cipherData != 1 || references != 1 || (!idpf && !adobe)
            || !safeName(target) || target.contains('%') || !package.paths.contains(target)
            || targets.contains(target) || size <= 0)
            return false;
        targets.insert(target);
        fonts.append(Font{ target, size, idpf });
    }
    while (!xml.atEnd()) {
        xml.readNext();
        if (!cleanXml(xml))
            return false;
    }
    if (xml.hasError() || fonts.isEmpty())
        return false;

    // Spend the validation budget on the smallest fonts first. This does not
    // change the manifest or the publisher's CSS font order.
    std::stable_sort(fonts.begin(), fonts.end(),
                     [](const Font &a, const Font &b) { return a.size < b.size; });
    qint64 remainingFontBytes = Publication::maxBytes;
    for (const auto &font : fonts) {
        if (font.size > maxEmbeddedFontBytes || font.size > remainingFontBytes) {
            // Omit only a declared font that is outside the reading order and
            // navigation. Never expose unverified obfuscated bytes to CREngine.
            const auto id = package.declaredFonts.value(font.path);
            if (id.isEmpty() || package.contentIds.contains(id) || font.path == package.path
                || font.path == "mimetype" || font.path.startsWith("META-INF/"))
                return false;
            skippedFonts.insert(font.path);
            continue;
        }
        // MIME labels and extensions vary between publishers. Decode a local
        // copy for the renderer's font parser; leave archive resources unchanged.
        if (!readableFont(read(font.path, maxEmbeddedFontBytes),
                          font.idpf ? idpfKey : package.adobeKey, font.idpf ? 1040 : 1024))
            return false;
        remainingFontBytes -= font.size;
    }
    obfuscatedFonts = std::move(targets);
    return true;
}

// Encode each segment independently. Look up the exact encoded URL before
// reading any bytes, so alternate encodings cannot bypass the allowlist.
QByteArray memberUrl(const QByteArray &origin, const QString &member)
{
    if (!safeName(member) || member.startsWith("META-INF/", Qt::CaseInsensitive))
        return {};
    QByteArray encoded = origin;
    for (const auto &part : member.split('/')) {
        encoded += '/';
        encoded += QUrl::toPercentEncoding(part);
    }
    return encoded;
}

qint64 webResourceLimit(const QString &name, const QByteArray &media)
{
    const auto ext = QFileInfo(name).suffix().toLower();
    if ((media == "application/xhtml+xml" && (ext == "xhtml" || ext == "html" || ext == "htm"))
        || (media == "text/css" && ext == "css"))
        return packageLimit;
    if ((media == "image/png" && ext == "png")
        || (media == "image/jpeg" && (ext == "jpg" || ext == "jpeg"))
        || (media == "image/gif" && ext == "gif") || (media == "image/webp" && ext == "webp"))
        return 8 * 1024 * 1024;
    if ((media == "font/ttf" && ext == "ttf") || (media == "font/otf" && ext == "otf")
        || (media == "font/woff" && ext == "woff") || (media == "font/woff2" && ext == "woff2"))
        return packageLimit;
    return 0;
}

bool hasDeclaredSignature(const QByteArray &bytes, const QByteArray &media)
{
    if (media == "image/png")
        return bytes.startsWith("\x89PNG\r\n\x1a\n");
    if (media == "image/jpeg")
        return bytes.startsWith("\xff\xd8\xff");
    if (media == "image/gif")
        return bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a");
    if (media == "image/webp")
        return bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP";
    if (media == "font/ttf")
        return bytes.size() >= 4 && bytes.left(4) == QByteArray("\0\1\0\0", 4);
    if (media == "font/otf")
        return bytes.startsWith("OTTO");
    if (media == "font/woff" || media == "font/woff2")
        return bytes.startsWith(media == "font/woff" ? "wOFF" : "wOF2");
    return true;
}
} // namespace

std::unique_ptr<WebResources> WebResources::open(const Publication &publication,
                                                 std::shared_ptr<ResourceCache> cache,
                                                 std::shared_ptr<SourceLease> lease, QString *error)
{
    auto reject = [error](const char *reason) -> std::unique_ptr<WebResources> {
        if (error)
            *error = QString::fromLatin1(reason);
        return {};
    };
    if (error)
        error->clear();
    if (!cache || !lease || lease->revoked() || publication.protectedBook()
        || publication.fingerprint().isEmpty())
        return reject("Publication is not admitted");
    auto read = [&](const QString &name, qint64 limit) {
        const auto size = publication.sizes().value(name, -1);
        if (lease->revoked() || size <= 0 || size > limit)
            return QByteArray();
        const auto bytes = cache->readBounded(name, limit);
        return bytes.size() == size ? bytes : QByteArray();
    };
    if (read(QStringLiteral("mimetype"), 32) != "application/epub+zip")
        return reject("Invalid EPUB media type");
    Package package;
    if (!readPackage(read, package, true))
        return reject("Invalid EPUB package manifest");

    auto scope = std::unique_ptr<WebResources>(new WebResources);
    scope->origin_ = "kobalt-epub://book-" + QByteArray::number(lease->generation());
    scope->fingerprint_ = publication.fingerprint();
    scope->title_ = package.title;
    scope->author_ = package.author;
    const auto toc = package.items.value(package.toc);
    if (toc.media == QStringLiteral("application/x-dtbncx+xml") && cache->contains(toc.path))
        scope->ncx_ = toc.path;
    const QRegularExpression whitespace(QStringLiteral("\\s+"));
    QByteArray legacyCover;
    for (auto it = package.items.cbegin(); it != package.items.cend(); ++it) {
        const auto &item = it.value();
        const auto size = publication.sizes().value(item.path, -1);
        const auto media = item.media.toLatin1();
        const auto limit = webResourceLimit(item.path, media);
        if (size <= 0 || size > limit || !cache->contains(item.path)
            || publication.obfuscatedFonts().contains(item.path) || item.path == package.path)
            continue;
        const auto encoded = memberUrl(scope->origin_, item.path);
        if (encoded.isEmpty())
            continue;
        scope->members_.insert(encoded, { item.path, media, size });
        const auto properties = item.properties.split(whitespace);
        if (media.startsWith("image/")) {
            if (scope->cover_.isEmpty() && properties.contains("cover-image"))
                scope->cover_ = encoded;
            if (it.key() == package.legacyCoverId)
                legacyCover = encoded;
        }
        if (media == "application/xhtml+xml" && properties.contains("nav")) {
            if (!scope->navigation_.isEmpty())
                return reject("Ambiguous EPUB navigation");
            scope->navigation_ = item.path;
        }
    }
    if (scope->cover_.isEmpty())
        scope->cover_ = legacyCover;
    for (const auto &id : package.spine) {
        const auto item = package.items.value(id);
        if (item.media != "application/xhtml+xml" || scope->url(item.path).isEmpty())
            return reject("Spine resource is missing, oversized or unsupported");
        scope->spine_.append(item.path);
    }

    if (lease->revoked())
        return reject("Publication access ended");
    scope->cache_ = std::move(cache);
    scope->lease_ = std::move(lease);
    return scope;
}

QVector<ChapterLink> WebResources::chapters()
{
    QVector<ChapterLink> result;
    const bool ncx = navigation_.isEmpty();
    const auto member = ncx ? ncx_ : navigation_;
    if (member.isEmpty() || lease_->revoked())
        return result;
    // The admitted OPF names the document. Read within the same cache and
    // bound as the package. Only resolved spine members become menu targets.
    const auto bytes = cache_->readBounded(member, packageLimit);
    QXmlStreamReader xml(bytes);

    struct Entry
    {
        QString title, link;
    };

    QVector<Entry> entries;
    int depth = 0, navigationDepth = -1, textDepth = -1;
    auto append = [&](const Entry &entry, int level) {
        QUrl href(entry.link, QUrl::StrictMode);
        const auto fragment = href.fragment(QUrl::FullyDecoded);
        href.setFragment(QString());
        const auto name = entry.link.startsWith('#')
                ? member
                : packageMember(member, href.toString(QUrl::FullyEncoded), true);
        if (entry.title.isEmpty() || fragment.size() > 512 || !spine_.contains(name))
            return;
        auto target = QUrl::fromEncoded(url(name));
        if (!fragment.isEmpty())
            target.setFragment(fragment, QUrl::DecodedMode);
        result.append({ entry.title.simplified().left(1024), target.toString(QUrl::FullyEncoded),
                        -1, level });
    };
    while (!xml.atEnd() && result.size() < maximumChapterEntries && !lease_->revoked()) {
        xml.readNext();
        if (!cleanXml(xml)
            && !(xml.tokenType() == QXmlStreamReader::DTD
                 && xml.text().toString().trimmed() == QStringLiteral("<!DOCTYPE html>")))
            return {};
        const auto tag = xml.name();
        const auto attributes = xml.attributes();
        if (xml.isStartElement()) {
            if (++depth > 128)
                return {};
            if (attributes.hasAttribute(QStringLiteral("http://www.w3.org/XML/1998/namespace"),
                                        QStringLiteral("base")))
                return {};
            if (navigationDepth < 0
                && (ncx ? tag == QStringLiteral("navMap")
                        : tag == QStringLiteral("nav")
                                    && attributes.value(
                                               QStringLiteral("http://www.idpf.org/2007/ops"),
                                               QStringLiteral("type"))
                                            == QStringLiteral("toc")))
                navigationDepth = depth;
            if (navigationDepth < 0)
                continue;
            if (tag == (ncx ? QStringLiteral("navPoint") : QStringLiteral("li")))
                entries.append(Entry{});
            if (!entries.isEmpty()
                && (ncx ? tag == QStringLiteral("content") : tag == QStringLiteral("a"))) {
                auto &entry = entries.last();
                entry.link = attributes.value(ncx ? "src" : "href").toString();
                if (ncx) {
                    // NCX places the label before the target and before nested rows.
                    append(entry, entries.size() - 1);
                } else {
                    textDepth = depth;
                }
            }
            if (ncx && !entries.isEmpty() && tag == QStringLiteral("text"))
                textDepth = depth;
        } else if (xml.isCharacters() && textDepth >= 0 && !entries.isEmpty()) {
            auto &title = entries.last().title;
            if (title.size() < 1024)
                title += xml.text().toString().left(1024 - title.size());
        } else if (xml.isEndElement()) {
            if (depth == textDepth) {
                if (!ncx && !entries.isEmpty())
                    append(entries.last(), entries.size() - 1);
                textDepth = -1;
            }
            if (!entries.isEmpty()
                && tag == (ncx ? QStringLiteral("navPoint") : QStringLiteral("li")))
                entries.removeLast();
            if (depth == navigationDepth)
                break;
            --depth;
        }
    }
    return xml.hasError() || lease_->revoked() ? QVector<ChapterLink>{} : result;
}

QString WebResources::noteText(const QString &spineMember, const QString &fragment)
{
    constexpr int textLimit = 8192;
    const auto uri = url(spineMember);
    const auto member = members_.constFind(uri);
    if (fragment.isEmpty() || !spine_.contains(spineMember) || member == members_.cend()
        || member->size > packageLimit)
        return {};
    const auto data = read(uri);
    if (data.mediaType != "application/xhtml+xml")
        return {};
    QXmlStreamReader xml(data.bytes);
    QString text;
    int depth = 0, body = 0, note = 0, skipped = 0;
    while (!xml.atEnd() && !lease_->revoked()) {
        xml.readNext();
        if (!cleanXml(xml)
            && !(xml.tokenType() == QXmlStreamReader::DTD
                 && xml.text() == QStringLiteral("<!DOCTYPE html>")))
            return {};
        if (xml.isStartElement()) {
            if (++depth > 128)
                return {};
            if (skipped)
                continue;
            const auto tag = xml.name();
            if (tag == QStringLiteral("script") || tag == QStringLiteral("style")) {
                skipped = depth;
                continue;
            }
            if (!body && tag == QStringLiteral("body"))
                body = depth;
            if (body && !note && depth > body
                && xml.attributes().value(QStringLiteral("id")) == fragment)
                note = depth;
            if (note
                && (tag == QStringLiteral("p") || tag == QStringLiteral("li")
                    || tag == QStringLiteral("br")))
                text += QLatin1Char('\n');
        } else if (xml.isEndElement()) {
            if (depth == note)
                return lease_->revoked() ? QString() : text.trimmed();
            if (depth == body)
                return {};
            if (depth == skipped)
                skipped = 0;
            --depth;
        } else if (xml.isCharacters() && note && !skipped) {
            if (xml.text().size() > textLimit - text.size())
                return {};
            text += xml.text();
        }
        if (text.size() > textLimit)
            return {};
    }
    return {};
}

namespace {
int countedBetween(const QString &text, int start, int end)
{
    int result = 0;
    for (int i = start; i < end; ++i) {
        const auto unit = text.at(i);
        if (unit.isHighSurrogate() && i + 1 < end && text.at(i + 1).isLowSurrogate())
            ++i;
        result += !locationUncounted(unit.unicode());
    }
    return result;
}

QByteArray elementKey(const QVector<int> &path)
{
    QByteArray key;
    for (const auto index : path) {
        key += '/';
        key += QByteArray::number(index);
    }
    return key;
}

bool separatesSearchText(const QString &tag)
{
    static const QStringList elements{
        "address", "article",    "aside",   "blockquote", "br", "dd",   "div", "dl",
        "dt",      "figcaption", "figure",  "footer",     "h1", "h2",   "h3",  "h4",
        "h5",      "h6",         "header",  "hr",         "li", "main", "nav", "ol",
        "p",       "pre",        "section", "table",      "td", "th",   "tr",  "ul"
    };
    return elements.contains(tag);
}

int sourceUnit(const QString &text, int offset, bool after)
{
    if (!after && !offset)
        return 0;
    for (int i = 0; i < text.size();) {
        const auto begin = i;
        const auto unit = text.at(i++);
        if (unit.isHighSurrogate() && i < text.size() && text.at(i).isLowSurrogate())
            ++i;
        if (locationUncounted(unit.unicode()))
            continue;
        if ((after && offset == 0) || (!after && offset == 1))
            return after ? begin : i;
        --offset;
    }
    return offset == 0 ? int(text.size()) : -1;
}
} // namespace

bool WebResources::indexSearchItem(int item, const std::function<bool()> &current)
{
    if (item == searchItem_)
        return true;
    searchItem_ = -1;
    searchBlocks_.clear();
    indexedElements_.clear();
    indexedText_.clear();
    const auto member = members_.constFind(url(spine_.value(item)));
    if (member == members_.cend() || member->size > packageLimit || !current())
        return false;
    const auto bytes = cache_->readBounded(member->name, member->size);
    if (bytes.size() != member->size)
        return false;

    struct Element
    {
        QByteArray key;
        int index, begin;
        int childCount = 0, counted = 0;
        bool hidden = false, separates = false;
    };

    QXmlStreamReader xml(bytes);
    QVector<Element> elements;
    QVector<SearchBlock> blocks;
    QMap<QByteArray, QPair<int, int>> positions;
    QString textInOrder;
    int counted = 0;
    int storedIndexes = 0, storedKeyBytes = 0, spans = 0;
    bool body = false, nextBlock = true;
    while (!xml.atEnd() && current()) {
        xml.readNext();
        if (!cleanXml(xml)
            && !(xml.tokenType() == QXmlStreamReader::DTD
                 && xml.text() == QStringLiteral("<!DOCTYPE html>")))
            return false;
        if (xml.isStartElement()) {
            if (!elements.isEmpty()) {
                auto &parent = elements.last();
                const bool hidden = parent.hidden || xml.name() == QStringLiteral("script")
                        || xml.name() == QStringLiteral("style")
                        || xml.attributes().hasAttribute(QStringLiteral("hidden"));
                const int index = parent.childCount++;
                const auto key = parent.key + '/' + QByteArray::number(index);
                storedKeyBytes += int(key.size());
                const bool separates =
                        separatesSearchText(xml.name().toString()) || hidden != parent.hidden;
                if (separates) {
                    nextBlock = true;
                    textInOrder += QLatin1Char(' ');
                }
                elements.append({ key, index, counted, 0, 0, hidden, separates });
                if (elements.size() > 129 || positions.size() >= 32768
                    || storedKeyBytes > 1024 * 1024)
                    return false;
            } else if (xml.name() == QStringLiteral("body")) {
                body = true;
                elements.append({ {}, 0, 0, 0, 0, false, false });
            }
        } else if (xml.isEndElement() && !elements.isEmpty()) {
            const auto element = elements.takeLast();
            positions.insert(element.key, { element.begin, element.counted });
            if (element.separates) {
                nextBlock = true;
                textInOrder += QLatin1Char(' ');
            }
            if (elements.isEmpty())
                break;
        } else if (xml.isCharacters() && !elements.isEmpty()) {
            const auto text = xml.text().toString();
            const int length = countedBetween(text, 0, int(text.size()));
            textInOrder += text;
            if (!elements.last().hidden && (!nextBlock || !xml.isWhitespace())) {
                storedIndexes += elements.size();
                if (++spans > 16384 || storedIndexes > 262144)
                    return false;
                if (nextBlock) {
                    blocks.append(SearchBlock{});
                    nextBlock = false;
                }
                auto &block = blocks.last();
                SearchSpan span{
                    int(block.text.size()), block.counted, {}, elements.last().counted
                };
                for (int i = 1; i < elements.size(); ++i)
                    span.path.append(elements[i].index);
                block.spans.append(std::move(span));
                block.text += text;
                block.counted += length;
            }
            counted += length;
            for (auto &element : elements)
                element.counted += length;
        }
    }
    if (!body || xml.hasError() || !current())
        return false;
    searchItem_ = item;
    searchBlocks_ = std::move(blocks);
    indexedElements_ = std::move(positions);
    indexedText_ = std::move(textInOrder);
    return true;
}

int WebResources::countedPosition(const Location &location) const
{
    const auto at = indexedElements_.constFind(elementKey(location.path));
    return at == indexedElements_.cend() || location.offset < 0 || location.offset > at->second
            ? -1
            : at->first + location.offset;
}

TextRange WebResources::textRange(const QString &start, const QString &end, bool withContext)
{
    const auto first = sourcePositionFromToken(start, fingerprint_);
    const auto last = sourcePositionFromToken(end, fingerprint_);
    const int item = spine_.indexOf(first.href);
    if (item < 0 || first.href != last.href
        || !indexSearchItem(item, [this] { return !lease_->revoked(); }))
        return {};
    int before = countedPosition(first), after = countedPosition(last);
    if (before < 0 || after < 0 || before == after)
        return {};
    if (before > after)
        return textRange(end, start, withContext);
    const int begin = sourceUnit(indexedText_, before, first.after);
    const int finish = sourceUnit(indexedText_, after, last.after);
    if (begin < 0 || finish <= begin || finish - begin > 8192 || lease_->revoked())
        return {};
    TextRange range{ start, end, indexedText_.mid(begin, finish - begin).simplified() };
    if (withContext) {
        const auto left = qMax(0, begin - 100);
        range.context = indexedText_.mid(left, finish - left + 100).simplified();
    }
    return range;
}

TextRange WebResources::selectionRange(const QStringList &tokens)
{
    if (tokens.size() != 4)
        return {};
    const auto first = sourcePositionFromToken(tokens[0], fingerprint_);
    const auto last = sourcePositionFromToken(tokens[2], fingerprint_);
    const int item = spine_.indexOf(first.href);
    if (item < 0 || first.href != last.href
        || !indexSearchItem(item, [this] { return !lease_->revoked(); }))
        return {};
    const int a = countedPosition(first), b = countedPosition(last);
    if (a < 0 || b < 0)
        return {};
    const bool reverse = b < a;
    auto range = textRange(tokens[reverse ? 2 : 0], tokens[reverse ? 1 : 3], false);
    range.chosenStart = range.start;
    range.chosenEnd = range.end;
    return range;
}

QVector<int> WebResources::intersectingRanges(const QString &start, const QString &end,
                                              const QVector<QPair<QString, QString>> &ranges)
{
    const auto first = sourcePositionFromToken(start, fingerprint_);
    const auto last = sourcePositionFromToken(end, fingerprint_);
    const int item = spine_.indexOf(first.href);
    if (item < 0 || first.href != last.href
        || !indexSearchItem(item, [this] { return !lease_->revoked(); }))
        return { -1 };
    const int a = countedPosition(first), b = countedPosition(last);
    if (a < 0 || b < 0 || a >= b)
        return { -1 };
    QVector<int> hits;
    for (int i = 0; i < ranges.size(); ++i) {
        const auto left = sourcePositionFromToken(ranges[i].first, fingerprint_);
        const auto right = sourcePositionFromToken(ranges[i].second, fingerprint_);
        if (!left.valid() || !right.valid())
            return { -1 };
        const int from = spine_.indexOf(left.href), to = spine_.indexOf(right.href);
        if (from < 0 || to < from)
            return { -1 };
        if (item < from || item > to)
            continue;
        const int low = from == item ? countedPosition(left) : 0;
        const int high = to == item ? countedPosition(right) : indexedElements_.value({}).second;
        if (low < 0 || high < low)
            return { -1 };
        if (a < high && low < b)
            hits.append(i);
    }
    return lease_->revoked() ? QVector<int>{ -1 } : hits;
}

SearchBatch WebResources::searchText(const QString &query, SearchCursor cursor, int lastItem,
                                     SearchScope scope, const Location &pageEnd,
                                     const std::function<bool()> &current)
{
    SearchBatch batch;
    batch.cursor = cursor;
    const auto stopped = [&] {
        batch.done = batch.limited = true;
        return batch;
    };
    if (query.isEmpty() || !current() || cursor.item < 0 || cursor.item >= spine_.size()
        || !indexSearchItem(cursor.item, current))
        return stopped();
    Location location;
    location.href = spine_[cursor.item];
    const bool bounded = location.href == pageEnd.href
            && (scope == SearchScope::Before || scope == SearchScope::After);
    const int stopAt = bounded ? countedPosition(pageEnd) : -1;
    if (bounded && stopAt < 0)
        return stopped();
    const auto chapter = QFileInfo(location.href).completeBaseName();
    constexpr int batchSize = 32;
    while (batch.cursor.block < searchBlocks_.size() && current()) {
        const auto &block = searchBlocks_[batch.cursor.block];
        const auto at = block.text.indexOf(query, batch.cursor.offset, Qt::CaseInsensitive);
        if (at < 0) {
            ++batch.cursor.block;
            batch.cursor.offset = 0;
            batch.cursor.slice = 0;
            continue;
        }
        const auto begin = int(at), end = int(at + query.size());
        const auto anchor = [&](int unit, int counted, bool leading) {
            const auto found = std::upper_bound(
                    block.spans.cbegin(), block.spans.cend(), leading ? unit : unit - 1,
                    [](int unit, const SearchSpan &span) { return unit < span.unit; });
            const auto &span = *std::prev(found);
            Location result = location;
            result.path = span.path;
            result.offset = span.offset + counted - span.counted;
            result.after = leading;
            return result;
        };
        const int firstCount =
                batch.cursor.slice + countedBetween(block.text, batch.cursor.offset, begin);
        const int lastCount = firstCount + countedBetween(block.text, begin, end);
        const auto first = anchor(begin, firstCount, true), last = anchor(end, lastCount, false);
        const bool inScope = !bounded
                || (scope == SearchScope::Before ? countedPosition(last) <= stopAt
                                                 : countedPosition(first) >= stopAt);
        if (inScope) {
            const auto excerpt = qMax(0, begin - 36);
            batch.hits.append({ sourcePositionToken(first, fingerprint_),
                                sourcePositionToken(last, fingerprint_),
                                block.text.mid(excerpt, begin - excerpt),
                                block.text.mid(begin, end - begin), block.text.mid(end, 36), -1,
                                chapter });
        }
        batch.cursor.offset = end;
        batch.cursor.slice = lastCount;
        if (batch.hits.size() >= batchSize)
            return batch;
    }
    if (!current())
        return stopped();
    batch.cursor = {};
    batch.cursor.item = cursor.item + 1;
    batch.done = cursor.item >= lastItem || batch.cursor.item >= spine_.size();
    return batch;
}

QByteArray WebResources::url(const QString &member) const
{
    const auto encoded = memberUrl(origin_, member);
    return members_.contains(encoded) ? encoded : QByteArray();
}

WebResources::Resource WebResources::read(const QByteArray &encodedUrl)
{
    constexpr int requestLimit = 4096;
    constexpr qint64 trafficLimit = Publication::maxBytes;
    if (lease_->revoked() || requests_ >= requestLimit || encodedUrl.size() > 16384)
        return {};
    ++requests_;
    const auto member = members_.constFind(encodedUrl);
    if (member == members_.cend() || member->size > trafficLimit - servedBytes_)
        return {};
    auto resource = readMember(*member);
    servedBytes_ += resource.bytes.size();
    return resource;
}

WebResources::Resource WebResources::readMember(const Member &member)
{
    const auto bytes = cache_->readBounded(member.name, member.size);
    if (lease_->revoked() || bytes.size() != member.size
        || !hasDeclaredSignature(bytes, member.media))
        return {};
    return { bytes, member.media };
}

QImage WebResources::coverImage(QSize maximumSize)
{
    const auto cover = members_.constFind(cover_);
    if (lease_->revoked() || cover == members_.cend() || maximumSize.isEmpty()
        || qint64(maximumSize.width()) * maximumSize.height() > 4 * 1024 * 1024)
        return {};
    const auto bytes = readMember(*cover).bytes;
    const auto header = inspectImageHeader(bytes);
    if (header.action != ImageHeader::Keep || header.size.isEmpty())
        return {};
    return decodePublicationCover(bytes, maximumSize);
}

bool Publication::inspect(const QByteArray &source, QString *error, OpeningResult *result)
{
    if (result)
        *result = {};
    if (source.size() > maxBytes) {
        *this = {};
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_size"));
    }
    QBuffer buffer;
    buffer.setData(source);
    buffer.open(QIODevice::ReadOnly);
    return inspect(buffer, error, result);
}

bool Publication::inspect(QIODevice &source, QString *error, OpeningResult *result)
{
    if (result)
        *result = {};
    names_.clear();
    sizes_.clear();
    skippedFonts_.clear();
    obfuscatedFonts_.clear();
    archive_.clear();
    directory_.clear();
    fingerprint_.clear();
    received_ = 0;
    protected_ = false;
    zip64_ = false;
    const qint64 length = source.size();
    if (source.isSequential() || length < 22)
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_size"));
    zip::Directory contents;
    if (!zip::readDirectory(source, contents))
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_directory"));
    zip64_ = contents.zip64;
    const auto &directory = contents.entries;
    const int directorySize = directory.size(), count = contents.count;
    auto read16 = [&](int p) {
        return qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(directory.constData() + p));
    };
    auto read32 = [&](int p) {
        return qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar *>(directory.constData() + p));
    };
    int at = 0;
    QSet<QString> seen;
    quint64 expanded = 0;
    bool unsupportedMetadata = false;
    for (int i = 0; i < count; ++i) {
        if (at > int(directorySize) - 46 || read32(at) != 0x02014b50)
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.invalid_publication_directory"));
        const int nameLength = read16(at + 28);
        const quint64 next = quint64(at) + 46 + nameLength + read16(at + 30) + read16(at + 32);
        zip::Member member;
        if (next > quint64(directorySize) || !zip::member(directory, at, member)
            || member.local >= contents.offset || member.packed > contents.offset - member.local)
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.unsupported_publication_member"));
        zip64_ |= read32(at + 20) == 0xffffffff || read32(at + 24) == 0xffffffff
                || read32(at + 42) == 0xffffffff || read16(at + 34) == 0xffff;
        if (read16(at + 8) & 0x41) {
            protected_ = true;
            return inspectionFailure(error, result, OpeningFailureKind::Protected,
                                     strings::key("error.unsupported_publication_member"));
        }
        const auto encoded = directory.mid(at + 46, nameLength);
        auto name = QString::fromUtf8(encoded);
        if (name.toUtf8() != encoded)
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.publication_member_name_is_not_utf_8"));
        const bool isDirectory = name.endsWith('/');
        if (isDirectory)
            name.chop(1);
        if (!safeName(name) || seen.contains(name))
            return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                     strings::key("error.ambiguous_publication_member_path"));
        seen.insert(name);
        if (member.size > 4ULL * 1024 * 1024 * 1024 - expanded)
            return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                     strings::key("error.expanded_publication_is_too_large"));
        expanded += member.size;
        // Kobalt does not open LCP-protected books. Their encryption metadata
        // would reject them below, but that check is about fonts and may change.
        if (name == "META-INF/license.lcpl") {
            protected_ = true;
            return inspectionFailure(error, result, OpeningFailureKind::Protected,
                                     strings::key("error.this_publication_is_lcp_protected"));
        }
        // Native content records remain Nickel's responsibility. Font and EPUB
        // encryption metadata need a separate, explicitly supported path.
        if (name == "META-INF/encryption.xml")
            unsupportedMetadata = true;
        if (!isDirectory) {
            names_.append(name);
            sizes_[name] = member.size;
        }
        at = int(next);
    }
    if (!zip64_ && length > 1024LL * 1024 * 1024)
        return inspectionFailure(error, result, OpeningFailureKind::Unsupported,
                                 strings::key("error.unsupported_publication_size"));
    if (at != int(directorySize) || !names_.contains("META-INF/container.xml")
        || !names_.contains("mimetype"))
        return inspectionFailure(error, result, OpeningFailureKind::InvalidContent,
                                 strings::key("error.missing_publication_container"));
    // Scan the whole directory before admitting supported font metadata, so an
    // LCP license or encrypted member is rejected regardless of entry order.
    QSet<QString> skippedFonts;
    if (unsupportedMetadata
        && !supportedFontMetadata(source, sizes_, skippedFonts, obfuscatedFonts_))
        return inspectionFailure(
                error, result, OpeningFailureKind::Unsupported,
                strings::key("error.this_publication_needs_unsupported_font_or_resource_handling"));
    skippedFonts_ = std::move(skippedFonts);
    for (const auto &font : skippedFonts_) {
        names_.removeOne(font);
        sizes_.remove(font);
    }
    names_.removeOne("mimetype");
    names_.prepend("mimetype");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray size;
    u32(size, quint32(length));
    u32(size, quint32(quint64(length) >> 32));
    hash.addData(size);
    hash.addData(directory);
    hash.addData(contents.endRecord);
    fingerprint_ = hash.result().toHex();
    return true;
}

bool Publication::append(const QString &name, const QByteArray &data, QString *error)
{
    if (received_ >= names_.size() || name != names_[received_])
        return fail(error, strings::get("error.publication_session_resource_mismatch"));
    if (data.size() > maxResource || (name == "mimetype" && data != "application/epub+zip"))
        return fail(error, strings::get("error.invalid_publication_resource_response"));
    const auto encoded = name.toUtf8();
    if (quint64(archive_.size()) + directory_.size() + data.size() + encoded.size() * 2 + 98
        > maxBytes)
        return fail(error, strings::get("error.publication_exceeds_the_memory_limit"));
    const quint32 offset = archive_.size(),
                  crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()), data.size());
    u32(archive_, 0x04034b50);
    u16(archive_, 20);
    u16(archive_, 0x800);
    u16(archive_, 0);
    u16(archive_, 0);
    u16(archive_, 0);
    u32(archive_, crc);
    u32(archive_, data.size());
    u32(archive_, data.size());
    u16(archive_, encoded.size());
    u16(archive_, 0);
    archive_.append(encoded);
    archive_.append(data);
    u32(directory_, 0x02014b50);
    u16(directory_, 20);
    u16(directory_, 20);
    u16(directory_, 0x800);
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u32(directory_, crc);
    u32(directory_, data.size());
    u32(directory_, data.size());
    u16(directory_, encoded.size());
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u32(directory_, 0);
    u32(directory_, offset);
    directory_.append(encoded);
    ++received_;
    return true;
}

QByteArray Publication::finish(QString *error)
{
    if (names_.isEmpty() || received_ != names_.size()) {
        fail(error, strings::get("error.publication_transfer_is_incomplete"));
        return {};
    }
    const quint32 offset = archive_.size(), length = directory_.size();
    archive_.append(directory_);
    directory_.clear();
    u32(archive_, 0x06054b50);
    u16(archive_, 0);
    u16(archive_, 0);
    u16(archive_, received_);
    u16(archive_, received_);
    u32(archive_, length);
    u32(archive_, offset);
    u16(archive_, 0);
    names_.clear();
    received_ = 0;
    return std::move(archive_);
}
} // namespace kobalt

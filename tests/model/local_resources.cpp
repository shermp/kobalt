#include "check.h"
#include "publication/local_resources.h"
#include "publication/publication.h"
#include "publication/web_resources.h"
#include "support/strings.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <thread>
#include <tuple>

#include <zlib.h>

using namespace kobalt;

namespace {
void u16(QByteArray &bytes, quint16 value)
{
    value = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&value), 2);
}

void u32(QByteArray &bytes, quint32 value)
{
    value = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&value), 4);
}

QByteArray zip(const QMap<QString, QByteArray> &files, bool deflated = false, bool badCrc = false)
{
    QByteArray bytes, directory;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const auto name = it.key().toUtf8(), data = it.value();
        QByteArray packed = data;
        if (deflated) {
            z_stream stream{};
            require(deflateInit2(&stream, 1, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK,
                    "deflate init");
            packed.resize(compressBound(data.size()));
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
            stream.avail_in = data.size();
            stream.next_out = reinterpret_cast<Bytef *>(packed.data());
            stream.avail_out = packed.size();
            require(deflate(&stream, Z_FINISH) == Z_STREAM_END, "deflate fixture");
            packed.resize(stream.total_out);
            deflateEnd(&stream);
        }
        const quint32 offset = bytes.size();
        const auto crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()), data.size())
                ^ (badCrc && it.key() == "data.bin" ? 1U : 0U);
        u32(bytes, 0x04034b50);
        u16(bytes, 20);
        u16(bytes, 0);
        u16(bytes, deflated ? 8 : 0);
        u32(bytes, 0);
        u32(bytes, crc);
        u32(bytes, packed.size());
        u32(bytes, data.size());
        u16(bytes, name.size());
        u16(bytes, 0);
        bytes += name;
        bytes += packed;
        u32(directory, 0x02014b50);
        u16(directory, 20);
        u16(directory, 20);
        u16(directory, 0);
        u16(directory, deflated ? 8 : 0);
        u32(directory, 0);
        u32(directory, crc);
        u32(directory, packed.size());
        u32(directory, data.size());
        u16(directory, name.size());
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, offset);
        directory += name;
    }
    const quint32 offset = bytes.size();
    bytes += directory;
    u32(bytes, 0x06054b50);
    u16(bytes, 0);
    u16(bytes, 0);
    u16(bytes, files.size());
    u16(bytes, files.size());
    u32(bytes, directory.size());
    u32(bytes, offset);
    u16(bytes, 0);
    return bytes;
}
void checkWebResources(const QString &fixture, const QString &scratch)
{
    auto checkBook = [](const QString &path) {
        QFile file(path);
        Publication publication;
        QString error;
        require(file.open(QIODevice::ReadOnly) && publication.inspect(file, &error),
                qPrintable(error));
        auto lease = std::make_shared<SourceLease>();
        auto cache = localResourceFactory(path, lease)(publication.sizes(), {}, {});
        auto resources = WebResources::open(publication, cache, lease, &error);
        require(resources && resources->fingerprint() == publication.fingerprint(),
                qPrintable(error));
        return std::make_tuple(std::move(resources), std::move(cache), std::move(lease));
    };

    auto [book, cache, lease] = checkBook(fixture);
    require(book->spine().contains(QStringLiteral("OEBPS/chapter1.xhtml"))
                    && book->navigation() == QStringLiteral("OEBPS/nav.xhtml"),
            "Type Tester has incorrect manifest order or navigation");
    const auto chapter = book->url("OEBPS/chapter1.xhtml");
    const int before = cache->statistics()["requests"].toInt();
    const auto reading = book->read(chapter);
    require(reading.mediaType == "application/xhtml+xml" && reading.bytes.contains("<p"),
            "Type Tester chapter did not load");
    require(book->read(chapter).bytes == reading.bytes
                    && cache->statistics()["requests"].toInt() == before + 1,
            "Scoped request extracted the same chapter twice");
    require(book->read(book->url("OEBPS/styles.css")).mediaType == "text/css"
                    && book->read(book->url("OEBPS/cover.png")).mediaType == "image/png",
            "Declared stylesheet or image did not load");
    const auto prefix = chapter.left(chapter.indexOf("/OEBPS/"));
    const auto requests = cache->statistics()["requests"].toInt();
    for (const auto &url : {QByteArray("https://example.org/OEBPS/chapter1.xhtml"),
                            QByteArray("file:///OEBPS/chapter1.xhtml"),
                            prefix + "/OEBPS/../OEBPS/chapter1.xhtml",
                            prefix + "/OEBPS/%2e%2e/OEBPS/chapter1.xhtml",
                            prefix + "/OEBPS%2Fchapter1.xhtml",
                            prefix + "/META-INF/container.xml",
                            prefix + "/OEBPS/toc.ncx", chapter + "?x", chapter + "#anchor"}) {
        require(book->read(url).bytes.isNull(), "Unauthorized URI was served");
    }
    auto [other, otherCache, otherLease] = checkBook(fixture);
    require(other->read(chapter).bytes.isNull()
                    && cache->statistics()["requests"].toInt() == requests,
            "Cross-opening URL or rejected resource reached the archive");
    lease->revoke();
    require(book->read(chapter).bytes.isNull(), "Revoked opening served cached bytes");

    QMap<QString, QByteArray> files{
        {"mimetype", "application/epub+zip"},
        {"META-INF/container.xml",
         "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles>"
         "<rootfile full-path='OPS/book.opf' media-type='application/oebps-package+xml'/>"
         "</rootfiles></container>"},
        {"OPS/ch ap.xhtml", "<html xmlns='http://www.w3.org/1999/xhtml'><body>Test</body></html>"},
        {"OPS/spoof.png", "<html>not a bitmap</html>"},
        {"OPS/big.css", QByteArray(2 * 1024 * 1024 + 1, 'x')},
        {"OPS/font.woff2", QByteArray("wOF2fixture")},
        {"OPS/hidden.xhtml", "<html>undeclared</html>"},
    };
    const QByteArray opf = "<package xmlns='http://www.idpf.org/2007/opf'><manifest>"
            "<item id='c' href='ch%20ap.xhtml' media-type='application/xhtml+xml'/>"
            "<item id='i' href='spoof.png' media-type='image/png'/>"
            "<item id='s' href='big.css' media-type='text/css'/>"
            "<item id='f' href='font.woff2' media-type='font/woff2'/>"
            "<item id='m' href='wrong.png' media-type='text/css'/>"
            "</manifest><spine><itemref idref='c'/></spine></package>";
    files["OPS/wrong.png"] = "body{}";
    auto openSynthetic = [&](const QByteArray &package, bool expectedAdmission = true) {
        files["OPS/book.opf"] = package;
        const auto bytes = zip(files, true);
        QFile file(scratch);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && file.write(bytes) == bytes.size(), "write scoped EPUB");
        file.close();
        Publication publication;
        QString error;
        require(publication.inspect(bytes, &error) == expectedAdmission, "Unexpected ZIP admission");
        return openLocalWebResources(scratch, std::make_shared<SourceLease>(), &error);
    };
    auto scoped = openSynthetic(opf);
    require(scoped && !scoped->read(scoped->url("OPS/ch ap.xhtml")).bytes.isNull()
                    && scoped->read(scoped->url("OPS/font.woff2")).mediaType == "font/woff2",
            "Canonical escaped name or supported font failed");
    for (const auto *member : {"OPS/big.css", "OPS/wrong.png", "OPS/hidden.xhtml"}) {
        require(scoped->url(QString::fromLatin1(member)).isEmpty(),
                "Oversized, mistyped or unlisted member was authorized");
    }
    require(scoped->read(scoped->url("OPS/spoof.png")).bytes.isNull(),
            "MIME signature mismatch was accepted");
    const auto pinnedChapter = scoped->url("OPS/ch ap.xhtml");
    auto rewritten = opf;
    require(bool(openSynthetic(rewritten)), "Replacement fixture did not open");
    require(scoped->read(pinnedChapter).bytes.isNull(), "Changed EPUB served cached resource bytes");
    for (const auto *href : {"../../outside.xhtml", "%2e%2e/ch.xhtml",
                             "dir%2fch.xhtml", "https://example.org/a.xhtml", "//remote/a.xhtml"}) {
        auto invalid = opf;
        require(!openSynthetic(invalid.replace("ch%20ap.xhtml", href)),
                "Malformed or external manifest URL was admitted");
    }
    auto unknownSpine = opf;
    require(!openSynthetic(unknownSpine.replace("idref='c'", "idref='missing'")),
            "Missing spine member was admitted");
    files["META-INF/license.lcpl"] = "{}";
    require(!openSynthetic(opf, false), "Protected book opened through the web scope");
    files.remove("META-INF/license.lcpl");
    files["META-INF/encryption.xml"] = "<encryption><EncryptedData/></encryption>";
    require(!openSynthetic(opf, false), "Unsupported encryption metadata was admitted");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    require(argc == 2, "Expected the Type Tester EPUB path");
    checkWebResources(QString::fromLocal8Bit(argv[1]), directory.path() + "/scoped.epub");
    const auto path = directory.path() + "/book#part?name%20.kepub.epub";
    QMap<QString, QByteArray> files{ { "mimetype", "application/epub+zip" },
                                     { "META-INF/container.xml", "<container/>" },
                                     { "book.opf", "<package/>" },
                                     { "chapter.xhtml", "<html><body><p>Recoverable HTML" },
                                     { "empty.bin", {} },
                                     { "data.bin",
                                       QByteArray(3 * ResourceCache::blockBytes + 9, 'a') } };
    auto write = [&](bool compressed = false, bool badCrc = false) {
        QFile file(path);
        const auto bytes = zip(files, compressed, badCrc);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && file.write(bytes) == bytes.size(),
                "write EPUB");
    };
    auto sizes = [&] {
        QMap<QString, qint64> result;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            result[it.key()] = it.value().size();
        }
        return result;
    };
    auto rejected = std::make_shared<std::atomic_bool>(false);
    for (bool compressed : { false, true }) {
        write(compressed);
        auto source = native::openLocalPublication(path);
        require(source && source->active() && source->port() == 0,
                "Local publication created a network endpoint");
        auto lease = std::make_shared<SourceLease>();
        auto factory = source->resources(lease);
        std::thread worker([&] {
            auto cache = factory(sizes(), "sans-serif", rejected);
            require(cache->readBounded("data.bin", ResourceCache::budgetBytes) == files["data.bin"],
                    "Block read differs");
            const auto requests = cache->statistics()["requests"].toInt();
            require(cache->readBounded("data.bin", ResourceCache::budgetBytes) == files["data.bin"]
                            && cache->statistics()["requests"].toInt() == requests,
                    "Cached bytes were read again");
            require(cache->readBounded("chapter.xhtml", ResourceCache::budgetBytes)
                                    == files["chapter.xhtml"]
                            && !rejected->load(),
                    "Recoverable chapter HTML was rejected as package XML");
            require(cache->size("empty.bin") == 0, "Empty resource failed");
            lease->revoke();
            require(cache->readBounded("data.bin", ResourceCache::budgetBytes).isNull()
                            && cache->failure() == ResourceCache::Failure::Cancelled,
                    "Revocation allowed cached bytes to be read");
        });
        worker.join();
    }
    write();
    {
        auto lease = std::make_shared<SourceLease>();
        lease->cancel();
        auto cache = localResourceFactory(path, lease)(sizes(), {}, rejected);
        require(cache->size("data.bin") < 0
                        && cache->failure() == ResourceCache::Failure::Cancelled,
                "Cancelled opening read the archive");
    }
    {
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->readBounded("data.bin", 100).isNull()
                        && cache->failure() == ResourceCache::Failure::Limit,
                "Local reads bypassed inspection limits");
        require(cache->resumeAfterInspection()
                        && cache->size("data.bin") == files["data.bin"].size(),
                "A bounded inspection prevented a normal read");
        cache->releaseBytes();
        files["data.bin"].fill('b');
        write();
        require(cache->size("data.bin") < 0 && cache->failure() == ResourceCache::Failure::Changed
                        && !cache->resumeAfterInspection(),
                "Changed resource bytes were reused");
        auto renewed =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(renewed->readBounded("data.bin", ResourceCache::budgetBytes) == files["data.bin"],
                "New lease kept old bytes");
    }
    for (bool compressed : { false, true }) {
        write(compressed, true);
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->size("data.bin") < 0 && cache->failure() == ResourceCache::Failure::Transfer,
                "Invalid resource CRC was accepted");
    }
    strings::showKeys(false);
    const auto xmlError = strings::get("error.publication_package_xml_is_invalid");
    require(xmlError != QStringLiteral("error.publication_package_xml_is_invalid"),
            "Malformed package XML has no display message");
    for (const auto &xml :
         { QByteArray(), QByteArray("<package>"), QByteArray("<package><broken></package>") }) {
        files["book.opf"] = xml;
        write();
        rejected->store(false);
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->size("book.opf") < 0 && rejected->load(),
                "Malformed package XML was accepted");
        require(cache->error() == xmlError, "Malformed package XML lost its display message");
    }
    files["book.opf"] = "<package/>";
    {
        QByteArray png = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452");
        auto dimension = [&](quint32 value) {
            value = qToBigEndian(value);
            png.append(reinterpret_cast<const char *>(&value), 4);
        };
        dimension(64);
        dimension(80);
        png.resize(300 * 1024);
        files["image.png"] = png;
        write();
        auto cache =
                localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->deferredImageSize("image.png") == QSize(64, 80)
                        && cache->statistics()["allocated_bytes"].toDouble() == 0,
                "Image dimension probe filled the source cache");
        const auto huge = qToBigEndian(quint32(8000));
        png.replace(16, 4, reinterpret_cast<const char *>(&huge), 4);
        png.replace(20, 4, reinterpret_cast<const char *>(&huge), 4);
        files["image.png"] = png;
        write();
        cache = localResourceFactory(path, std::make_shared<SourceLease>())(sizes(), {}, rejected);
        require(cache->readBounded("image.png", ResourceCache::budgetBytes).startsWith("<svg"),
                "Oversized bitmap escaped the pixel limit");
        auto oversized = sizes();
        oversized["image.png"] = ResourceCache::budgetBytes + 1;
        cache = localResourceFactory("/missing.epub",
                                     std::make_shared<SourceLease>())(oversized, {}, rejected);
        require(cache->readBounded("image.png", ResourceCache::budgetBytes).startsWith("<svg"),
                "Oversized image source was read instead of replaced");
    }
    files["META-INF/license.lcpl"] = "{}";
    write();
    require(!native::openLocalPublication(path), "Local backend accepted a protected publication");
    std::puts(
            "PASS: local worker reads, stored and deflated CRCs, cache reuse, limits, image probes, cancellation and revocation");
}

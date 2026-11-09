#include "epub.h"
#include <zlib.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

using kobalt::webkit::Epub;
using namespace std::string_literals;
using Entries = std::vector<std::pair<std::string, std::string>>;

static void u16(std::string &out, unsigned n)
{
    out += char(n);
    out += char(n >> 8);
}

static void u32(std::string &out, unsigned n)
{
    u16(out, n);
    u16(out, n >> 16);
}

static void archive(const std::string &path, const Entries &entries)
{
    std::string output, directory;
    for (const auto &[name, data] : entries) {
        unsigned offset = output.size(),
                 crc = crc32(0, reinterpret_cast<const Bytef *>(data.data()), data.size());
        u32(output, 0x04034b50);
        u16(output, 20);
        u16(output, 0);
        u16(output, 0);
        u32(output, 0);
        u32(output, crc);
        u32(output, data.size());
        u32(output, data.size());
        u16(output, name.size());
        u16(output, 0);
        output += name;
        output += data;
        u32(directory, 0x02014b50);
        u16(directory, 20);
        u16(directory, 20);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, crc);
        u32(directory, data.size());
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
    unsigned offset = output.size();
    output += directory;
    u32(output, 0x06054b50);
    u32(output, 0);
    u16(output, entries.size());
    u16(output, entries.size());
    u32(output, directory.size());
    u32(output, offset);
    u16(output, 0);
    std::ofstream(path, std::ios::binary).write(output.data(), output.size());
}

// Check untrusted ZIP integrity, bounded paths, package structure and link/resource admission.
int main()
{
    char temporary[] = "/tmp/kobalt-epub-test-XXXXXX";
    int fd = mkstemp(temporary);
    assert(fd >= 0);
    close(fd);
    std::string path = temporary;
    Entries entries{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)" },
        { "OEBPS/book.opf",
          R"(<package xmlns="http://www.idpf.org/2007/opf"><manifest><item id="one" href="text/one.xhtml" media-type="application/xhtml+xml"/><item id="two" href="text/two.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" properties="nav" media-type="application/xhtml+xml"/><item id="script" href="bad.js" media-type="text/javascript"/></manifest><spine><itemref idref="one"/><itemref idref="two"/></spine></package>)" },
        { "OEBPS/text/one.xhtml", "<html><body>One</body></html>" },
        { "OEBPS/text/two.xhtml", "<html><body id='target'>Two</body></html>" },
        { "OEBPS/nav.xhtml",
          R"(<html xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><a href="text/two.xhtml#target">Two</a></nav></body></html>)" },
        { "OEBPS/bad.js", "throw new Error('book scripts must not run')" }
    };
    archive(path, entries);
    Epub book;
    assert(book.open(path));
    assert(book.spine().size() == 2 && book.navigation().size() == 1);
    unsigned target = 9;
    std::string fragment, resource;
    assert(book.resolveLink(0, "two.xhtml#target", target, fragment) && target == 1
           && fragment == "target");
    assert(book.resolveLink(0, book.navigation()[0].href, target, fragment) && target == 1);
    assert(book.resolveLink(0, "#here", target, fragment) && target == 0 && fragment == "here");
    for (auto href :
         { "https://example.com/", "file:///etc/passwd", "javascript:alert(1)", "//host/book",
           "../../../../etc/passwd", "%2fetc/passwd", "%00", "two.xhtml?x=1", "..\\two.xhtml" })
        assert(!book.resolveLink(0, href, target, fragment));
    kobalt::webkit::Resource admitted;
    assert(book.resource("OEBPS/text/one.xhtml", admitted));
    assert(!book.resource("OEBPS/bad.js", admitted));
    assert(!book.resource("META-INF/container.xml", admitted));
    assert(book.read("OEBPS/text/one.xhtml", resource)
           && resource.find("One") != std::string::npos);
    auto oldLink = book.navigation()[0].href;
    assert(book.open(path));
    assert(!book.resolveLink(0, oldLink, target, fragment));
    auto epub2 = entries;
    auto &package = epub2[2].second;
    package.replace(
            package.find(
                    "href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\""),
            std::string(
                    "href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"")
                    .size(),
            "href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"");
    epub2[5] = {
        "OEBPS/toc.ncx",
        "<ncx><navMap><navPoint><navLabel><text>Chapter two</text></navLabel><content src='text/two.xhtml#target'/></navPoint></navMap></ncx>"
    };
    archive(path, epub2);
    assert(book.open(path) && book.navigation().size() == 1);
    assert(book.navigation()[0].title == "Chapter two");
    // Known byte vectors test obfuscation independently of the implementation's
    // key derivation, including XML whitespace and the unchanged suffix.
    const std::string idpfKey =
            "\x8e\xab\x87\x29\xd7\xa1\x0c\x8f\x8a\xd2\xab\xdc\x0f\x4e\x83\xe3\xb2\xb8\xb8\xac";
    const std::string adobeKey =
            "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff"s;
    std::string plain(1500, ' ');
    for (size_t i = 0; i < plain.size(); ++i)
        plain[i] = char(i % 251);
    for (const bool adobe : { false, true }) {
        auto fonts = entries;
        auto &opf = fonts[2].second;
        opf.insert(opf.find("<package") + 8, " unique-identifier='bookid'");
        opf.insert(
                opf.find("<manifest>"),
                "<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='bookid'> urn:example:\nkobalt </dc:identifier><dc:identifier>urn:uuid:00112233-4455-6677-8899-aabbccddeeff</dc:identifier></metadata>");
        opf.insert(opf.find("</manifest>"),
                   "<item id='font' href='font.ttf' media-type='font/ttf'/>");
        auto encoded = plain;
        const auto &key = adobe ? adobeKey : idpfKey;
        for (size_t i = 0; i < (adobe ? 1024u : 1040u); ++i)
            encoded[i] ^= key[i % key.size()];
        fonts.push_back({ "OEBPS/font.ttf", encoded });
        const std::string algorithm =
                adobe ? "http://ns.adobe.com/pdf/enc#RC" : "http://www.idpf.org/2008/embedding";
        fonts.push_back(
                { "META-INF/encryption.xml",
                  "<encryption xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><EncryptedData xmlns='http://www.w3.org/2001/04/xmlenc#'><EncryptionMethod Algorithm='"
                          + algorithm
                          + "'/><CipherData><CipherReference URI='OEBPS/font.ttf'/></CipherData></EncryptedData></encryption>" });
        archive(path, fonts);
        assert(book.open(path));
        assert(book.read("OEBPS/font.ttf", resource) && resource == plain);
        auto &encryption = fonts.back().second;
        encryption.replace(encryption.find("OEBPS/font.ttf"), 14, "OEBPS/text/one.xhtml");
        archive(path, fonts);
        assert(!book.open(path));
    }
    auto malformed = entries;
    malformed.push_back({ "../escape", "bad" });
    archive(path, malformed);
    assert(!book.open(path));
    malformed = entries;
    malformed.push_back(entries.back());
    archive(path, malformed);
    assert(!book.open(path));
    malformed = entries;
    malformed.push_back({ "META-INF/encryption.xml", "<encryption/>" });
    archive(path, malformed);
    assert(!book.open(path));
    malformed = entries;
    malformed[1].second = "<!DOCTYPE container [<!ENTITY x 'bad'>]><container>&x;</container>";
    archive(path, malformed);
    assert(!book.open(path));
    malformed = entries;
    malformed[2].second.replace(malformed[2].second.find("text/one.xhtml"), 14,
                                "../../../one.xhtml");
    archive(path, malformed);
    assert(!book.open(path));
    archive(path, entries);
    assert(book.open(path));
    std::fstream damaged(path, std::ios::binary | std::ios::in | std::ios::out);
    damaged.seekp(30 + entries[0].first.size());
    damaged.put('!');
    damaged.close();
    assert(!book.read("mimetype", resource));
    assert(!book.open(path));
    unlink(path.c_str());
    puts("EPUB admission checks passed");
}

#include "epub.h"
#include <glib.h>
#include <libxml/parser.h>
#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <sys/stat.h>

namespace kobalt::webkit {
namespace {
constexpr unsigned maxDirectory = 4 * 1024 * 1024, maxResource = 16 * 1024 * 1024;
using File = std::unique_ptr<FILE, int (*)(FILE *)>;
using Xml = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

unsigned u16(const std::string &s, size_t p)
{
    return uint8_t(s[p]) | unsigned(uint8_t(s[p + 1])) << 8;
}

uint32_t u32(const std::string &s, size_t p)
{
    return u16(s, p) | uint32_t(u16(s, p + 2)) << 16;
}

bool bytes(FILE *file, uint64_t at, size_t size, std::string &out)
{
    out.resize(size);
    return !fseeko(file, at, SEEK_SET) && fread(out.data(), 1, size, file) == size;
}

bool safeName(const std::string &name)
{
    if (name.empty() || name.size() > 4096 || name.front() == '/'
        || name.find_first_of("\\:\0", 0, 3) != std::string::npos
        || !g_utf8_validate(name.data(), name.size(), nullptr))
        return false;
    size_t start = 0;
    do {
        auto end = name.find('/', start);
        auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    } while (true);
}

std::string attribute(xmlNode *node, const char *name)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    std::string text = value ? reinterpret_cast<char *>(value) : "";
    xmlFree(value);
    return text;
}

bool element(xmlNode *node, const char *name)
{
    return node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name);
}

bool namespaceIs(xmlNode *node, const char *uri)
{
    return node && node->ns && xmlStrEqual(node->ns->href, BAD_CAST uri);
}

void walk(xmlNode *node, const std::function<void(xmlNode *)> &visit)
{
    for (; node; node = node->next) {
        visit(node);
        walk(node->children, visit);
    }
}

Xml xml(const std::string &bytes)
{
    Xml doc(xmlReadMemory(bytes.data(), bytes.size(), nullptr, nullptr,
                          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
            xmlFreeDoc);
    // Ordinary XHTML DOCTYPE declarations are allowed, but never load a DTD or
    // substitute entities. Internal declarations are unnecessary for EPUB input.
    if (doc && doc->intSubset && doc->intSubset->children)
        doc.reset();
    return doc;
}

bool token(const std::string &text, const std::string &wanted)
{
    gchar **parts = g_strsplit_set(text.c_str(), " \t\r\n", -1);
    bool found = false;
    for (unsigned i = 0; parts[i]; ++i)
        found |= parts[i] == wanted;
    g_strfreev(parts);
    return found;
}

bool fontType(const std::string &mime)
{
    return mime == "font/ttf" || mime == "font/otf" || mime == "font/woff" || mime == "font/woff2"
            || mime == "application/font-woff" || mime == "application/vnd.ms-opentype"
            || mime == "application/x-font-ttf" || mime == "application/x-font-opentype"
            || mime == "application/font-sfnt";
}

std::string uuidKey(std::string text)
{
    if (!g_ascii_strncasecmp(text.c_str(), "urn:uuid:", 9))
        text.erase(0, 9);
    if (!g_uuid_string_is_valid(text.c_str()))
        return {};
    std::string result;
    int high = -1;
    for (unsigned char c : text) {
        if (c == '-')
            continue;
        int value = g_ascii_xdigit_value(c);
        if (high < 0)
            high = value;
        else {
            result += char(high * 16 + value);
            high = -1;
        }
    }
    return result;
}
} // namespace

std::string jsonString(const std::string &text)
{
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\')
            result += '\\';
        if (c < 32) {
            char escaped[7];
            snprintf(escaped, sizeof escaped, "\\u%04x", c);
            result += escaped;
        } else
            result += char(c);
    }
    return result + '"';
}

bool Epub::fail(const char *message)
{
    error_ = message;
    return false;
}

bool Epub::open(const std::string &path)
{
    *this = {};
    filename_ = path;
    char *host = g_uuid_string_random();
    host_ = host;
    g_free(host);
    File file(fopen(path.c_str(), "rb"), fclose);
    struct stat info{};
    if (!file || fstat(fileno(file.get()), &info) || !S_ISREG(info.st_mode) || info.st_size < 22
        || uint64_t(info.st_size) > 1024ULL * 1024 * 1024)
        return fail("invalid-archive");
    size_t tailSize = std::min<uint64_t>(info.st_size, 65557);
    std::string tail;
    if (!bytes(file.get(), info.st_size - tailSize, tailSize, tail))
        return fail("archive-read-failed");
    size_t end = tail.size() - 22;
    while (u32(tail, end) != 0x06054b50 || end + 22 + u16(tail, end + 20) != tail.size()) {
        if (!end--)
            return fail("invalid-directory");
    }
    unsigned count = u16(tail, end + 10);
    uint32_t size = u32(tail, end + 12), offset = u32(tail, end + 16);
    if (!count || count > 4096 || u16(tail, end + 4) || u16(tail, end + 6)
        || count != u16(tail, end + 8) || size > maxDirectory
        || uint64_t(offset) + size != uint64_t(info.st_size) - tailSize + end)
        return fail("invalid-directory");
    std::string directory;
    if (!bytes(file.get(), offset, size, directory))
        return fail("archive-read-failed");
    identity_ = info;
    directoryOffset_ = offset;
    size_t at = 0;
    uint64_t expanded = 0;
    std::set<std::string> names;
    for (unsigned i = 0; i < count; ++i) {
        if (at + 46 > directory.size() || u32(directory, at) != 0x02014b50)
            return fail("invalid-directory");
        size_t next = at + 46 + u16(directory, at + 28) + u16(directory, at + 30)
                + u16(directory, at + 32);
        if (next > directory.size() || u16(directory, at + 34))
            return fail("invalid-directory");
        std::string name = directory.substr(at + 46, u16(directory, at + 28));
        bool folder = !name.empty() && name.back() == '/';
        if (folder)
            name.pop_back();
        if (!safeName(name) || !names.insert(name).second)
            return fail("ambiguous-member-path");
        Member member{ u32(directory, at + 42),           u32(directory, at + 20),
                       u32(directory, at + 24),           u32(directory, at + 16),
                       uint16_t(u16(directory, at + 10)), uint16_t(u16(directory, at + 8)) };
        expanded += member.size;
        if ((member.flags & 0x41) || (member.method != 0 && member.method != 8)
            || expanded > 4ULL * 1024 * 1024 * 1024
            || uint64_t(member.offset) + 30 + member.packed > offset)
            return fail("unsupported-member");
        if (name == "META-INF/license.lcpl")
            return fail("unsupported-protection");
        if (!folder)
            members_.emplace(name, member);
        at = next;
    }
    if (at != directory.size())
        return fail("invalid-directory");
    std::string mime;
    if (!read("mimetype", mime) || mime != "application/epub+zip")
        return fail("invalid-mimetype");
    return package();
}

bool Epub::read(const std::string &name, std::string &out) const
{
    out.clear();
    auto found = members_.find(name);
    if (found == members_.end())
        return false;
    const auto &m = found->second;
    if (m.size > maxResource || m.packed > maxResource)
        return false;
    File file(fopen(filename_.c_str(), "rb"), fclose);
    std::string header, localName, packed;
    struct stat current{};
    if (!file || fstat(fileno(file.get()), &current) || current.st_dev != identity_.st_dev
        || current.st_ino != identity_.st_ino || current.st_size != identity_.st_size
        || current.st_mtim.tv_sec != identity_.st_mtim.tv_sec
        || current.st_mtim.tv_nsec != identity_.st_mtim.tv_nsec
        || current.st_ctim.tv_sec != identity_.st_ctim.tv_sec
        || current.st_ctim.tv_nsec != identity_.st_ctim.tv_nsec
        || !bytes(file.get(), m.offset, 30, header) || u32(header, 0) != 0x04034b50
        || u16(header, 6) != m.flags || u16(header, 8) != m.method || u16(header, 26) != name.size()
        || uint64_t(m.offset) + 30 + name.size() + u16(header, 28) + m.packed > directoryOffset_
        || !bytes(file.get(), uint64_t(m.offset) + 30, name.size(), localName) || localName != name
        || !bytes(file.get(), uint64_t(m.offset) + 30 + name.size() + u16(header, 28), m.packed,
                  packed))
        return false;
    if (!m.method) {
        if (m.size != m.packed)
            return false;
        out = std::move(packed);
    } else {
        // One spare byte detects a stream that expands past its declared size.
        out.resize(size_t(m.size) + 1);
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef *>(packed.data());
        stream.avail_in = packed.size();
        stream.next_out = reinterpret_cast<Bytef *>(out.data());
        stream.avail_out = out.size();
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
            return false;
        int result = inflate(&stream, Z_FINISH);
        bool valid = result == Z_STREAM_END && stream.total_out == m.size && !stream.avail_in;
        inflateEnd(&stream);
        if (!valid) {
            out.clear();
            return false;
        }
        out.resize(m.size);
    }
    if (crc32(0, reinterpret_cast<const Bytef *>(out.data()), out.size()) != m.crc) {
        out.clear();
        return false;
    }
    // Check the stored ZIP member's CRC before reversible EPUB font
    // obfuscation. Never mutate the archive or decode non-font resources.
    auto font = obfuscated_.find(name);
    if (font != obfuscated_.end())
        for (size_t i = 0; i < std::min(out.size(), font->second.prefix); ++i)
            out[i] ^= font->second.key[i % font->second.key.size()];
    return true;
}

bool Epub::resolve(const std::string &base, const std::string &href, std::string &path,
                   std::string &fragment)
{
    if (href.size() > 4096 || href.find_first_of("\\\0", 0, 2) != std::string::npos)
        return false;
    auto hash = href.find('#');
    std::string relative = href.substr(0, hash);
    if (relative.find_first_of(":?") != std::string::npos
        || (!relative.empty() && relative.front() == '/'))
        return false;
    auto decode = [](const std::string &s, std::string &out) {
        gchar *decoded = g_uri_unescape_string(s.c_str(), nullptr);
        if (!decoded)
            return false;
        out = decoded;
        g_free(decoded);
        return bool(g_utf8_validate(out.data(), out.size(), nullptr));
    };
    if (!decode(relative, relative)
        || !decode(hash == std::string::npos ? "" : href.substr(hash + 1), fragment)
        || relative.find_first_of("\\:") != std::string::npos
        || (!relative.empty() && relative.front() == '/'))
        return false;
    auto slash = base.rfind('/');
    std::string joined = relative.empty()
            ? base
            : (slash == std::string::npos ? "" : base.substr(0, slash + 1)) + relative;
    std::vector<std::string> parts;
    size_t at = 0;
    do {
        size_t end = joined.find('/', at);
        auto part = joined.substr(at, end - at);
        if (part == "..") {
            if (parts.empty())
                return false;
            parts.pop_back();
        } else if (part != "." && !part.empty())
            parts.push_back(part);
        if (end == std::string::npos)
            break;
        at = end + 1;
    } while (true);
    path.clear();
    for (const auto &part : parts)
        path += (path.empty() ? "" : "/") + part;
    return safeName(path);
}

bool Epub::package()
{
    std::string data;
    if (!read("META-INF/container.xml", data) || data.size() > 2 * 1024 * 1024)
        return fail("missing-container");
    auto container = xml(data);
    if (!container || !element(xmlDocGetRootElement(container.get()), "container")
        || !namespaceIs(xmlDocGetRootElement(container.get()),
                        "urn:oasis:names:tc:opendocument:xmlns:container"))
        return fail("invalid-container");
    std::string opf;
    unsigned rootfiles = 0;
    walk(xmlDocGetRootElement(container.get()), [&](xmlNode *n) {
        if (element(n, "rootfile") && n->parent && element(n->parent, "rootfiles")
            && attribute(n, "media-type") == "application/oebps-package+xml") {
            opf = attribute(n, "full-path");
            ++rootfiles;
        }
    });
    if (rootfiles != 1 || !safeName(opf) || !read(opf, data) || data.size() > 2 * 1024 * 1024)
        return fail("invalid-package-path");
    auto doc = xml(data);
    if (!doc || !element(xmlDocGetRootElement(doc.get()), "package")
        || !namespaceIs(xmlDocGetRootElement(doc.get()), "http://www.idpf.org/2007/opf"))
        return fail("invalid-package");
    std::map<std::string, Resource> ids;
    std::string nav, ncx;
    std::string identifierId = attribute(xmlDocGetRootElement(doc.get()), "unique-identifier");
    std::string identifier, adobeKey;
    unsigned identifiers = 0;
    bool valid = true;
    walk(xmlDocGetRootElement(doc.get()), [&](xmlNode *n) {
        if (element(n, "identifier") && namespaceIs(n, "http://purl.org/dc/elements/1.1/")
            && n->parent && element(n->parent, "metadata")
            && n->parent->parent == xmlDocGetRootElement(doc.get())) {
            xmlChar *value = xmlNodeGetContent(n);
            std::string text = value ? reinterpret_cast<char *>(value) : "";
            xmlFree(value);
            if (!identifierId.empty() && attribute(n, "id") == identifierId) {
                identifier = text;
                ++identifiers;
            }
            if (adobeKey.empty()) {
                gchar *trimmed = g_strdup(text.c_str());
                adobeKey = uuidKey(g_strstrip(trimmed));
                g_free(trimmed);
            }
        }
        if (element(n, "meta") && attribute(n, "property") == "rendition:layout") {
            xmlChar *value = xmlNodeGetContent(n);
            valid &= !xmlStrEqual(value, BAD_CAST "pre-paginated");
            xmlFree(value);
        }
        if (!element(n, "item") || !namespaceIs(n, "http://www.idpf.org/2007/opf") || !n->parent
            || !element(n->parent, "manifest")
            || n->parent->parent != xmlDocGetRootElement(doc.get()))
            return;
        std::string path, fragment, id = attribute(n, "id"), mime = attribute(n, "media-type");
        if (id.empty() || ids.count(id) || attribute(n, "href").empty()
            || !resolve(opf, attribute(n, "href"), path, fragment) || !fragment.empty()
            || !members_.count(path) || resources_.count(path)) {
            valid = false;
            return;
        }
        Resource item{ path, mime };
        ids.emplace(id, item);
        resources_.emplace(path, item);
        if (token(attribute(n, "properties"), "nav"))
            nav = path;
        if (mime == "application/x-dtbncx+xml")
            ncx = path;
    });
    walk(xmlDocGetRootElement(doc.get()), [&](xmlNode *n) {
        if (element(n, "spine") && namespaceIs(n, "http://www.idpf.org/2007/opf")
            && n->parent == xmlDocGetRootElement(doc.get()))
            rightToLeft_ = attribute(n, "page-progression-direction") == "rtl";
        if (!element(n, "itemref") || !namespaceIs(n, "http://www.idpf.org/2007/opf") || !n->parent
            || !element(n->parent, "spine") || n->parent->parent != xmlDocGetRootElement(doc.get()))
            return;
        auto found = ids.find(attribute(n, "idref"));
        if (found == ids.end() || found->second.mime != "application/xhtml+xml"
            || token(attribute(n, "properties"), "rendition:layout-pre-paginated")) {
            valid = false;
            return;
        }
        // Nonlinear sections remain reachable by explicit spine index or link.
        auto item = found->second;
        item.linear = attribute(n, "linear") != "no";
        spine_.push_back(item);
        const auto step = [](xmlNode *node) {
            unsigned index = 2;
            for (auto *previous = node->prev; previous; previous = previous->prev)
                if (previous->type == XML_ELEMENT_NODE)
                    index += 2;
            return std::to_string(index);
        };
        spineCfi_.push_back("/" + step(n->parent) + "/" + step(n));
    });
    if (!valid || spine_.empty()
        || std::none_of(spine_.begin(), spine_.end(), [](const Resource &r) { return r.linear; }))
        return fail("unsupported-package");
    if (identifiers != 1)
        identifier.clear();
    if (!fonts(identifier, adobeKey))
        return false;
    const auto navigationPath = nav.empty() ? ncx : nav;
    if (!navigationPath.empty()) {
        if (!read(navigationPath, data) || data.size() > 2 * 1024 * 1024)
            return fail("invalid-navigation");
        auto navigation = xml(data);
        if (!navigation)
            return fail("invalid-navigation");
        xmlNode *scope = xmlDocGetRootElement(navigation.get());
        if (!nav.empty()) {
            scope = nullptr;
            walk(xmlDocGetRootElement(navigation.get()), [&](xmlNode *n) {
                xmlChar *type =
                        xmlGetNsProp(n, BAD_CAST "type", BAD_CAST "http://www.idpf.org/2007/ops");
                if (element(n, "nav") && type && token(reinterpret_cast<char *>(type), "toc"))
                    scope = n;
                xmlFree(type);
            });
        }
        if (scope)
            walk(scope->children, [&](xmlNode *n) {
                const bool anchor = element(n, "a"), content = element(n, "content");
                if (!anchor && !content)
                    return;
                std::string path, fragment;
                if (!resolve(navigationPath, attribute(n, anchor ? "href" : "src"), path, fragment))
                    return;
                unsigned target = 0;
                std::string ignored;
                char *escaped = g_uri_escape_string(fragment.c_str(), nullptr, FALSE);
                std::string href = uri(path) + (fragment.empty() ? "" : "#" + std::string(escaped));
                g_free(escaped);
                // Navigation destinations must be in the reading order.
                if (!resolveLink(0, href, target, ignored))
                    return;
                xmlNode *label = anchor ? n : n->parent;
                if (content)
                    for (auto *child = n->parent->children; child; child = child->next)
                        if (element(child, "navLabel")) {
                            label = child;
                            break;
                        }
                xmlChar *title = xmlNodeGetContent(label);
                int depth = 0;
                for (auto *parent = n->parent; parent && parent != scope; parent = parent->parent)
                    if (element(parent, anchor ? "ol" : "navPoint"))
                        ++depth;
                if (navigation_.size() < 4096)
                    navigation_.push_back({ title ? reinterpret_cast<char *>(title) : "", href,
                                            std::max(0, depth - 1) });
                xmlFree(title);
            });
    }
    return true;
}

bool Epub::fonts(const std::string &identifier, const std::string &adobeKey)
{
    if (!members_.count("META-INF/encryption.xml"))
        return true;
    std::string data;
    if (!read("META-INF/encryption.xml", data) || data.size() > 2 * 1024 * 1024)
        return fail("unsupported-protection");
    auto doc = xml(data);
    auto *root = doc ? xmlDocGetRootElement(doc.get()) : nullptr;
    if (!root || !element(root, "encryption")
        || !namespaceIs(root, "urn:oasis:names:tc:opendocument:xmlns:container"))
        return fail("unsupported-protection");
    std::string unique = identifier;
    unique.erase(
            std::remove_if(unique.begin(), unique.end(),
                           [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
            unique.end());
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(checksum, reinterpret_cast<const guchar *>(unique.data()), unique.size());
    std::string idpfKey(20, 0);
    gsize keySize = idpfKey.size();
    g_checksum_get_digest(checksum, reinterpret_cast<guint8 *>(idpfKey.data()), &keySize);
    g_checksum_free(checksum);
    std::map<std::string, ObfuscatedFont> fonts;
    constexpr const char *enc = "http://www.w3.org/2001/04/xmlenc#";
    for (auto *entry = root->children; entry; entry = entry->next) {
        if (entry->type != XML_ELEMENT_NODE)
            continue;
        if (!element(entry, "EncryptedData") || !namespaceIs(entry, enc))
            return fail("unsupported-protection");
        std::string algorithm, target;
        unsigned methods = 0, cipherData = 0, references = 0;
        for (auto *part = entry->children; part; part = part->next) {
            if (part->type != XML_ELEMENT_NODE)
                continue;
            if (!namespaceIs(part, enc))
                return fail("unsupported-protection");
            if (element(part, "EncryptionMethod")) {
                ++methods;
                algorithm = attribute(part, "Algorithm");
                for (auto *child = part->children; child; child = child->next)
                    if (child->type == XML_ELEMENT_NODE)
                        return fail("unsupported-protection");
            } else if (element(part, "CipherData")) {
                ++cipherData;
                for (auto *reference = part->children; reference; reference = reference->next) {
                    if (reference->type != XML_ELEMENT_NODE)
                        continue;
                    if (!element(reference, "CipherReference") || !namespaceIs(reference, enc))
                        return fail("unsupported-protection");
                    ++references;
                    target = attribute(reference, "URI");
                    for (auto *child = reference->children; child; child = child->next)
                        if (child->type == XML_ELEMENT_NODE)
                            return fail("unsupported-protection");
                }
            } else
                return fail("unsupported-protection");
        }
        std::string path, fragment;
        const bool idpf = algorithm == "http://www.idpf.org/2008/embedding";
        const bool adobe = algorithm == "http://ns.adobe.com/pdf/enc#RC";
        if (methods != 1 || cipherData != 1 || references != 1 || (!idpf && !adobe)
            || (idpf && unique.empty()) || (adobe && adobeKey.size() != 16)
            || !resolve("", target, path, fragment) || !fragment.empty() || !resources_.count(path)
            || !fontType(resources_.at(path).mime) || fonts.count(path))
            return fail("unsupported-protection");
        fonts.emplace(path, ObfuscatedFont{ idpf ? idpfKey : adobeKey, idpf ? 1040u : 1024u });
    }
    if (fonts.empty())
        return fail("unsupported-protection");
    obfuscated_ = std::move(fonts);
    return true;
}

std::string Epub::uri(const std::string &path) const
{
    char *escaped = g_uri_escape_string(path.c_str(), "/", FALSE);
    std::string result = "kobalt-epub://" + host_ + "/" + std::string(escaped);
    g_free(escaped);
    return result;
}

bool Epub::resource(const std::string &name, Resource &result) const
{
    auto found = resources_.find(name);
    if (found == resources_.end())
        return false;
    result = found->second;
    // Manifest script/object/video entries are never served.
    return result.mime == "application/xhtml+xml" || result.mime == "text/css"
            || result.mime == "image/svg+xml" || result.mime == "image/png"
            || result.mime == "image/jpeg" || result.mime == "image/gif"
            || result.mime == "image/webp" || fontType(result.mime);
}

bool Epub::resolveLink(unsigned spine, const std::string &href, unsigned &target,
                       std::string &fragment) const
{
    if (spine >= spine_.size())
        return false;
    std::string path, relative = href;
    std::string prefix = "kobalt-epub://" + host_ + "/";
    bool absolute = relative.compare(0, prefix.size(), prefix) == 0;
    if (absolute)
        relative.erase(0, prefix.size());
    if (!resolve(absolute ? "" : spine_[spine].path, relative, path, fragment))
        return false;
    for (unsigned i = 0; i < spine_.size(); ++i)
        if (spine_[i].path == path) {
            target = i;
            return true;
        }
    return false;
}

std::string Epub::description() const
{
    std::string result = "{\"spine\":[";
    for (const auto &item : spine_)
        result += (result.back() == '[' ? "" : ",") + jsonString(item.path);
    result += "],\"linear\":[";
    for (const auto &item : spine_)
        result += std::string(result.back() == '[' ? "" : ",") + (item.linear ? "true" : "false");
    result += "],\"spine_cfi\":[";
    for (const auto &cfi : spineCfi_)
        result += (result.back() == '[' ? "" : ",") + jsonString(cfi);
    result += "],\"navigation\":[";
    for (const auto &item : navigation_)
        result += std::string(result.back() == '[' ? "" : ",")
                + "{\"title\":" + jsonString(item.title) + ",\"href\":" + jsonString(item.href)
                + ",\"depth\":" + std::to_string(item.depth) + "}";
    return result + "],\"page_progression_rtl\":" + (rightToLeft_ ? "true" : "false") + "}";
}
} // namespace kobalt::webkit

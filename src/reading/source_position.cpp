#include "source_position.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace kobalt::sourceposition {
namespace {
const QString prefix = QStringLiteral("kobalt-source:1:");
}

bool valid(const QJsonObject &value)
{
    if (value.value("version").toDouble() != 1 || !value.value("href").isString()
        || !value.value("cfi").isString() || !value.value("offset").isDouble()
        || !value.value("before").isString() || !value.value("after").isString())
        return false;
    const auto href = value.value("href").toString();
    if (href.isEmpty() || href.size() > 1024 || href.startsWith('/') || href.contains('\\')
        || href.contains(':') || href.contains('#') || href.contains('?')
        || href.contains(QChar(0)))
        return false;
    for (const auto &part : href.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    const auto cfi = value.value("cfi").toString();
    // Initial interoperable subset: a package/spine path, content path and
    // optional UTF-16 character offset. Ranges and embedded CFI assertions
    // remain unavailable; bounded supplementary text assertions live in JSON.
    static const QRegularExpression syntax(
            "^epubcfi\\((?:/[1-9][0-9]*)+!(?:/[1-9][0-9]*)+(?::(?:0|[1-9][0-9]*))?\\)$");
    const auto offset = value.value("offset").toDouble();
    return cfi.size() <= 2048 && syntax.match(cfi).hasMatch() && std::isfinite(offset)
            && offset >= value.value("before").toString().size() && offset <= 2147483647
            && offset == std::floor(offset) && value.value("before").toString().size() <= 64
            && value.value("after").toString().size() <= 64
            && QJsonDocument(value).toJson(QJsonDocument::Compact).size() <= 4096;
}

QString token(const QJsonObject &value)
{
    return valid(value) ? prefix
                    + QString::fromLatin1(
                                  QJsonDocument(value).toJson(QJsonDocument::Compact).toBase64())
                        : QString();
}

QJsonObject fromToken(const QString &value)
{
    if (!value.startsWith(prefix) || value.size() > 5500)
        return {};
    const auto bytes = value.mid(prefix.size()).toLatin1();
    const auto decoded = QByteArray::fromBase64(bytes);
    if (decoded.toBase64() != bytes)
        return {};
    const auto result = QJsonDocument::fromJson(decoded).object();
    return valid(result) ? result : QJsonObject();
}
} // namespace kobalt::sourceposition

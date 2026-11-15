#pragma once

#include "location.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace kobalt {
inline constexpr auto sourcePositionKey = "source_location";

inline QJsonObject sourcePosition(const Location &location)
{
    QJsonArray path;
    for (const auto index : location.path)
        path.append(index);
    QJsonObject result;
    result["version"] = location.version;
    result["href"] = location.href;
    result["path"] = path;
    result["offset"] = location.offset;
    result["after"] = location.after;
    return result;
}

inline Location readSourcePosition(const QJsonObject &object)
{
    const auto href = object.value("href").toString();
    const auto pathValue = object.value("path");
    const auto path = pathValue.toArray();
    const auto offset = object.value("offset").toInt(-2);
    const auto after = object.value("after");
    if (object.value("version").toInt(-1) != Location::currentVersion || href.isEmpty()
        || href.size() > 1400 || href.contains(QChar(0)) || !pathValue.isArray()
        || path.size() > 128 || offset < -1 || offset > 2 * 1024 * 1024 || !after.isBool())
        return {};
    Location result{ Location::currentVersion, href, {}, offset, after.toBool() };
    for (const auto &entry : path) {
        const auto index = entry.toInt(-1);
        if (index < 0 || index > 2 * 1024 * 1024)
            return {};
        result.path.append(index);
    }
    return result;
}

inline QString sourcePositionToken(const Location &location, const QByteArray &fingerprint)
{
    if (!location.valid() || fingerprint.isEmpty())
        return {};
    auto object = sourcePosition(location);
    object["fingerprint"] = QString::fromLatin1(fingerprint);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return bytes.size() <= 4096 ? QString::fromUtf8(bytes) : QString();
}

inline Location sourcePositionFromToken(const QString &token, const QByteArray &fingerprint)
{
    if (!token.startsWith('{') || token.size() > 4096 || fingerprint.isEmpty())
        return {};
    const auto object = QJsonDocument::fromJson(token.toUtf8()).object();
    return object.value("fingerprint").toString().toLatin1() == fingerprint
            ? readSourcePosition(object)
            : Location{};
}

inline Location savedSourcePosition(const QJsonObject &saved, const QByteArray &fingerprint,
                                    bool forBlaze = false)
{
    const auto record = saved.value(sourcePositionKey).toObject();
    const auto engine = record.value("engine").toString();
    if (fingerprint.isEmpty() || (engine != "wpe" && engine != "koblaze")
        || record.value("fingerprint").toString().toLatin1() != fingerprint
        || (forBlaze && engine != "wpe") || record.value("position") != saved.value("position")
        || record.value("dom_version") != saved.value("dom_version"))
        return {};
    return readSourcePosition(record);
}

inline bool rememberSourcePosition(QJsonObject &saved, const Location &location,
                                   const QByteArray &fingerprint, const QString &engine)
{
    const auto old = saved.value(sourcePositionKey).toObject();
    const auto oldEngine = old.value("engine").toString();
    if (!location.valid() || fingerprint.isEmpty()
        || (saved.contains(sourcePositionKey)
            && (!readSourcePosition(old).valid()
                || (oldEngine != "wpe" && oldEngine != "koblaze"))))
        return false;
    auto record = old;
    const auto position = sourcePosition(location);
    for (auto it = position.constBegin(); it != position.constEnd(); ++it)
        record[it.key()] = it.value();
    record["fingerprint"] = QString::fromLatin1(fingerprint);
    record["engine"] = engine;
    if (saved.contains("position"))
        record["position"] = saved.value("position");
    else
        record.remove("position");
    if (saved.contains("dom_version"))
        record["dom_version"] = saved.value("dom_version");
    else
        record.remove("dom_version");
    if (QJsonDocument(record).toJson(QJsonDocument::Compact).size() > 8192)
        return false;
    saved[sourcePositionKey] = record;
    return true;
}
} // namespace kobalt

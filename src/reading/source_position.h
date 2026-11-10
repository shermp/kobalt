#pragma once
#include <QJsonObject>
#include <QString>

namespace kobalt::sourceposition {
// Versioned publication-source locator. This is independent of layout, page
// numbers, WebKit's DOM objects and CREngine's native XPointer representation.
bool valid(const QJsonObject &);
QString token(const QJsonObject &);
QJsonObject fromToken(const QString &);
} // namespace kobalt::sourceposition

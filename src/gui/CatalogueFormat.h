#pragma once

// CatalogueFormat.h
// The format /v1/catalogue/open is asked to read a file as, from its name.

#include <QFileInfo>
#include <QString>

namespace visivo {

inline QString catalogueFormatForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    // A VisIVO Binary Table of points (issue #83), by its header or its binary.
    if (suffix == QStringLiteral("head") || suffix == QStringLiteral("bin"))
        return QStringLiteral("vbt");
    if (suffix == QStringLiteral("csv"))
        return QStringLiteral("csv");
    if (suffix == QStringLiteral("vot") || suffix == QStringLiteral("votable")
        || suffix == QStringLiteral("xml"))
        return QStringLiteral("votable");
    if (suffix == QStringLiteral("fits") || suffix == QStringLiteral("fit"))
        return QStringLiteral("fits");
    if (suffix == QStringLiteral("speck"))
        return QStringLiteral("speck");
    if (suffix == QStringLiteral("tbl"))
        return QStringLiteral("ipac");
    return QStringLiteral("csv");
}

} // namespace visivo

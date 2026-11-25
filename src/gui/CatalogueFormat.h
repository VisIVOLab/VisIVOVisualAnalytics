#pragma once

// CatalogueFormat.h
// The format /v1/catalogue/open is asked to read a file as, from its name.

#include <QFileInfo>
#include <QString>

namespace visivo {

inline QString catalogueFormatForPath(const QString &path)
{
    // A particle type of a Gadget snapshot (#31), by its reference. (The
    // backend tells a snapshot by its content whatever format it is asked for.)
    if (path.contains(QStringLiteral("::gadget-type=")))
        return QStringLiteral("gadget");
    // A family of a RAMSES output (#549), by its reference.
    if (path.contains(QStringLiteral("::ramses-family=")))
        return QStringLiteral("ramses");
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

/// What a table of particles is called on screen (the command bar's badge and
/// the Session Data row): the format the backend read it as, VBT when it says
/// nothing or the file is one. Session Data named every point table VBT, a
/// RAMSES output's included (issue #563).
inline QString particleFormatLabel(const QString &tableFormat, const QString &path)
{
    if (tableFormat.isEmpty() || catalogueFormatForPath(path) == QStringLiteral("vbt"))
        return QStringLiteral("VBT");
    return tableFormat.toUpper();
}

} // namespace visivo

#pragma once
// ImageLayerContext.h
// What the image viewer shows ABOUT the selected layer (its FITS header, its
// intensity unit) and how the Inspector words an axis. Plain string logic, no
// widgets and no VTK, so it is unit-tested.
//
// Layers stack in one window, and the window's own file is only the first of
// them: the header, the title and the unit are those of the layer the user
// selected, not of the base image.

#include "FitsHeaderCard.h"

#include <QString>
#include <QStringList>

namespace visivo::layerctx {

/// The file of the layer at \a row of \a layerPaths, or \a basePath when the row
/// is out of range (nothing selected) or its path is empty.
inline QString pathOfLayer(const QStringList &layerPaths, int row, const QString &basePath)
{
    if (row < 0 || row >= layerPaths.size() || layerPaths.at(row).isEmpty())
        return basePath;
    return layerPaths.at(row);
}

/// The value of keyword \a key among FITS header \a cards (80-column images), or
/// an empty string. A value is read as the card parser reads it, so a BUNIT of
/// 'Jy/beam' keeps its slash. Commentary cards never carry a value.
inline QString headerKeyword(const QStringList &cards, const QString &key)
{
    for (const QString &raw : cards) {
        const FitsHeaderCard card = parseFitsHeaderCard(raw);
        if (!card.commentary && card.key.compare(key, Qt::CaseInsensitive) == 0)
            return card.value.trimmed();
    }
    return {};
}

/// An axis of the Inspector's axis table: its type, its unit, and whether the
/// image is collapsed along it. The third axis of a 2-D image (a velocity of
/// 'VRAD [m s-1]' in a continuum plane) is a coordinate of the plane, not a
/// unit of the values, and reads like one without the word.
inline QString axisTypeLabel(const QString &ctype, const QString &cunit, bool collapsed)
{
    const QString type = ctype.trimmed();
    const QString unit = cunit.trimmed();
    QStringList detail;
    if (!unit.isEmpty())
        detail << unit;
    if (collapsed)
        detail << QStringLiteral("collapsed");
    return detail.isEmpty() ? type : QStringLiteral("%1 (%2)").arg(type, detail.join(QStringLiteral(", ")));
}

/// The tooltip of an axis row: "Axis 3: VRAD (m s-1, collapsed)".
inline QString axisRowText(int axis, const QString &ctype, const QString &cunit, bool collapsed)
{
    return QStringLiteral("Axis %1: %2").arg(axis).arg(axisTypeLabel(ctype, cunit, collapsed));
}

} // namespace visivo::layerctx

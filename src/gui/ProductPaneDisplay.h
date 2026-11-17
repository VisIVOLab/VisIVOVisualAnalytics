#pragma once

// ProductPaneDisplay.h
// What the cube window says about a 2-D product pane: the Scale control's
// three segments, and the one-line summary of how the shown product was made
// that the status line carries while the pane is active (issue #404).
//
// No widgets and no VTK, so it can be tested.

#include "vtk/LutScale.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace visivo {

/// The segment of the Linear / Log / Sqrt control that shows `mode`. Square and
/// Power are set in the colour-map editor and have no segment; they show as
/// Linear, as the slice's control always has.
inline int scaleSegment(LutScale::Mode mode)
{
    switch (mode) {
    case LutScale::Mode::Log:  return 1;
    case LutScale::Mode::Sqrt: return 2;
    default:                   return 0;
    }
}

/// The stretch a segment of the control stands for.
inline LutScale::Mode scaleForSegment(int segment)
{
    return segment == 1   ? LutScale::Mode::Log
         : segment == 2   ? LutScale::Mode::Sqrt
                          : LutScale::Mode::Linear;
}

/// A short account of how a product was made, from its label and the recipe
/// it was registered with (the same parameters Inspector › Parameters lists):
/// "Moment 0 · channels 1–512 · mask off". Only the parameters that tell two
/// maps of the same kind apart are kept; the rest stay in the Inspector.
inline QString productSummary(const QString &label, const QVariantMap &params)
{
    using namespace Qt::StringLiterals;
    QStringList parts;
    const QString name = label.trimmed();
    if (!name.isEmpty())
        parts << name;
    const auto text = [&params](const QString &key) {
        return params.value(key).toString().trimmed();
    };
    // A line-width map's label already names the method ("FWHM …").
    if (const QString method = text(u"Method"_s);
        !method.isEmpty() && !name.contains(method, Qt::CaseInsensitive))
        parts << method;
    if (QString channels = text(u"Channels"_s); !channels.isEmpty())
        parts << u"channels "_s + channels.replace(u".."_s, u"–"_s);
    if (params.contains(u"Mask"_s)) {
        const QString mask = text(u"Mask"_s);
        const bool off = mask.isEmpty() || mask == u"none"_s || mask == u"off"_s
                || mask == u"-"_s;
        parts << (off ? u"mask off"_s : u"mask "_s + QString(mask).replace(u">="_s, u"≥"_s));
    }
    if (const QString phi = text(u"φ range"_s); !phi.isEmpty())
        parts << u"φ "_s + phi;
    if (const QString path = text(u"Path"_s); !path.isEmpty())
        parts << u"path "_s + path;
    if (const QString width = text(u"Width"_s); !width.isEmpty())
        parts << u"width "_s + width;
    if (const QString region = text(u"Region"_s); !region.isEmpty())
        parts << u"region "_s + region;
    return parts.join(u" · "_s);
}

} // namespace visivo

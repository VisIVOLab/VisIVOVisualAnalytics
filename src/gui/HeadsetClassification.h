#pragma once

// HeadsetClassification.h
// A catalogue classified in the VR headset (issue #279) as the cube window's
// catalogue table shows it: one entry per classified or found source, placed
// at the backend's own pixel (the cube's full WCS, so the table and its
// markers sit where the headset drew them), its sky position kept for the
// table, and the classification as columns: the flag (under the headset's
// flag column name), the note, the velocity, where it came from and its row
// in the catalogue. The full catalogue with the flag column is the VOTable
// the backend wrote (the event's file): Save VOTable copies it.
//
// Widget-free and header-only: unit-tested in tests/test_vr_headset_hub.cpp.

#include "CatalogueOverlayUtils.h"
#include "VrHeadsetHub.h"

#include <QString>
#include <QStringList>

#include <cmath>
#include <vector>

namespace VrHub {

/// The table's own columns, after the ones every catalogue has.
inline QStringList classificationFields(const Classification &c)
{
    return { c.column.isEmpty() ? QStringLiteral("vr_flag") : c.column, QStringLiteral("Note"),
             QStringLiteral("Velocity (km/s)"), QStringLiteral("Channel"), QStringLiteral("Origin"),
             QStringLiteral("Catalogue row") };
}

inline QString numberText(double v, int decimals)
{
    return std::isfinite(v) ? QString::number(v, 'f', decimals) : QString();
}

/// The sky frame of the table's two sky columns: the catalogue's (its first
/// source with a position), else ICRS (a found source's position is ICRS).
inline bool classificationGalactic(const Classification &c)
{
    for (const ClassifiedSource &s : c.sources)
        if (s.origin != u"user" && std::isfinite(s.lon))
            return s.galactic;
    return false;
}

inline QString classificationFrameLabel(const Classification &c)
{
    return classificationGalactic(c) ? QStringLiteral("Galactic") : QStringLiteral("ICRS");
}

/// The entries of the table: image-frame markers at the backend's 0-based
/// pixels, with the sky position when the headset knew it in the table's
/// frame (a found source's ICRS position over a Galactic catalogue is left
/// out: its pixel and velocity place it).
inline std::vector<CatalogueOverlayEntry> classificationEntries(const Classification &c)
{
    std::vector<CatalogueOverlayEntry> out;
    const QStringList fields = classificationFields(c);
    const bool galactic = classificationGalactic(c);
    out.reserve(static_cast<std::size_t>(c.sources.size()));
    for (const ClassifiedSource &s : c.sources) {
        CatalogueOverlayEntry e;
        e.frame = CatalogueOverlayEntry::Frame::Image;
        e.shape = CatalogueOverlayEntry::Shape::Marker;
        e.pixelX = s.x;
        e.pixelY = s.y;
        e.label = s.label;
        e.hasSkyPosition = std::isfinite(s.lon) && std::isfinite(s.lat) && s.galactic == galactic;
        if (e.hasSkyPosition) {
            e.raDeg = s.lon;
            e.decDeg = s.lat;
        }
        const QString origin = s.origin == u"user" ? QStringLiteral("found in VR")
                : s.origin == u"overlay"           ? QStringLiteral("overlay")
                                                   : QStringLiteral("catalogue");
        e.band = s.flag.isEmpty() ? QStringLiteral("unflagged") : s.flag;
        e.extra = { { fields.at(0), s.flag },
                    { fields.at(1), s.note },
                    { fields.at(2), numberText(s.velocityKms, 1) },
                    { fields.at(3), numberText(s.z, 1) },
                    { fields.at(4), origin },
                    { fields.at(5), s.row >= 0 ? QString::number(s.row) : QString() } };
        out.push_back(e);
    }
    return out;
}

} // namespace VrHub

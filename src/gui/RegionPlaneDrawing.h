#ifndef RegionPlaneDrawing_h
#define RegionPlaneDrawing_h

// RegionPlaneDrawing.h
// The region list as the backend render session draws it on its cutting
// plane (issue #356): the `set_regions` command of a Remote 3-D view.
//
// With the 3-D view rendered on the backend the frames come from the
// session, which owns the camera and the depth buffer, so the session draws
// the regions itself. It is sent what the desktop's plane overlay draws
// (RegionListOverlay with no fill and no handles): each region's closed
// outlines, open paths, point markers and labels, in the cube's
// full-resolution 0-based pixels, its colour and whether it is selected. The
// session places pixel (x, y) at world (x, y, channel): its world is the
// cube's voxels at full resolution, where the desktop's single placement
// formula (RegionCubeSampling.h planePoint) puts it at scale 1.
//
// Pure, header-only, unit-tested in tests/test_region_model.cpp.

#include "RegionModel.h"

#include <QJsonArray>
#include <QJsonObject>

#include <array>
#include <cmath>
#include <functional>

namespace visivo::regions {

/// A region's colour, 0..255 (RegionListOverlay::colour in the app).
using RegionColour = std::function<std::array<unsigned char, 3>(const Region &)>;

namespace detail {

/// Pixel positions to a thousandth: the payload stays small, and a
/// thousandth of a pixel is far below anything a screen shows.
inline double rounded(double v) { return std::round(v * 1000.0) / 1000.0; }

inline QJsonArray pointJson(const Vec2 &p)
{
    return QJsonArray{ rounded(p[0]), rounded(p[1]) };
}

inline QJsonArray polylinesJson(const std::vector<std::vector<Vec2>> &lines)
{
    QJsonArray out;
    for (const auto &line : lines) {
        if (line.size() < 2)
            continue;
        QJsonArray pts;
        for (const Vec2 &p : line)
            pts.append(pointJson(p));
        out.append(pts);
    }
    return out;
}

}   // namespace detail

/// The `set_regions` command for @p list. An empty list clears the plane.
inline QJsonObject planeDrawingCommand(const RegionList &list, const RegionColour &colour)
{
    QJsonArray regions;
    for (const Region &r : list.regions()) {
        QJsonObject o;
        const auto rgb = colour(r);
        o[QStringLiteral("rgb")] = QJsonArray{ int(rgb[0]), int(rgb[1]), int(rgb[2]) };
        o[QStringLiteral("selected")] = r.id == list.selectedId();
        const QJsonArray loops = detail::polylinesJson(outlines(r));
        if (!loops.isEmpty())
            o[QStringLiteral("loops")] = loops;
        const QJsonArray paths = detail::polylinesJson(openPaths(r));
        if (!paths.isEmpty())
            o[QStringLiteral("paths")] = paths;
        if (r.shape == Shape::Point)
            o[QStringLiteral("points")] = QJsonArray{ detail::pointJson(r.center) };
        // Where the overlay writes them: a text region's string at its
        // position, a file's label for any other region above its middle.
        QJsonArray labels;
        if (r.shape == Shape::Text) {
            if (!r.text.isEmpty())
                labels.append(QJsonObject{ { QStringLiteral("text"), r.text },
                                           { QStringLiteral("at"), detail::pointJson(r.center) } });
        } else {
            const QString fileLabel = r.props.value(QStringLiteral("text")).toString();
            if (!fileLabel.isEmpty()) {
                const auto b = bounds(r);
                labels.append(QJsonObject{
                        { QStringLiteral("text"), fileLabel },
                        { QStringLiteral("at"), detail::pointJson({ (b[0] + b[1]) / 2.0, b[3] }) } });
            }
        }
        if (!labels.isEmpty())
            o[QStringLiteral("labels")] = labels;
        regions.append(o);
    }
    return QJsonObject{ { QStringLiteral("type"), QStringLiteral("set_regions") },
                        { QStringLiteral("regions"), regions } };
}

}   // namespace visivo::regions

#endif   // RegionPlaneDrawing_h

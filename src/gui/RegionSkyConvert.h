#ifndef RegionSkyConvert_h
#define RegionSkyConvert_h

// RegionSkyConvert.h
// Carry canonical regions from one dataset's pixels to another's through the
// sky (issue #522): a region drawn on the radio map becomes the same patch of
// sky on the cube's slice, and the other way round.
//
// A region is stored in its dataset's 0-based pixels (RegionModel.h). The
// vertices of a polygon or a line, and the centre of every other shape, go
// through the full WCS of both datasets. What the pixels of a shape measure
// (a radius, the axes of an ellipse, the sides of a box, an angle) go through
// the local linear map of the two pixel grids at the shape's centre
// (pixelJacobian), so a circle stays a circle when the pixel scales differ
// and a box turns with the rotation between the two grids. For a patch of
// sky the size of a source that is the same thing; a polygon, whose vertices
// are exact, is the shape to use for a region as large as the field.
//
// Pure QtCore + std, unit-tested in tests/test_sky_wcs.cpp.

#include "RegionModel.h"
#include "SkyWcs.h"

#include <optional>
#include <vector>

namespace visivo::skylink {

/// @p r in the pixels of @p to, or nothing when a vertex or the centre is off
/// @p to's projection (or either WCS cannot place it).
inline std::optional<regions::Region> convertRegion(const regions::Region &r, const SkyWcs &from,
                                                    const SkyWcs &to)
{
    using regions::Shape;
    regions::Region out = r;
    // What it was typed or read in no longer describes it.
    out.sourceFrame.clear();
    out.sourceUnit.clear();

    if (regions::isPointList(r.shape)) {
        for (std::size_t i = 0; i < r.points.size(); ++i) {
            double tx = 0.0, ty = 0.0;
            if (!matchPixel(from, r.points[i][0], r.points[i][1], to, tx, ty))
                return std::nullopt;
            out.points[i] = { tx, ty };
        }
        return out;
    }

    double tx = 0.0, ty = 0.0;
    std::array<double, 4> m{};
    if (!matchPixel(from, r.center[0], r.center[1], to, tx, ty)
        || !pixelJacobian(from, r.center[0], r.center[1], to, m))
        return std::nullopt;
    out.center = { tx, ty };
    const double scale = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
    if (!(scale > 0.0) || !std::isfinite(scale))
        return std::nullopt;

    // The direction an angle points in, after the two grids' relative turn.
    const auto turned = [&m](double angleDeg) {
        const double c = std::cos(angleDeg * kDeg), s = std::sin(angleDeg * kDeg);
        return std::atan2(m[2] * c + m[3] * s, m[0] * c + m[1] * s) / kDeg;
    };
    const auto scaled = [scale](const regions::Vec2 &v) { return regions::Vec2{ v[0] * scale, v[1] * scale }; };

    switch (r.shape) {
    case Shape::Point:
    case Shape::Text:
        break;
    case Shape::Circle:
        out.radius = r.radius * scale;
        break;
    case Shape::Annulus:
        out.radii = scaled(r.radii);
        break;
    case Shape::Ellipse:
        out.semiAxes = scaled(r.semiAxes);
        out.angleDeg = turned(r.angleDeg);
        break;
    case Shape::Box:
        out.size = scaled(r.size);
        out.angleDeg = turned(r.angleDeg);
        break;
    case Shape::EllipseAnnulus:
    case Shape::BoxAnnulus:
        out.inner = scaled(r.inner);
        out.outer = scaled(r.outer);
        out.angleDeg = turned(r.angleDeg);
        break;
    case Shape::Vector:
        out.length = r.length * scale;
        out.angleDeg = turned(r.angleDeg);
        break;
    case Shape::Polygon:
    case Shape::Line:
        break;   // handled above
    }
    return out;
}

/// Every region of @p list that can be placed on @p to, in the same order;
/// @p notPlaced counts the ones that cannot (off the projection).
inline std::vector<regions::Region> convertRegions(const std::vector<regions::Region> &list,
                                                   const SkyWcs &from, const SkyWcs &to,
                                                   int *notPlaced = nullptr)
{
    std::vector<regions::Region> out;
    int missed = 0;
    for (const regions::Region &r : list) {
        if (auto c = convertRegion(r, from, to))
            out.push_back(std::move(*c));
        else
            ++missed;
    }
    if (notPlaced)
        *notPlaced = missed;
    return out;
}

/// The vertices of a path (a line region, a PV path) in @p to's pixels; empty
/// when any vertex is off its projection.
inline std::vector<std::array<double, 2>> convertPath(const std::vector<regions::Vec2> &path,
                                                      const SkyWcs &from, const SkyWcs &to)
{
    std::vector<std::array<double, 2>> out;
    out.reserve(path.size());
    for (const regions::Vec2 &v : path) {
        double tx = 0.0, ty = 0.0;
        if (!matchPixel(from, v[0], v[1], to, tx, ty))
            return {};
        out.push_back({ tx, ty });
    }
    return out;
}

}   // namespace visivo::skylink

#endif   // RegionSkyConvert_h

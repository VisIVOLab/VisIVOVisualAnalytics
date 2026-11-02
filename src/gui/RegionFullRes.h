#ifndef RegionFullRes_h
#define RegionFullRes_h

// RegionFullRes.h
// A region drawn on the displayed image, in the full-resolution pixel grid of
// the file (issue #95).
//
// Regions are drawn and hit-tested in the displayed image's voxels. Below full
// resolution a displayed voxel stands for a block of `spacing` x `spacing`
// source pixels, centred at `origin + (v - extent0) * spacing` (where the image
// viewer places it: sampled or averaged, see ImageLod). The backend computes a
// region's statistics on the file, so the region is handed over in source
// pixels, covering whole blocks:
//
//   * box: the area its voxels cover on screen, from half a block before its
//     first voxel's centre to half a block after its last one (the far edge
//     excluded: it is the next voxel's), as a centre and a size. On averaged
//     levels that is exactly the voxels' blocks;
//   * circle and annulus: the centre's centre, radii times `spacing`;
//   * polygon: each vertex's centre.
//
// At spacing 1 these are the viewer's own rules (CubeRegionGeom): the same
// pixels are counted.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <array>
#include <cmath>
#include <vector>

namespace visivo {

struct DisplayGrid
{
    double originX{ 0. }, originY{ 0. };
    double spacingX{ 1. }, spacingY{ 1. };
    int extentX0{ 0 }, extentY0{ 0 };

    double sourceX(double v) const { return originX + (v - extentX0) * spacingX; }
    double sourceY(double v) const { return originY + (v - extentY0) * spacingY; }
    bool reduced() const { return spacingX > 1.001 || spacingY > 1.001; }
};

/// The region in source pixels, as a canonical image-frame region (0-based,
/// `frame: image`) for `POST /v1/regions/stats/{dataset_id}`. `shape` is "box",
/// "circle", "annulus" or "polygon"; anything else gives an empty object.
inline QJsonObject regionInSourcePixels(const QString &shape, const DisplayGrid &g,
                                        const std::array<int, 2> &anchor,
                                        const std::array<int, 2> &current,
                                        const std::vector<std::array<int, 2>> &polygon,
                                        double innerRadius)
{
    QJsonObject o;
    o[QStringLiteral("shape")] = shape;
    o[QStringLiteral("frame")] = QStringLiteral("image");
    o[QStringLiteral("unit")] = QStringLiteral("pix");
    if (shape == QLatin1String("box")) {
        const int vx0 = std::min(anchor[0], current[0]), vx1 = std::max(anchor[0], current[0]);
        const int vy0 = std::min(anchor[1], current[1]), vy1 = std::max(anchor[1], current[1]);
        // Reduced: the area the voxels cover on screen, half a block either
        // side of the end voxels' centres, the near edge in and the far edge
        // out (it is the next voxel's). The canonical box is |x - c| <= w/2,
        // so the far edge is pulled in by a hair.
        double x0 = g.sourceX(vx0), x1 = g.sourceX(vx1);
        double y0 = g.sourceY(vy0), y1 = g.sourceY(vy1);
        if (g.reduced()) {
            constexpr double eps = 1e-6;
            x0 -= g.spacingX / 2.0;
            x1 += g.spacingX / 2.0 - eps;
            y0 -= g.spacingY / 2.0;
            y1 += g.spacingY / 2.0 - eps;
        }
        o[QStringLiteral("center")] = QJsonArray{ (x0 + x1) / 2.0, (y0 + y1) / 2.0 };
        o[QStringLiteral("size")] = QJsonArray{ x1 - x0, y1 - y0 };
        o[QStringLiteral("angle_deg")] = 0.0;
        return o;
    }
    if (shape == QLatin1String("circle") || shape == QLatin1String("annulus")) {
        const double dx = static_cast<double>(current[0] - anchor[0]);
        const double dy = static_cast<double>(current[1] - anchor[1]);
        const double radius = std::sqrt(dx * dx + dy * dy) * g.spacingX;
        o[QStringLiteral("center")] = QJsonArray{ g.sourceX(anchor[0]), g.sourceY(anchor[1]) };
        if (shape == QLatin1String("circle"))
            o[QStringLiteral("radius")] = radius;
        else
            o[QStringLiteral("radii")] =
                    QJsonArray{ std::max(0.0, innerRadius) * g.spacingX, radius };
        return o;
    }
    if (shape == QLatin1String("polygon")) {
        QJsonArray points;
        for (const auto &v : polygon)
            points.append(QJsonArray{ g.sourceX(v[0]), g.sourceY(v[1]) });
        o[QStringLiteral("points")] = points;
        return o;
    }
    return {};
}

} // namespace visivo

#endif // RegionFullRes_h

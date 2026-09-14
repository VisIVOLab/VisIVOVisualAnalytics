#ifndef CubeRegionGeom_h
#define CubeRegionGeom_h

// Shared ROI hit-testing + annulus-fill helpers, identical between the cube and
// image viewers (deduplicated). Free functions, no class state / no RegionMode.
//
// The four hit-testers are pure geometry (no VTK) and live inline here (moved
// verbatim from CubeRegionGeom.cpp, E-04) so they can be unit-tested headless
// without linking the VTK-dependent buildAnnulusFill. Behaviour is unchanged.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

class vtkCellArray;
class vtkPoints;
class vtkPolyData;

namespace CubeRegionGeom {

inline bool pointInBox(const std::array<int, 2> &anchor, const std::array<int, 2> &current,
                       int x, int y)
{
    const int xmin = std::min(anchor[0], current[0]);
    const int xmax = std::max(anchor[0], current[0]);
    const int ymin = std::min(anchor[1], current[1]);
    const int ymax = std::max(anchor[1], current[1]);
    return x >= xmin && x <= xmax && y >= ymin && y <= ymax;
}

inline bool pointInCircle(const std::array<int, 2> &anchor, const std::array<int, 2> &current,
                          int x, int y)
{
    const double dx = static_cast<double>(current[0] - anchor[0]);
    const double dy = static_cast<double>(current[1] - anchor[1]);
    const double radius = std::sqrt(dx * dx + dy * dy);
    if (radius <= 0.0) {
        return x == anchor[0] && y == anchor[1];
    }
    const double px = static_cast<double>(x - anchor[0]);
    const double py = static_cast<double>(y - anchor[1]);
    return (px * px + py * py) <= radius * radius;
}

// Ray-casting point-in-polygon over integer vertices, sampled at the pixel CENTRE
// (x+0.5, y+0.5) so edge pixels test consistently; needs ≥3 vertices; the
// horizontal-edge (yj==yi) case is guarded against a divide-by-zero.
inline bool pointInPolygon(const std::vector<std::array<int, 2>> &vertices, int x, int y)
{
    if (vertices.size() < 3) {
        return false;
    }

    bool inside = false;
    const double px = static_cast<double>(x) + 0.5;
    const double py = static_cast<double>(y) + 0.5;
    for (std::size_t i = 0, j = vertices.size() - 1; i < vertices.size(); j = i++) {
        const double xi = static_cast<double>(vertices[i][0]) + 0.5;
        const double yi = static_cast<double>(vertices[i][1]) + 0.5;
        const double xj = static_cast<double>(vertices[j][0]) + 0.5;
        const double yj = static_cast<double>(vertices[j][1]) + 0.5;
        const bool intersects = ((yi > py) != (yj > py))
                && (px < (xj - xi) * (py - yi) / ((yj - yi) == 0.0 ? 1e-12 : (yj - yi)) + xi);
        if (intersects) {
            inside = !inside;
        }
    }
    return inside;
}

inline bool pointInAnnulus(const std::array<int, 2> &center, const std::array<int, 2> &outerPoint,
                           double innerRadius, int x, int y)
{
    const double outerDx = static_cast<double>(outerPoint[0] - center[0]);
    const double outerDy = static_cast<double>(outerPoint[1] - center[1]);
    const double outerRadius = std::sqrt(outerDx * outerDx + outerDy * outerDy);
    if (outerRadius <= 0.0) {
        return false;
    }

    const double px = static_cast<double>(x - center[0]);
    const double py = static_cast<double>(y - center[1]);
    const double radius = std::sqrt(px * px + py * py);
    return radius >= std::max(0.0, innerRadius) && radius <= outerRadius;
}

// ── Bounding boxes to scan ───────────────────────────────────────────────────
// The hit-testers above are only asked about pixels inside these bounds, so a
// box that does not cover the shape silently drops part of the measurement.
// That is exactly what happened to circles and annuli in both viewers: their
// geometry is a CENTRE plus a point on the rim, and taking the box between
// those two corners scans one quadrant. A circle's statistics were computed
// over a quarter of the circle, in the direction the user happened to drag.

/// {xmin, xmax, ymin, ymax} spanned by the two opposite corners of a box.
inline std::array<int, 4> boxBounds(const std::array<int, 2> &anchor,
                                    const std::array<int, 2> &current)
{
    return { std::min(anchor[0], current[0]), std::max(anchor[0], current[0]),
             std::min(anchor[1], current[1]), std::max(anchor[1], current[1]) };
}

/// {xmin, xmax, ymin, ymax} around a centre and a point on the rim. The radius
/// is rounded OUT so a rim pixel is never cut off by truncation.
inline std::array<int, 4> circleBounds(const std::array<int, 2> &center,
                                       const std::array<int, 2> &rimPoint)
{
    const double dx = static_cast<double>(rimPoint[0] - center[0]);
    const double dy = static_cast<double>(rimPoint[1] - center[1]);
    const int r = static_cast<int>(std::ceil(std::sqrt(dx * dx + dy * dy)));
    return { center[0] - r, center[0] + r, center[1] - r, center[1] + r };
}

/// {xmin, xmax, ymin, ymax} of a polygon, from its vertices alone. The in-progress
/// cursor vertex is passed as one of them while drawing; a restored polygon has no
/// cursor, and folding in unrelated anchor/current values would inflate the box.
inline std::array<int, 4> polygonBounds(const std::vector<std::array<int, 2>> &vertices)
{
    if (vertices.empty())
        return { 0, 0, 0, 0 };
    int xmin = vertices.front()[0], xmax = xmin;
    int ymin = vertices.front()[1], ymax = ymin;
    for (const auto &v : vertices) {
        xmin = std::min(xmin, v[0]);
        xmax = std::max(xmax, v[0]);
        ymin = std::min(ymin, v[1]);
        ymax = std::max(ymax, v[1]);
    }
    return { xmin, xmax, ymin, ymax };
}

// VTK-dependent (fills a triangle-strip); defined in CubeRegionGeom.cpp.
void buildAnnulusFill(vtkPoints *points, vtkCellArray *cells, vtkPolyData *polyData,
                      const std::array<int, 2> &center, double innerRadius, double outerRadius,
                      int sides);

}  // namespace CubeRegionGeom

#endif

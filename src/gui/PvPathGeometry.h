#ifndef PvPathGeometry_h
#define PvPathGeometry_h

// Geometry of an editable PV (position-velocity) path, free of VTK and Qt so it
// is unit-testable: hit-testing handles and segments, moving, rotating and
// re-shaping the path, and the smooth curve through its control vertices.
//
// Coordinates are whatever the caller works in. Hit tests take DISPLAY
// coordinates (the tolerance is in screen pixels, so it does not depend on the
// zoom); edits take the cube's pixel coordinates, where vertices may be
// fractional — an edited path is not snapped to the grid.

#include <array>
#include <cstddef>
#include <vector>

namespace pvpath {

using Vertex = std::array<double, 2>;
using Path = std::vector<Vertex>;

/// Index of the vertex nearest to @p p within @p tolerance, or -1.
int hitVertex(const Path &points, const Vertex &p, double tolerance);

struct SegmentHit
{
    int segment{ -1 };    ///< segment i joins vertex i and i + 1; -1 = no hit
    double t{ 0.0 };      ///< position along it, 0..1
    double distance{ 0.0 };
};
/// The segment nearest to @p p within @p tolerance (segment -1 when none).
SegmentHit hitSegment(const Path &points, const Vertex &p, double tolerance);

Vertex centroid(const Path &path);
Path translated(const Path &path, double dx, double dy);
/// Rotated by @p radians (counter-clockwise) about @p center.
Path rotated(const Path &path, const Vertex &center, double radians);
/// Angle of @p p seen from @p center (atan2).
double angleAround(const Vertex &center, const Vertex &p);
/// @p p inserted between vertices @p segment and @p segment + 1.
Path withVertexInserted(const Path &path, int segment, const Vertex &p);
/// Vertex @p index removed; the path is returned unchanged if that would leave
/// fewer than two vertices or the index is out of range.
Path withVertexRemoved(const Path &path, int index);

/// A smooth curve through every control vertex: centripetal Catmull-Rom, which
/// neither overshoots into loops nor cusps on unevenly spaced vertices.
/// @p samplesPerSegment points per span; the control vertices are kept exactly.
/// Fewer than three vertices: returned as is (a straight cut).
Path smoothed(const Path &control, int samplesPerSegment = 8);

/// Polyline length.
double length(const Path &path);

/// A PV diagram (row-major, @p ny rows of @p nx, one row per channel in CHANNEL
/// order) laid out for a plot whose rows are EQUALLY spaced from the lowest to the
/// highest spectral value — what QCPColorMap draws. @p spectral = each channel's
/// coordinate. On a linear axis that is the channels themselves, reversed when
/// the axis decreases (CDELT3 < 0); on a non-linear one (e.g. VOPT-F2W) every
/// plot row takes the channel nearest to its value, on a grid as fine as the
/// closest two channels, so no channel is drawn at someone else's velocity.
/// Every channel appears at least once: if that grid would exceed
/// max(4096, 8 × channels) rows, the rows are the channels in value order.
struct UniformRows
{
    std::vector<double> values;   ///< rows × nx
    int rows{ 0 };
};
UniformRows rowsOnUniformSpectralGrid(const std::vector<double> &values, int nx, int ny,
                                      const std::vector<double> &spectral);

}  // namespace pvpath

#endif  // PvPathGeometry_h

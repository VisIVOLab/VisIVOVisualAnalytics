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

#include <QString>

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

/// Pixel -> sky matrix, row-major [a, b, c, d] (any one unit): a pixel offset
/// (dx, dy) is (a dx + b dy, c dx + d dy) on the sky.
using SkyMatrix = std::array<double, 4>;

/// The sample step the backend cuts with, in the matrix's unit: the shorter
/// pixel side on the sky.
double sampleStep(const SkyMatrix &m);

/// The two edges of the band a cut of @p widthPixels averages over, in the
/// backend's convention (issue #3's geometry): the band is widthPixels sample
/// steps wide, measured on the SKY along each vertex's normal (the mean of its
/// adjacent segments'), so it is right for pixels that are not square. Both
/// edges have as many vertices as @p path; a single-pixel width has none.
struct Band
{
    Path upper;
    Path lower;
};
Band bandEdges(const Path &path, int widthPixels, const SkyMatrix &m);

/// The polygon of the band: the upper edge along, the lower one back. Empty for
/// a width of one pixel or fewer than two vertices.
Path bandPolygon(const Path &path, int widthPixels, const SkyMatrix &m);

/// A width in pixels as angular width, @p stepArcsec being what one pixel of
/// width is on the sky (the response's pixel_scale_arcsec_per_pixel).
inline double widthArcsec(int widthPixels, double stepArcsec) { return widthPixels * stepArcsec; }
/// The pixel width nearest to @p arcsec, at least 1; 1 when the step is unknown.
int widthPixelsForArcsec(double arcsec, double stepArcsec);
/// "3 px (3.00 arcsec)", or "3 px" when the scale is unknown.
QString widthText(int widthPixels, double stepArcsec);

/// How vertex @p index (0-based) is named on the display and everywhere the
/// path is described: counted from 1.
inline QString vertexName(std::size_t index) { return QString::number(index + 1); }
/// "1 (12.0, 4.5); 2 (30.0, 8.0)": every vertex under its number, in cube pixels.
QString vertexListText(const Path &vertices);

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

/// What the status bar tells the user while a PV path is being drawn. Nothing
/// on the image says the path ends with a double-click (or a right-click), so
/// the hint stays up from the first point until the path is finished (issue #502).
inline QString drawingHint(std::size_t vertexCount)
{
    if (vertexCount == 0)
        return QStringLiteral("PV path: click on the image to add the first point; "
                              "double-click (or right-click) to finish.");
    if (vertexCount == 1)
        return QStringLiteral("PV path: 1 point. Click to add points, double-click (or "
                              "right-click) to finish.");
    return QStringLiteral("PV path: %1 points. Click to add points, double-click (or "
                          "right-click) to finish.").arg(vertexCount);
}

}  // namespace pvpath

#endif  // PvPathGeometry_h

#ifndef RAYTRACEOVERLAY_H
#define RAYTRACEOVERLAY_H

// What a ray-traced picture is read by (issue #101): the data's bounding box,
// projected as the renderer projects the particles, and the colour bar of its
// colours. Drawn over the GPU view and onto the stills and movie frames the
// backend renders, so what is saved is what was shown.

#include <QJsonObject>
#include <QString>
#include <QVector3D>

#include <array>
#include <vector>

class QPainter;
class QRectF;

namespace visivo {

struct RaytraceOverlay
{
    bool box = true;
    bool colourbar = true;
    bool hasBounds = false;
    std::array<double, 6> bounds{};   ///< xmin xmax ymin ymax zmin zmax
    /// The renderer projects without perspective (a projected map).
    bool parallel = false;
    // The colour bar: colours from ``lo`` (bottom) to ``hi`` (top).
    bool hasBar = false;
    std::vector<QVector3D> lut;
    double lo = 0.0, hi = 1.0;
    bool log = false;
    QString title;
};

/// Draws ``overlay`` over ``target`` (a picture of the view through
/// ``camera``, in the target's coordinates): the box's edges, and the colour
/// bar at its right, both sized to the target's height.
void paintRaytraceOverlay(QPainter &painter, const QRectF &target, const QJsonObject &camera,
                          const RaytraceOverlay &overlay);

} // namespace visivo

#endif // RAYTRACEOVERLAY_H

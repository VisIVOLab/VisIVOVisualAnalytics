#pragma once

// SelectionMarkerSize.h
// How big the hover and selection spheres of the 3-D catalogue viewer are on
// screen (issue #159). The marker starts from a size in the data (the glyph it
// wraps, or a fraction of the particles' mean spacing) and is then kept
// between a few pixels and a bound: a sphere sized only by the data could
// cover half the view, or vanish to a dot. Widget-free, so it is unit-tested.

#include <algorithm>
#include <cmath>

namespace visivo {

/// World units under one screen pixel, for a point @p depth in front of the
/// camera (along its direction of projection). @p viewAngleDeg is VTK's
/// vertical view angle; a parallel projection shows 2 × @p parallelScale over
/// the viewport's height whatever the depth. 0 when nothing is known.
inline double worldPerPixel(bool parallel, double viewAngleDeg, double parallelScale,
                            double depth, double viewportHeightPx)
{
    if (!(viewportHeightPx > 0.0))
        return 0.0;
    if (parallel)
        return parallelScale > 0.0 ? 2.0 * parallelScale / viewportHeightPx : 0.0;
    if (!(depth > 0.0) || !(viewAngleDeg > 0.0) || !(viewAngleDeg < 180.0))
        return 0.0;
    const double halfAngle = viewAngleDeg * 0.5 * 3.14159265358979323846 / 180.0;
    return 2.0 * depth * std::tan(halfAngle) / viewportHeightPx;
}

/// The screen sizes a marker keeps to, in pixels of radius.
struct MarkerPixelRange
{
    double minPx;
    double maxPx;
};

/// A glyph (ellipsoid, sphere, cross) is wrapped by its marker, so the marker
/// grows with it, up to a bound; a point, a splat or an arrow has no body of
/// its own to wrap, and its marker stays a small ring.
inline MarkerPixelRange markerPixelRange(bool wrapsGlyph)
{
    return wrapsGlyph ? MarkerPixelRange{ 6.0, 48.0 } : MarkerPixelRange{ 5.0, 12.0 };
}

/// @p radiusWorld kept within @p range on screen. Unchanged when the scale is
/// unknown (@p worldPerPx 0), so a marker is never lost for want of a camera.
inline double clampMarkerRadius(double radiusWorld, double worldPerPx, MarkerPixelRange range)
{
    if (!(worldPerPx > 0.0))
        return radiusWorld;
    const double lo = range.minPx * worldPerPx;
    const double hi = range.maxPx * worldPerPx;
    if (!(radiusWorld > 0.0) || !std::isfinite(radiusWorld))
        return lo;
    return std::clamp(radiusWorld, lo, hi);
}

}  // namespace visivo

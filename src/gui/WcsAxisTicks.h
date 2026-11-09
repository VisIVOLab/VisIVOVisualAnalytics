#ifndef WCS_AXIS_TICKS_H
#define WCS_AXIS_TICKS_H
// WcsAxisTicks.h
// Where the coordinate printed beside a WCS axis tick is read.
//
// The 2-D views draw a straight x axis along the bottom of the image and a
// straight y axis down its left side, with five ticks each. Both used to read
// tick i at the same voxel, (xMin + t·w, yMin + t·h): a point on the image's
// DIAGONAL. That is right only while each sky coordinate depends on one pixel
// axis. In another frame it is not: an equatorial cube shown in Galactic
// coordinates (HCG 44) has b changing mostly along x, so the y axis printed,
// at each height, the latitude of a point far to the right of the axis. Its
// labels ran 54.59 (top) to 55.54 (bottom) where astropy gives 56.02 and 55.58
// along the left edge, and the hover readout (right) disagreed with the axis
// it sat beside.
//
// A tick names the coordinate at the tick: the x axis is read along the
// bottom edge (y = yMin), the y axis along the left edge (x = xMin).
//
// Header-only, so the rule is tested without a window (TestWcsAxisTicks).

namespace visivo {

/// A position in voxel (pixel-index) coordinates of the displayed image.
struct AxisVoxel
{
    double x{ 0.0 };
    double y{ 0.0 };
};

/// The voxel the x axis's tick at fraction @p t (0 = left end, 1 = right
/// end) labels: on the bottom edge, where the axis is drawn.
inline AxisVoxel xAxisTickVoxel(double t, double xMin, double xMax, double yMin)
{
    return { xMin + t * (xMax - xMin), yMin };
}

/// The voxel the y axis's tick at fraction @p t (0 = bottom, 1 = top) labels:
/// on the left edge, where the axis is drawn.
inline AxisVoxel yAxisTickVoxel(double t, double yMin, double yMax, double xMin)
{
    return { xMin, yMin + t * (yMax - yMin) };
}

}   // namespace visivo

#endif   // WCS_AXIS_TICKS_H

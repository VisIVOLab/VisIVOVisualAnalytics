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

#include <QString>

#include <algorithm>
#include <cmath>

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

/// How far apart two longitudes (or any angle) are, the short way round:
/// 359.99 and 0.01 are 0.02° apart, not 359.98°.
inline double angularAxisSpan(double a, double b)
{
    double d = std::fabs(a - b);
    if (d > 180.0)
        d = 360.0 - d;
    return d;
}

/// Decimals for the degree labels of an axis spanning @p axisSpan degrees with
/// five ticks (spacing span/4): enough that neighbouring ticks differ, so a
/// field of a few arcseconds does not print "29.53" on every tick (issue #502).
/// A span that is unknown (≤ 0, the hover readout) keeps two decimals.
inline int degreeTickDecimals(double axisSpan)
{
    if (!(axisSpan > 0.0) || !std::isfinite(axisSpan))
        return 2;
    const double spacing = axisSpan / 4.0;
    return std::clamp(static_cast<int>(std::ceil(-std::log10(spacing))) + 1, 2, 8);
}

/// A degree label at the precision the axis needs; a tiny negative value at
/// the origin reads "0.000", not "-0.000".
inline QString formatDegreeTick(double value, double axisSpan)
{
    QString text = QString::number(value, 'f', degreeTickDecimals(axisSpan));
    if (text.startsWith(QLatin1Char('-')) && text.mid(1).toDouble() == 0.0)
        text.remove(0, 1);
    return text;
}

/// Display x of the rotated y-axis title. It sits at @p defaultX unless the tick
/// labels, which end @p tickGap left of the axis and run @p labelChars wide at
/// @p charWidth each, reach it: a small field prints long labels
/// ("230.751234") that the title used to be drawn over (issue #502). Never
/// further left than @p minX.
inline double yAxisTitleX(double axisX, double defaultX, double minX, int labelChars,
                          double charWidth, double tickGap, double titleHalfWidth = 9.0)
{
    const double clear = axisX - tickGap - labelChars * charWidth - 6.0 - titleHalfWidth;
    return std::max(minX, std::min(defaultX, clear));
}

}   // namespace visivo

#endif   // WCS_AXIS_TICKS_H

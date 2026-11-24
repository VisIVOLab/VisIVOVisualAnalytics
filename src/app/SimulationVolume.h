#pragma once

// SimulationVolume.h
// The rules of a simulation volume in the cube viewer (issue #524), as pure
// logic: which plane is shown when the slice is taken along x, y or z, how a
// coordinate typed in the grid's units picks a plane, how a plane is labelled,
// and how a coordinate is written. The physical units a run is shown in are
// the shared mechanism of PhysicalUnits.h. No widgets, no VTK:
// tests/test_simulation_volume.cpp.
//
// A spectral cube steps through channels along its third axis only. A volume of
// a simulation (a PLUTO variable, a VBT field) has no spectrum: any of its
// three axes can be the one the slice is normal to, and every axis is a
// coordinate in the grid's units.

#include "PhysicalUnits.h"

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>

namespace visivo::simvol {

/// The cube axis the slice plane is normal to.
enum class SliceAxis { X = 0, Y = 1, Z = 2 };

inline QString axisLetter(SliceAxis axis)
{
    return axis == SliceAxis::X ? QStringLiteral("x")
            : (axis == SliceAxis::Y ? QStringLiteral("y") : QStringLiteral("z"));
}

inline SliceAxis axisFromIndex(int index)
{
    return index == 0 ? SliceAxis::X : (index == 1 ? SliceAxis::Y : SliceAxis::Z);
}

/// The axis a letter names ("x", "y", "z", any case); Z for anything else.
inline SliceAxis axisFromLetter(const QString &letter)
{
    const QString l = letter.trimmed().toLower();
    return l == QLatin1String("x") ? SliceAxis::X : (l == QLatin1String("y") ? SliceAxis::Y
                                                                              : SliceAxis::Z);
}

/// How many planes there are along @p axis of a @p width x @p height x @p depth cube.
inline int planeCount(SliceAxis axis, int width, int height, int depth)
{
    const int n = axis == SliceAxis::X ? width : (axis == SliceAxis::Y ? height : depth);
    return std::max(1, n);
}

/// The cube axes a plane normal to @p axis shows horizontally and vertically:
/// z planes are x by y, y planes x by z, x planes y by z (the order the
/// backend returns them in).
inline std::array<int, 2> planeAxes(SliceAxis axis)
{
    return axis == SliceAxis::Z ? std::array<int, 2>{ 0, 1 }
            : (axis == SliceAxis::Y ? std::array<int, 2>{ 0, 2 } : std::array<int, 2>{ 1, 2 });
}

/// World coordinate of 0-based plane @p index of an axis (@p crval at the
/// 1-based pixel @p crpix, @p cdelt per plane).
inline double axisWorld(double crval, double crpix, double cdelt, int index)
{
    return crval + (static_cast<double>(index) + 1.0 - crpix) * cdelt;
}

/// The 0-based plane of an @p n-plane axis nearest to @p coordinate. A
/// coordinate beyond the first or last plane by more than half a plane is
/// clamped to it and @p inside (when given) says false, so the caller can tell
/// the user the typed value is off the grid. -1 when the axis has no spacing.
inline int nearestPlane(double coordinate, double crval, double crpix, double cdelt, int n,
                        bool *inside = nullptr)
{
    if (inside)
        *inside = false;
    if (!std::isfinite(coordinate) || !std::isfinite(cdelt) || cdelt == 0.0 || n < 1)
        return -1;
    const double pixel = (coordinate - crval) / cdelt + crpix - 1.0;
    if (inside)
        *inside = pixel >= -0.5 && pixel <= n - 0.5;
    return std::clamp(static_cast<int>(std::lround(pixel)), 0, n - 1);
}

/// "x = 0.15 pc", or "x = 0.15" without a unit.
inline QString coordinateLabel(SliceAxis axis, double world, const QString &unit)
{
    const QString value = visivo::units::formatNumber(world);
    return unit.trimmed().isEmpty()
            ? QStringLiteral("%1 = %2").arg(axisLetter(axis), value)
            : QStringLiteral("%1 = %2 %3").arg(axisLetter(axis), value, unit.trimmed());
}

}   // namespace visivo::simvol

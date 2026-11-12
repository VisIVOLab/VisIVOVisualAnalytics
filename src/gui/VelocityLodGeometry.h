#ifndef VelocityLodGeometry_h
#define VelocityLodGeometry_h

// Where the velocity viewer puts a block-averaged (reduced) grid in its world
// frame. Widget-free and header-only so the unit tests can check it without VTK.
//
// The viewer's frame is point-centred and centred on 0, like the backend's
// (`_spacing_origin` in backend/app/velocity_field.py): an axis of N samples
// has a cell of box/(N-1) (1 without a box) and its first sample at
// -(N-1)/2 cells. The backend reduces a grid by an integer factor f per axis
// (`lod_factors`), and a block mean of f samples is the value at the block
// centre. So the reduced axis has a cell f times the full one and its first
// sample (f-1)/2 full cells in from the full grid's first sample. Glyphs,
// streamlines, basins and the density overlay all use this one placement,
// so a field drawn at full resolution and at a reduced level of detail line up.

#include <algorithm>
#include <array>
#include <cmath>

namespace velocitylod {

/// Full-resolution cell of an axis of `fullN` samples: box/(N-1) for a box in
/// Mpc, else unit cells (grid units).
inline double fullCell(int fullN, double boxMpc)
{
    return boxMpc > 0.0 ? boxMpc / std::max(1, fullN - 1) : 1.0;
}

/// First sample of a full-resolution axis, centred on 0.
inline double fullOrigin(int fullN, double cell)
{
    return -0.5 * cell * (std::max(1, fullN) - 1);
}

/// One axis of the backend's `lod_factors`: the overall factor, except on an
/// axis too short for it, which keeps at least two samples (n // 2, or 1).
inline int lodAxisFactor(int n, int overall)
{
    return std::max(1, std::min(overall, n / 2));
}

/// The backend's `lod_factors` for a grid of (nx, ny, nz) reduced so that its
/// largest axis is at most `maxDim`. Returned as (fx, fy, fz).
inline std::array<int, 3> lodFactors(const std::array<int, 3> &fullXyz, int maxDim)
{
    const int biggest = std::max({ fullXyz[0], fullXyz[1], fullXyz[2] });
    if (maxDim <= 0 || biggest <= maxDim)
        return { 1, 1, 1 };
    const int overall = (biggest + maxDim - 1) / maxDim;   // ceil, integer only
    return { lodAxisFactor(fullXyz[0], overall), lodAxisFactor(fullXyz[1], overall),
             lodAxisFactor(fullXyz[2], overall) };
}

/// The factor of one reduced axis when the backend did not send it. `expected`
/// is what the request should have produced (lodFactors, or an older backend's
/// single factor); it is kept when it gives the `n` samples that came back
/// (the backend crops each axis to a multiple of its factor). Otherwise the
/// best guess is fullN / n.
inline int inferAxisFactor(int fullN, int n, int expected)
{
    if (n <= 0 || fullN <= 0)
        return 1;
    if (expected >= 1 && fullN / expected == n)
        return expected;
    return std::max(1, fullN / n);
}

/// A reduced axis in the viewer's frame.
struct Axis
{
    double origin = 0.0;    // world position of the first reduced sample
    double spacing = 1.0;   // world distance between reduced samples
};

/// Place a reduced axis from its factor alone: the cell grows by f and the
/// first sample sits at the centre of the first block.
inline Axis reducedAxis(int factor, double fullCellSize, double fullOriginPos)
{
    const int f = std::max(1, factor);
    return { fullOriginPos + 0.5 * (f - 1) * fullCellSize, f * fullCellSize };
}

/// Place a reduced axis from the origin and spacing the backend reported for
/// it. Both frames are centred on 0 and differ only by the box scale, so the
/// reported origin, measured in reported cells, is kept as it is and only the
/// cell is rescaled to the viewer's. Falls back to reducedAxis() when the
/// report is unusable.
inline Axis reducedAxisFromReport(int factor, double reportedOrigin, double reportedSpacing,
                                  double fullCellSize, double fullOriginPos)
{
    if (!std::isfinite(reportedOrigin) || !std::isfinite(reportedSpacing)
        || reportedSpacing <= 0.0)
        return reducedAxis(factor, fullCellSize, fullOriginPos);
    const double spacing = std::max(1, factor) * fullCellSize;
    return { reportedOrigin / reportedSpacing * spacing, spacing };
}

/// Whether a backend product computed in a box of `boxBefore` Mpc must be
/// asked for again in a box of `boxAfter` (0 = no box, unit cells), because
/// the ratio between the axes' cells, cell_i = box/(N_i-1), changes.
///
/// Basins follow the flow direction in cells, v_i / cell_i, and do not depend
/// on the overall speed; ∇·v = Σ ∂v_i/∂x_i only scales by 1/k when every cell
/// scales by k (see cellScale). So both survive any change that scales all
/// three cells by one factor: every change on a cubic grid, and a change
/// between two boxes on any grid. Only a non-cubic grid going to or from
/// "no box" changes the cells' ratio.
inline bool cellRatioChanges(const std::array<int, 3> &fullXyz, double boxBefore,
                             double boxAfter)
{
    double before[3], after[3];
    for (int a = 0; a < 3; ++a) {
        before[a] = fullCell(fullXyz[a], boxBefore);
        after[a] = fullCell(fullXyz[a], boxAfter);
    }
    // Proportional cells: after_i / before_i is the same on every axis.
    const double r0 = after[0] / before[0];
    for (int a = 1; a < 3; ++a) {
        const double r = after[a] / before[a];
        if (std::abs(r - r0) > 1e-9 * std::max(std::abs(r), std::abs(r0)))
            return true;
    }
    return false;
}

/// The factor k every cell is scaled by going from `boxBefore` to `boxAfter`.
/// Meaningful only when cellRatioChanges() is false (then it is the same on
/// every axis): positions and spacings scale by k, ∇·v by 1/k.
inline double cellScale(const std::array<int, 3> &fullXyz, double boxBefore, double boxAfter)
{
    return fullCell(fullXyz[0], boxAfter) / fullCell(fullXyz[0], boxBefore);
}

} // namespace velocitylod

#endif

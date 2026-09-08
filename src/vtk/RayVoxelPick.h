#ifndef RayVoxelPick_h
#define RayVoxelPick_h

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

// ---------------------------------------------------------------------------
// RayVoxelPick — walk a view ray through a voxel grid and pick one voxel.
//
// The geometry behind the cube viewer's 3-D spectral picker, kept free of VTK
// so it can be tested without a render window. The caller supplies the ray (two
// world points), the grid, and a sampler.
//
// EVERY voxel the ray passes through is visited, exactly once, in order. That
// is a 3-D DDA (Amanatides & Woo), not a fixed-step march: stepping at a fixed
// rate either skips voxels on a long axis or wastes samples on a short one, and
// capping the step count — which a fixed march must, or a 10 000-channel cube
// costs a million samples per click — silently starts skipping. Measured on
// exactly that cube, a capped half-voxel march missed 1807 channels.
//
// The criterion mirrors the blend mode the user is looking through. All three
// are conventions over VOXEL values: the renderer samples with linear
// interpolation, so the extremum along the interpolated ray is not always the
// extremum of the voxels it crosses.
//
//   Maximum              MIP shows the largest sample along the ray; this
//                        returns the largest voxel on it — the same thing
//                        except in marginal cases.
//   Minimum              MinIP, the smallest, likewise.
//   FirstAboveThreshold  Composite has no single producing voxel at all: the
//                        pixel is a weighted sum. This is therefore a convention
//                        outright — the first voxel the opacity ramp makes
//                        visible, i.e. the front surface, rather than something
//                        brighter hidden behind it.
// ---------------------------------------------------------------------------
namespace RayVoxelPick {

enum class Criterion { FirstAboveThreshold, Maximum, Minimum };

struct Grid
{
    int extent[6]{ 0, -1, 0, -1, 0, -1 };   ///< xmin,xmax,ymin,ymax,zmin,zmax
    double origin[3]{ 0., 0., 0. };
    double spacing[3]{ 1., 1., 1. };

    bool valid() const
    {
        for (int a = 0; a < 3; ++a)
            if (extent[2 * a] > extent[2 * a + 1] || !(std::fabs(spacing[a]) > 0.))
                return false;
        return true;
    }
    /// VTK's convention: point i sits at origin + i*spacing, with i the ABSOLUTE
    /// extent index. No extent offset is added — doing so shifts every index by
    /// extent[2a] on a grid that does not start at zero.
    double world(int axis, double index) const
    {
        return origin[axis] + index * spacing[axis];
    }
    double index(int axis, double w) const { return (w - origin[axis]) / spacing[axis]; }
};

struct Result
{
    bool hit{ false };
    int voxel[3]{ 0, 0, 0 };
    double value{ 0. };
    double world[3]{ 0., 0., 0. };
};

/// Value at a voxel, or a non-finite value where there is none.
using Sampler = std::function<double(int, int, int)>;

/// True for a sample that must be ignored — a blank. Optional; when absent only
/// non-finite values are skipped. The cube viewer needs it because blanks have
/// already been rewritten to a finite sentinel below the data minimum by the
/// time the display image exists, so an isfinite() test alone would let MinIP
/// answer with a blank every time a ray crossed one.
using BlankTest = std::function<bool(double)>;

inline Result along(const Grid &grid, const double p0[3], const double p1[3],
                    Criterion criterion, double threshold, const Sampler &sample,
                    const BlankTest &isBlank = {})
{
    Result out;
    if (!grid.valid() || !sample)
        return out;

    // Work in continuous index space: cells are unit cubes centred on integers,
    // so the grid occupies [extent0 - 0.5, extent1 + 0.5] along each axis.
    double c0[3], c1[3], d[3];
    for (int a = 0; a < 3; ++a) {
        c0[a] = grid.index(a, p0[a]);
        c1[a] = grid.index(a, p1[a]);
        d[a] = c1[a] - c0[a];
    }

    double t0 = 0., t1 = 1.;
    for (int a = 0; a < 3; ++a) {
        const double lo = grid.extent[2 * a] - 0.5;
        const double hi = grid.extent[2 * a + 1] + 0.5;
        if (std::fabs(d[a]) < 1e-12) {
            if (c0[a] < lo || c0[a] > hi)
                return out;   // parallel to this slab and outside it
            continue;
        }
        double ta = (lo - c0[a]) / d[a], tb = (hi - c0[a]) / d[a];
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return out;       // misses the grid
    }

    // Amanatides & Woo setup, at the entry point.
    int cur[3], step[3];
    double tMax[3], tDelta[3];
    for (int a = 0; a < 3; ++a) {
        const double entry = c0[a] + t0 * d[a];
        // Direction-aware ownership. Entering exactly on the boundary between
        // two cells, the one the ray is heading INTO owns the interval; rounding
        // to nearest would pick the cell behind the ray and sample it over an
        // interval of zero length.
        const double owned = (d[a] >= 0.) ? std::floor(entry + 0.5) : std::ceil(entry - 0.5);
        cur[a] = std::clamp(static_cast<int>(owned), grid.extent[2 * a],
                            grid.extent[2 * a + 1]);
        if (std::fabs(d[a]) < 1e-12) {
            step[a] = 0;
            tMax[a] = std::numeric_limits<double>::infinity();
            tDelta[a] = std::numeric_limits<double>::infinity();
        } else {
            step[a] = (d[a] > 0.) ? 1 : -1;
            const double boundary = cur[a] + 0.5 * step[a];
            tMax[a] = (boundary - c0[a]) / d[a];
            tDelta[a] = std::fabs(1.0 / d[a]);
        }
    }

    const bool wantMax = (criterion == Criterion::Maximum);
    const bool wantMin = (criterion == Criterion::Minimum);
    double best = wantMin ? std::numeric_limits<double>::infinity()
                          : -std::numeric_limits<double>::infinity();

    // Bounded by the number of voxels the ray can cross, so a degenerate
    // direction cannot spin: at most the sum of the extents plus one.
    long long guard = 3;
    for (int a = 0; a < 3; ++a)
        guard += static_cast<long long>(grid.extent[2 * a + 1]) - grid.extent[2 * a] + 1;

    for (long long n = 0; n < guard; ++n) {
        const double value = sample(cur[0], cur[1], cur[2]);
        const bool skip = !std::isfinite(value) || (isBlank && isBlank(value));
        if (!skip) {
            if (wantMax || wantMin) {
                if ((wantMax && value > best) || (wantMin && value < best)) {
                    best = value;
                    out.hit = true;
                    out.value = value;
                    std::copy(cur, cur + 3, out.voxel);
                }
            } else if (value >= threshold) {
                out.hit = true;
                out.value = value;
                std::copy(cur, cur + 3, out.voxel);
                break;   // the front surface: do not march into what it hides
            }
        }

        // Advance across the nearest cell boundary — and across EVERY axis that
        // shares it. On a diagonal the ray crosses a corner: stepping one axis at
        // a time would then walk (0,0,0) -> (0,0,1) -> (0,1,1) -> (1,1,1), and the
        // two middle cells are touched at a point, over an interval of zero
        // length. Sampling them lets a voxel the ray never really enters win a
        // MIP, or stop a Composite walk short.
        const double next = std::min({ tMax[0], tMax[1], tMax[2] });
        if (!(next <= t1) || !std::isfinite(next))
            break;                       // past the exit point
        bool moved = false;
        bool left = false;
        for (int a = 0; a < 3; ++a) {
            if (step[a] == 0 || tMax[a] > next + 1e-12)
                continue;
            cur[a] += step[a];
            tMax[a] += tDelta[a];
            moved = true;
            if (cur[a] < grid.extent[2 * a] || cur[a] > grid.extent[2 * a + 1])
                left = true;
        }
        if (!moved || left)
            break;
    }

    if (out.hit)
        for (int a = 0; a < 3; ++a)
            out.world[a] = grid.world(a, out.voxel[a]);
    return out;
}

}  // namespace RayVoxelPick

#endif   // RayVoxelPick_h

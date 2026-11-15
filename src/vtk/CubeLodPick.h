#ifndef CubeLodPick_h
#define CubeLodPick_h

#include "RayVoxelPick.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

// ---------------------------------------------------------------------------
// CubeLodPick — the 3-D pick of a refined view (issue #321): the view ray
// walked through the volumes actually drawn, the fine blocks inside the
// refined box and the coarse pieces of the preview elsewhere, each on its own
// grid (RayVoxelPick::along), so a click inside the box lands on the finer
// voxel the user sees there.
//
// The pieces own disjoint world boxes (CubeLodAssembly), so the ray is cut at
// their faces and each part walked on the grid of the piece that owns it, in
// the order the ray meets them. The criterion is RayVoxelPick's: the first
// voxel at or above the threshold (Composite), or the largest (MIP) or
// smallest (MinIP) over every piece.
// ---------------------------------------------------------------------------
namespace CubeLodPick {

struct Piece
{
    RayVoxelPick::Grid grid;          ///< its samples, ghosts included, in the world
    std::array<double, 6> owned{};    ///< the world box it draws (x0, x1, y0, y1, z0, z1)
    RayVoxelPick::Sampler sample;     ///< value at an index of its grid
};

struct Result
{
    bool hit{ false };
    int piece{ -1 };                  ///< index into the pieces walked
    RayVoxelPick::Result voxel;       ///< the hit on that piece's grid; world is the voxel's centre
};

/// The part [t0, t1] of the segment p0 → p1 inside @p box; false when none.
inline bool clipToBox(const double p0[3], const double p1[3], const std::array<double, 6> &box,
                      double &t0, double &t1)
{
    t0 = 0.0;
    t1 = 1.0;
    for (int a = 0; a < 3; ++a) {
        const double lo = box[2 * a], hi = box[2 * a + 1];
        if (!(lo <= hi))
            return false;
        const double d = p1[a] - p0[a];
        if (std::fabs(d) < 1e-15) {
            if (p0[a] < lo || p0[a] > hi)
                return false;
            continue;
        }
        double ta = (lo - p0[a]) / d, tb = (hi - p0[a]) / d;
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return false;
    }
    return true;
}

inline Result along(const std::vector<Piece> &pieces, const double p0[3], const double p1[3],
                    RayVoxelPick::Criterion criterion, double threshold,
                    const RayVoxelPick::BlankTest &isBlank = {})
{
    // The pieces the ray crosses, in the order it meets them.
    struct Crossing
    {
        int piece;
        double t0, t1;
    };
    std::vector<Crossing> crossings;
    for (int i = 0; i < int(pieces.size()); ++i) {
        double t0 = 0.0, t1 = 0.0;
        if (clipToBox(p0, p1, pieces[size_t(i)].owned, t0, t1) && t1 > t0)
            crossings.push_back({ i, t0, t1 });
    }
    std::sort(crossings.begin(), crossings.end(),
              [](const Crossing &a, const Crossing &b) { return a.t0 < b.t0; });

    using C = RayVoxelPick::Criterion;
    Result out;
    for (const Crossing &c : crossings) {
        const Piece &p = pieces[size_t(c.piece)];
        // A hair inside the piece's faces: a face on a cell boundary would
        // otherwise let the walk step into the cell beyond it, a ghost
        // sample under the neighbour that owns that region.
        const double eps = 1e-9 * (c.t1 - c.t0);
        double q0[3], q1[3];
        for (int a = 0; a < 3; ++a) {
            q0[a] = p0[a] + (c.t0 + eps) * (p1[a] - p0[a]);
            q1[a] = p0[a] + (c.t1 - eps) * (p1[a] - p0[a]);
        }
        const RayVoxelPick::Result r =
                RayVoxelPick::along(p.grid, q0, q1, criterion, threshold, p.sample, isBlank);
        if (!r.hit)
            continue;
        const bool better = !out.hit
                || (criterion == C::Maximum && r.value > out.voxel.value)
                || (criterion == C::Minimum && r.value < out.voxel.value);
        if (better) {
            out.hit = true;
            out.piece = c.piece;
            out.voxel = r;
        }
        if (criterion == C::FirstAboveThreshold)
            break;   // the front surface: what lies behind it is hidden
    }
    return out;
}

}   // namespace CubeLodPick

#endif   // CubeLodPick_h

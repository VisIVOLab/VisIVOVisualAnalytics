#ifndef RegionCubeSampling_h
#define RegionCubeSampling_h

// RegionCubeSampling.h
// Which samples of the cube shown in the 3-D view lie in a region of the list
// (issues #341, #343).
//
// The list is in the cube's full-resolution pixels. The 3-D view shows the
// full cube, a box of it, or a preview that samples it (strided, block means,
// a pyramid level). Along an axis its sample k sits at world
// origin + (k - extent0) * spacing, and world w is cube voxel
// CubeLodGeometry::toCube(w, sampling). A region spectrum averages the samples
// whose cube position is in the region; a region smaller than the preview's
// spacing takes the sample nearest its centre rather than none.
//
// Pure, header-only, unit-tested in tests/test_region_model.cpp.

#include "RegionModel.h"
#include "CubeLodGeometry.h"

#include <array>
#include <cmath>
#include <vector>

namespace visivo::regions {

/// One axis of the displayed cube: its extent and where its samples sit.
struct AxisGrid
{
    int extent0{ 0 };
    int extent1{ -1 };
    double origin{ 0.0 };
    double spacing{ 1.0 };
};

/// Cube voxel of displayed sample @p k.
inline double sampleToCube(const AxisGrid &a, const CubeLodGeometry::Sampling &s, int k)
{
    return CubeLodGeometry::toCube(a.origin + (k - a.extent0) * a.spacing, s);
}

/// Displayed sample (fractional) at cube voxel @p cube.
inline double cubeToSample(const AxisGrid &a, const CubeLodGeometry::Sampling &s, double cube)
{
    return a.extent0 + (CubeLodGeometry::toWorld(cube, s) - a.origin) / a.spacing;
}

/// The displayed samples (kx, ky) whose cube positions are in @p r.
inline std::vector<std::array<int, 2>> samplesInRegion(const Region &r, const AxisGrid &x,
                                                       const AxisGrid &y,
                                                       const CubeLodGeometry::Sampling &s)
{
    std::vector<std::array<int, 2>> inside;
    if (x.extent1 < x.extent0 || y.extent1 < y.extent0 || !(x.spacing > 0.0) || !(y.spacing > 0.0))
        return inside;
    const auto b = bounds(r);
    const int kx0 = std::max(x.extent0, int(std::floor(cubeToSample(x, s, b[0]))));
    const int kx1 = std::min(x.extent1, int(std::ceil(cubeToSample(x, s, b[1]))));
    const int ky0 = std::max(y.extent0, int(std::floor(cubeToSample(y, s, b[2]))));
    const int ky1 = std::min(y.extent1, int(std::ceil(cubeToSample(y, s, b[3]))));
    for (int ky = ky0; ky <= ky1; ++ky)
        for (int kx = kx0; kx <= kx1; ++kx)
            if (contains(r, sampleToCube(x, s, kx), sampleToCube(y, s, ky)))
                inside.push_back({ kx, ky });
    if (inside.empty()) {
        const int kx = int(std::lround(cubeToSample(x, s, (b[0] + b[1]) / 2.0)));
        const int ky = int(std::lround(cubeToSample(y, s, (b[2] + b[3]) / 2.0)));
        if (kx >= x.extent0 && kx <= x.extent1 && ky >= y.extent0 && ky <= y.extent1)
            inside.push_back({ kx, ky });
    }
    return inside;
}

}   // namespace visivo::regions

#endif   // RegionCubeSampling_h

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

// ── The 3-D view's cutting plane (issue #343) ────────────────────────────────
// The plane at the current channel wears the full-resolution slice as its
// texture. Its texel i is centred at u = (i + 0.5) / width, so for that centre
// to sit where the 3-D view puts cube pixel i, toWorld(i), the plane runs from
// toWorld(-0.5) to toWorld(width - 0.5); its height lies at toWorld(channel).
// The regions on it are placed with the same function, so plane and regions
// agree at every sampling (full resolution, strided, block means, a box read
// at full resolution with its origin at its corner).

struct PlaneRect
{
    double x0{ 0.0 }, x1{ 0.0 }, y0{ 0.0 }, y1{ 0.0 }, z{ 0.0 };
};

inline PlaneRect cuttingPlane(int width, int height, int channel, const CubeLodGeometry::Sampling &s)
{
    return { CubeLodGeometry::toWorld(-0.5, s), CubeLodGeometry::toWorld(width - 0.5, s),
             CubeLodGeometry::toWorld(-0.5, s), CubeLodGeometry::toWorld(height - 0.5, s),
             CubeLodGeometry::toWorld(double(channel), s) };
}

/// The 3-D view's world position of cube pixel @p p on channel @p channel.
inline std::array<double, 3> planePoint(const Vec2 &p, int channel, const CubeLodGeometry::Sampling &s)
{
    return { CubeLodGeometry::toWorld(p[0], s), CubeLodGeometry::toWorld(p[1], s),
             CubeLodGeometry::toWorld(double(channel), s) };
}

/// The cube pixel (fractional) under world @p w on the plane: the inverse of
/// planePoint along one axis. Its texel is the nearest integer.
inline double planeToCube(double w, const CubeLodGeometry::Sampling &s)
{
    return CubeLodGeometry::toCube(w, s);
}

}   // namespace visivo::regions

#endif   // RegionCubeSampling_h

#ifndef RegionPanePlacement_h
#define RegionPanePlacement_h

// RegionPanePlacement.h
// Where the region list sits on each 2-D view of the cube viewer (issue #355).
//
// The list is in the cube's full-resolution 0-based pixels. Every 2-D view
// draws an image whose voxel v this viewer places at world
// origin + (v - extent0) * spacing (the convention of its overlays and of
// updateRegionFromDisplayPosition; the images all start at extent 0), and
// whose voxel stands for some cube pixel. Per axis that is a
// CubeLodGeometry::Sampling: the map's pixel k (= v - extent0) is cube pixel
// k * scale + offset.
//
//   * The 2-D slice: its world is the cube's pixels (origin 0, spacing 1 today;
//     a reduced slice would carry its spacing as the block it stands for), so
//     the sampling is {spacing, origin} and a region needs no transform.
//   * A map in a Product pane (a moment, a line width, a Faraday-depth map):
//     the backend computes it on the full-resolution sky plane, so one map
//     pixel is one cube pixel, shifted by the crop origin when the map covers
//     a box of the field (RM synthesis over a region).
//
// A map that is not on the cube's sky grid (a kinematic fit over a selection's
// box, a cosmology product) has no placement: regions are not drawn there.
//
// Pure, header-only, unit-tested in tests/test_region_model.cpp.

#include "CubeLodGeometry.h"
#include "RegionFullRes.h"
#include "RegionModel.h"

#include <array>
#include <optional>

namespace visivo::regions {

/// One axis of a 2-D view: where its voxels sit in world, and which cube
/// pixel each of them stands for.
struct MapAxis
{
    double origin{ 0.0 };
    double spacing{ 1.0 };
    int extent0{ 0 };
    CubeLodGeometry::Sampling sampling;

    /// World position of cube pixel @p cube.
    double toWorld(double cube) const { return origin + sampling.index(cube) * spacing; }
    /// Cube pixel (fractional) under world @p world.
    double toCube(double world) const { return sampling.centre((world - origin) / spacing); }
};

struct MapPlacement
{
    MapAxis x;
    MapAxis y;

    std::array<double, 3> toWorld(const Vec2 &p) const
    {
        return { x.toWorld(p[0]), y.toWorld(p[1]), 0.0 };
    }
    Vec2 toCube(double wx, double wy) const { return { x.toCube(wx), y.toCube(wy) }; }
    /// The displayed voxel -> cube pixel rule the drawing tools measure with
    /// (RegionFullRes.h): voxel v is cube pixel offset + (v - extent0) * scale.
    DisplayGrid displayGrid() const
    {
        return { x.sampling.offset, y.sampling.offset, x.sampling.scale, y.sampling.scale,
                 x.extent0, y.extent0 };
    }
    /// Cube pixels per world unit along x: turns a distance read on screen in
    /// world units (a pick tolerance, the knob's stand-off) into list pixels.
    double cubePerWorld() const { return x.sampling.scale / x.spacing; }
};

/// A view whose world is the cube's pixels (the 2-D slice).
inline MapPlacement worldIsCube(const std::array<double, 2> &origin,
                                const std::array<double, 2> &spacing,
                                const std::array<int, 2> &extent0)
{
    MapPlacement p;
    p.x = { origin[0], spacing[0], extent0[0], { spacing[0], origin[0] } };
    p.y = { origin[1], spacing[1], extent0[1], { spacing[1], origin[1] } };
    return p;
}

/// A map on the cube's sky plane, one map pixel per cube pixel: the whole
/// field (@p crop empty, the map as large as the cube) or a box of it whose
/// first pixel is cube pixel @p crop. Empty when the map is not on that grid.
inline std::optional<MapPlacement> skyMapPlacement(const std::array<int, 2> &mapSize,
                                                   const std::array<double, 2> &origin,
                                                   const std::array<double, 2> &spacing,
                                                   const std::array<int, 2> &extent0,
                                                   const std::array<int, 2> &cubeSize,
                                                   const std::optional<std::array<int, 2>> &crop)
{
    if (mapSize[0] <= 0 || mapSize[1] <= 0 || cubeSize[0] <= 0 || cubeSize[1] <= 0
        || !(spacing[0] > 0.0) || !(spacing[1] > 0.0))
        return std::nullopt;
    std::array<int, 2> at{ 0, 0 };
    if (crop) {
        at = *crop;
        if (at[0] < 0 || at[1] < 0 || at[0] + mapSize[0] > cubeSize[0]
            || at[1] + mapSize[1] > cubeSize[1])
            return std::nullopt;
    } else if (mapSize != cubeSize) {
        return std::nullopt;
    }
    MapPlacement p;
    p.x = { origin[0], spacing[0], extent0[0], { 1.0, double(at[0]) } };
    p.y = { origin[1], spacing[1], extent0[1], { 1.0, double(at[1]) } };
    return p;
}

}   // namespace visivo::regions

#endif   // RegionPanePlacement_h

#ifndef CubeLodGeometry_h
#define CubeLodGeometry_h

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// ---------------------------------------------------------------------------
// CubeLodGeometry — the coordinates behind the 3-D view's level of detail
// (issue #321), free of VTK so they can be tested without a render window.
//
// A large cube is shown as a coarse preview of the whole cube plus, where the
// view needs it, a box of blocks read from a finer level of the brick
// pyramid; the rest of the cube is drawn as at most six pieces cut from the
// preview around that box. Every one of those volumes is cropped to the region
// it owns, so they meet on shared planes without gaps or overlaps, and they are
// drawn back to front. This header holds the arithmetic all of that shares:
//
//   - the shapes of the pyramid's levels (backend/app/brick_pyramid.py
//     level_shapes);
//   - where the samples of the preview and of every level sit, in cube voxels;
//   - the preview's world frame, which overlays, regions, the lasso and the WCS
//     already use and which therefore must not move;
//   - the node grid of a level, and the box of nodes covering a box of the cube;
//   - ghost samples, clamped at the cube's border;
//   - the cropping planes to give vtkGPUVolumeRayCastMapper, ported from the
//     backend's out-of-core renderer (render_session/_ooc.py _vtk_crop_planes);
//   - the kd-split of the cube around the fine box, and the back-to-front order
//     of its pieces and of the fine box's nodes (_ooc.py _sort).
//
// Conventions. Everything is (x, y, z), unlike the backend's (z, y, x) arrays.
// "Cube voxels" are full-resolution voxel coordinates: voxel i has its centre
// at i and spans [i - 0.5, i + 0.5]. Boxes of voxels are half-open, [lo, hi).
// ---------------------------------------------------------------------------
namespace CubeLodGeometry {

using Dims = std::array<int, 3>;   ///< x, y, z

/// A half-open box of integer coordinates, [lo, hi) on every axis.
struct Box
{
    Dims lo{ 0, 0, 0 };
    Dims hi{ 0, 0, 0 };

    bool empty() const
    {
        return !(lo[0] < hi[0] && lo[1] < hi[1] && lo[2] < hi[2]);
    }
    std::int64_t count() const
    {
        if (empty())
            return 0;
        return std::int64_t(hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    }
    bool operator==(const Box &o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const Box &o) const { return !(*this == o); }
};

/// The whole of a grid of @p shape.
inline Box wholeBox(const Dims &shape)
{
    return Box{ { 0, 0, 0 }, shape };
}

/// @p a within @p b (empty when they do not overlap).
inline Box intersect(const Box &a, const Box &b)
{
    Box out;
    for (int i = 0; i < 3; ++i) {
        out.lo[i] = std::max(a.lo[i], b.lo[i]);
        out.hi[i] = std::max(out.lo[i], std::min(a.hi[i], b.hi[i]));
    }
    return out;
}

// ── Pyramid levels ──────────────────────────────────────────────────────────

/// Cube voxels per sample of level @p level along an axis (levels past 62
/// would overflow; no cube has them).
inline std::int64_t levelScale(int level)
{
    return std::int64_t(1) << std::clamp(level, 0, 62);
}

/// The shape of level @p level: the cube's halved, rounded up, @p level times.
inline Dims levelShape(const Dims &shape, int level)
{
    Dims out = shape;
    for (int l = 0; l < level; ++l)
        for (int &n : out)
            n = (n + 1) / 2;
    return out;
}

/// The shape of every level, level 0 first: halved (rounded up) until the level
/// fits in one brick. The same rule as the backend's pyramid
/// (brick_pyramid.level_shapes), so the levels the client asks for exist.
inline std::vector<Dims> levelShapes(const Dims &shape, int brick)
{
    std::vector<Dims> shapes{ shape };
    if (brick < 1)
        return shapes;
    while (std::max({ shapes.back()[0], shapes.back()[1], shapes.back()[2] }) > brick)
        shapes.push_back(levelShape(shapes.back(), 1));
    return shapes;
}

// ── Where samples sit ───────────────────────────────────────────────────────

/// How a grid of samples lies on the cube: sample k has its centre at cube
/// voxel k * scale + offset.
///
/// - A strided preview (every s-th voxel) and a strided level: offset 0.
/// - A block-mean preview (#320) or a pyramid level (each sample the mean of a
///   scale^3 block of cube voxels): offset (scale - 1) / 2, the block's centre.
struct Sampling
{
    double scale{ 1.0 };
    double offset{ 0.0 };

    double centre(double k) const { return k * scale + offset; }
    double index(double cube) const { return (cube - offset) / scale; }

    static Sampling strided(int stride)
    {
        return { double(std::max(1, stride)), 0.0 };
    }
    static Sampling blockMean(int stride)
    {
        const double s = std::max(1, stride);
        return { s, (s - 1.0) / 2.0 };
    }
    /// Level @p level of the brick pyramid (block means).
    static Sampling pyramidLevel(int level)
    {
        const double s = double(levelScale(level));
        return { s, (s - 1.0) / 2.0 };
    }
    /// Level @p level read with a stride from the source (no pyramid).
    static Sampling stridedLevel(int level)
    {
        return { double(levelScale(level)), 0.0 };
    }
};

/// The 3-D view's world coordinate of cube voxel @p cube along an axis. The
/// world frame is the preview's (sample k at world k, spacing 1), so a block of
/// any level placed with this function lands where the preview shows the same
/// voxels, and everything already drawn in that frame stays put.
inline double toWorld(double cube, const Sampling &preview)
{
    return preview.index(cube);
}

/// The cube voxel at world @p world along an axis (the inverse of toWorld).
inline double toCube(double world, const Sampling &preview)
{
    return preview.centre(world);
}

/// Origin and spacing of a block in the world frame.
struct Placement
{
    std::array<double, 3> origin{ 0.0, 0.0, 0.0 };
    double spacing{ 1.0 };
};

/// Where to put a block of samples of @p level whose sample 0 (ghost included)
/// is sample @p sampleOrigin of that level, in the world frame of @p preview.
inline Placement placeBlock(const Dims &sampleOrigin, const Sampling &level, const Sampling &preview)
{
    Placement p;
    for (int i = 0; i < 3; ++i)
        p.origin[i] = toWorld(level.centre(sampleOrigin[i]), preview);
    p.spacing = level.scale / preview.scale;
    return p;
}

// ── Nodes ───────────────────────────────────────────────────────────────────

/// A block of the fine set: @p level and its index along x, y, z at that
/// level, in nodes (backend/app/brick_lod.py Node).
struct Node
{
    int level{ 0 };
    int ix{ 0 };
    int iy{ 0 };
    int iz{ 0 };

    bool operator==(const Node &o) const
    {
        return level == o.level && ix == o.ix && iy == o.iy && iz == o.iz;
    }
};

/// Nodes of @p nodeSize level voxels per axis covering level @p level of a
/// cube of @p cubeShape (the last one along an axis may be shorter).
inline Dims nodeGrid(const Dims &cubeShape, int level, int nodeSize)
{
    const Dims shape = levelShape(cubeShape, level);
    Dims out;
    for (int i = 0; i < 3; ++i)
        out[i] = nodeSize > 0 ? (shape[i] + nodeSize - 1) / nodeSize : 0;
    return out;
}

/// The node's samples in its level, clipped to the level.
inline Box nodeLevelBox(const Dims &cubeShape, int nodeSize, const Node &node)
{
    const Dims shape = levelShape(cubeShape, node.level);
    const Dims index{ node.ix, node.iy, node.iz };
    Box b;
    for (int i = 0; i < 3; ++i) {
        b.lo[i] = std::min(shape[i], index[i] * nodeSize);
        b.hi[i] = std::min(shape[i], (index[i] + 1) * nodeSize);
    }
    return b;
}

/// The samples of level @p level that cover the cube voxels @p cubeBox: the
/// box divided by the level's scale, rounded outward, within the level.
inline Box levelBoxCovering(const Box &cubeBox, const Dims &cubeShape, int level)
{
    const Box box = intersect(cubeBox, wholeBox(cubeShape));
    if (box.empty())
        return {};
    const std::int64_t scale = levelScale(level);
    const Dims shape = levelShape(cubeShape, level);
    Box out;
    for (int i = 0; i < 3; ++i) {
        out.lo[i] = int(box.lo[i] / scale);
        out.hi[i] = int(std::min<std::int64_t>(shape[i], (box.hi[i] + scale - 1) / scale));
    }
    return out;
}

/// The cube voxels the samples @p levelBox of level @p level stand for,
/// clipped to the cube.
inline Box cubeBoxOfLevelBox(const Box &levelBox, const Dims &cubeShape, int level)
{
    if (levelBox.empty())
        return {};
    const std::int64_t scale = levelScale(level);
    Box out;
    for (int i = 0; i < 3; ++i) {
        out.lo[i] = int(std::min<std::int64_t>(cubeShape[i], levelBox.lo[i] * scale));
        out.hi[i] = int(std::min<std::int64_t>(cubeShape[i], levelBox.hi[i] * scale));
    }
    return out;
}

/// The cube voxels the node owns, clipped to the cube (brick_lod.node_box).
inline Box nodeCubeBox(const Dims &cubeShape, int nodeSize, const Node &node)
{
    const std::int64_t side = std::int64_t(nodeSize) * levelScale(node.level);
    const Dims index{ node.ix, node.iy, node.iz };
    Box b;
    for (int i = 0; i < 3; ++i) {
        b.lo[i] = int(std::min<std::int64_t>(cubeShape[i], index[i] * side));
        b.hi[i] = int(std::min<std::int64_t>(cubeShape[i], (index[i] + 1) * side));
    }
    return b;
}

/// The range of node indices (a box in node units) of level @p level whose
/// nodes cover @p cubeBox: the box snapped outward to the node grid.
inline Box nodeRangeCovering(const Box &cubeBox, const Dims &cubeShape, int level, int nodeSize)
{
    const Box box = intersect(cubeBox, wholeBox(cubeShape));
    if (box.empty() || nodeSize < 1)
        return {};
    const std::int64_t side = std::int64_t(nodeSize) * levelScale(level);
    const Dims grid = nodeGrid(cubeShape, level, nodeSize);
    Box r;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = int(box.lo[i] / side);
        r.hi[i] = int(std::min<std::int64_t>(grid[i], (box.hi[i] + side - 1) / side));
    }
    return r;
}

/// The nodes of @p range (node indices) at @p level, x fastest.
inline std::vector<Node> nodesIn(const Box &range, int level)
{
    std::vector<Node> out;
    if (range.empty())
        return out;
    out.reserve(size_t(range.count()));
    for (int z = range.lo[2]; z < range.hi[2]; ++z)
        for (int y = range.lo[1]; y < range.hi[1]; ++y)
            for (int x = range.lo[0]; x < range.hi[0]; ++x)
                out.push_back(Node{ level, x, y, z });
    return out;
}

/// The cube voxels covered by the nodes of @p range, clipped to the cube.
inline Box cubeBoxOfRange(const Box &range, const Dims &cubeShape, int level, int nodeSize)
{
    if (range.empty())
        return {};
    const Box lo = nodeCubeBox(cubeShape, nodeSize, Node{ level, range.lo[0], range.lo[1], range.lo[2] });
    const Box hi = nodeCubeBox(cubeShape, nodeSize,
                               Node{ level, range.hi[0] - 1, range.hi[1] - 1, range.hi[2] - 1 });
    return Box{ lo.lo, hi.hi };
}

// ── Ghost samples ───────────────────────────────────────────────────────────

/// A block of samples with @p ghost extra samples on every side, as
/// render_session/_ooc.py _load reads one: the block is always
/// dims = box + 2 * ghost long; the part inside the grid is read
/// ([readLo, readHi) of the grid), and where the grid ends the edge sample is
/// repeated.
struct Ghosted
{
    Dims sampleLo{ 0, 0, 0 };   ///< the grid index of the block's sample 0
    Dims dims{ 0, 0, 0 };
    Box read;                   ///< the grid samples actually read
};

inline Ghosted ghosted(const Box &box, const Dims &gridShape, int ghost)
{
    Ghosted g;
    for (int i = 0; i < 3; ++i) {
        g.sampleLo[i] = box.lo[i] - ghost;
        g.dims[i] = box.hi[i] - box.lo[i] + 2 * ghost;
        g.read.lo[i] = std::clamp(box.lo[i] - ghost, 0, gridShape[i]);
        g.read.hi[i] = std::clamp(box.hi[i] + ghost, g.read.lo[i], gridShape[i]);
    }
    return g;
}

/// The samples of @p sampling needed to draw the cube voxels @p cubeBox owns
/// ([lo - 0.5, hi - 0.5] on each axis): the first sample at or before the low
/// face to the first at or after the high face. For a box aligned to a level's
/// blocks it is the level's own samples plus one ghost sample per side; it may
/// reach outside the grid at the cube's border (see ghosted for the edges).
inline Box bracketingSamples(const Box &cubeBox, const Sampling &sampling)
{
    Box out;
    for (int i = 0; i < 3; ++i) {
        out.lo[i] = int(std::floor(sampling.index(cubeBox.lo[i] - 0.5)));
        out.hi[i] = int(std::ceil(sampling.index(cubeBox.hi[i] - 0.5))) + 1;
    }
    return out;
}

// ── Cropping planes ─────────────────────────────────────────────────────────

/// Crop boxes: x0, x1, y0, y1, z0, z1.
using Crop = std::array<double, 6>;

/// The region of the cube @p cubeBox owns, in cube voxels: voxel i spans
/// [i - 0.5, i + 0.5], clipped to the cube's sample range [0, n - 1] as the
/// single-volume path draws it (_ooc.py _load).
inline Crop ownedCrop(const Box &cubeBox, const Dims &cubeShape)
{
    Crop c;
    for (int i = 0; i < 3; ++i) {
        c[2 * i] = std::max(cubeBox.lo[i] - 0.5, 0.0);
        c[2 * i + 1] = std::min(cubeBox.hi[i] - 0.5, double(cubeShape[i] - 1));
    }
    return c;
}

/// @p cube (cube voxels) in the world frame of @p preview.
inline Crop cropToWorld(const Crop &cube, const Sampling &preview)
{
    Crop w;
    for (int i = 0; i < 6; ++i)
        w[i] = toWorld(cube[i], preview);
    return w;
}

/// The cropping plane to give vtkGPUVolumeRayCastMapper so that it DRAWS a
/// face at world @p drawn, on an axis of @p n samples from @p origin with
/// @p spacing. The mapper reads a plane p as a fraction of the samples'
/// extent, (p - o) / ((n - 1) s), but draws it on the cells' extent, one
/// sample longer and half a sample earlier: d = o - s/2 + (p - o) n / (n - 1)
/// (measured on VTK 9.3.1 and 9.6.2).
inline double cropPlaneFor(double drawn, double origin, double spacing, int n)
{
    return n > 1 ? origin + (drawn - origin + spacing / 2.0) * (n - 1) / n : origin;
}

/// How far inside its high face a block's crop is pulled (world units, as the
/// backend's _FACE_EPS): blocks own half-open boxes.
inline constexpr double kFaceEps = 1e-3;

/// The cropping planes that make vtkGPUVolumeRayCastMapper draw exactly
/// @p crop (world units) of a block of @p dims samples at @p origin, @p spacing.
/// A port of render_session/_ooc.py _vtk_crop_planes, rounding included.
///
/// The mapper draws both faces of its crop, so a ray lying exactly on a face
/// two blocks share would be drawn twice: the high plane is pulled
/// @p faceEps inside. VTK keeps the planes as float32, so each is rounded
/// there: the low one up, the high one strictly down (at large coordinates
/// faceEps is below float32's resolution, and only the strict rounding keeps
/// the box half-open).
inline Crop vtkCropPlanes(const Crop &crop, const std::array<double, 3> &origin, double spacing,
                          const Dims &dims, double faceEps = kFaceEps)
{
    Crop planes;
    for (int axis = 0; axis < 3; ++axis) {
        const double o = origin[axis];
        const int n = dims[axis];
        const double lo = cropPlaneFor(crop[2 * axis], o, spacing, n);
        const double hi = cropPlaneFor(crop[2 * axis + 1] - faceEps, o, spacing, n);
        float lo32 = static_cast<float>(lo);
        if (double(lo32) < lo)
            lo32 = std::nextafter(lo32, std::numeric_limits<float>::infinity());
        float hi32 = static_cast<float>(hi);
        if (double(hi32) >= hi)
            hi32 = std::nextafter(hi32, -std::numeric_limits<float>::infinity());
        planes[2 * axis] = double(lo32);
        planes[2 * axis + 1] = double(hi32);
    }
    return planes;
}

// ── The kd-split around the fine box ────────────────────────────────────────

/// The pieces of the cube around a box. The split is a kd-tree: x below the
/// box, then x above it; within the box's x range, y below and above; within
/// its x and y ranges, z below and above. What is left is the box itself.
enum class Side { XLow, XHigh, YLow, YHigh, ZLow, ZHigh, Inner };

struct Piece
{
    Side side{ Side::Inner };
    Box box;
};

/// The non-empty pieces of a cube of @p cubeShape around @p inner (clipped to
/// the cube), in the order of Side, Inner excluded. With @p inner they cover
/// the cube exactly once. An empty @p inner gives no pieces: there is nothing
/// to split around, the whole preview is drawn as it is.
inline std::vector<Piece> kdSplit(const Dims &cubeShape, const Box &innerBox)
{
    std::vector<Piece> out;
    const Box cube = wholeBox(cubeShape);
    const Box inner = intersect(innerBox, cube);
    if (inner.empty())
        return out;
    auto add = [&out](Side side, const Box &b) {
        if (!b.empty())
            out.push_back(Piece{ side, b });
    };
    const Dims &n = cubeShape;
    const Dims &lo = inner.lo;
    const Dims &hi = inner.hi;
    add(Side::XLow, Box{ { 0, 0, 0 }, { lo[0], n[1], n[2] } });
    add(Side::XHigh, Box{ { hi[0], 0, 0 }, { n[0], n[1], n[2] } });
    add(Side::YLow, Box{ { lo[0], 0, 0 }, { hi[0], lo[1], n[2] } });
    add(Side::YHigh, Box{ { lo[0], hi[1], 0 }, { hi[0], n[1], n[2] } });
    add(Side::ZLow, Box{ { lo[0], lo[1], 0 }, { hi[0], hi[1], lo[2] } });
    add(Side::ZHigh, Box{ { lo[0], lo[1], hi[2] }, { hi[0], hi[1], n[2] } });
    return out;
}

/// The back-to-front drawing order of the kd-split around @p inner for an eye
/// at @p eye (cube voxels): the sides of @p pieces (as kdSplit gives them) and
/// Side::Inner, the fine box. At every split the side away from the eye comes
/// first; the pieces are convex and separated by the split planes, so this is
/// an exact visibility order whatever their sizes. The planes are the faces
/// between owned regions, half a voxel below each box's first voxel.
inline std::vector<Side> kdBackToFront(const Box &inner, const std::array<double, 3> &eye,
                                       const std::vector<Piece> &pieces)
{
    auto present = [&pieces](Side s) {
        return std::any_of(pieces.begin(), pieces.end(), [s](const Piece &p) { return p.side == s; });
    };
    // Built from the innermost split outward: each split puts the piece below
    // its plane and everything above it (or the reverse) in front of the other.
    std::vector<Side> order{ Side::Inner };
    static constexpr Side kLow[3] = { Side::XLow, Side::YLow, Side::ZLow };
    static constexpr Side kHigh[3] = { Side::XHigh, Side::YHigh, Side::ZHigh };
    for (int axis = 2; axis >= 0; --axis) {
        const double loPlane = inner.lo[axis] - 0.5;
        const double hiPlane = inner.hi[axis] - 0.5;
        // The split at the high face: what is below it (order) | the high piece.
        if (present(kHigh[axis])) {
            if (eye[axis] < hiPlane)
                order.insert(order.begin(), kHigh[axis]);   // eye below: high piece is far
            else
                order.push_back(kHigh[axis]);
        }
        // The split at the low face: the low piece | what is above it.
        if (present(kLow[axis])) {
            if (eye[axis] < loPlane)
                order.push_back(kLow[axis]);                // eye below: low piece is near
            else
                order.insert(order.begin(), kLow[axis]);
        }
    }
    return order;
}

/// @p nodes in back-to-front order for an eye at @p eye (cube voxels), by an
/// octree traversal: from level @p topLevel down to each node's level, the
/// cells farthest from the eye's cell come first, along x, then y, then z.
/// For boxes of a hierarchical partition this is a correct visibility order
/// (the distance between centres is not, for boxes of different sizes). A port
/// of render_session/_ooc.py _sort; @p nodeSize is the node side in level
/// voxels. Nodes that compare equal keep their order.
inline std::vector<Node> backToFront(std::vector<Node> nodes, const std::array<double, 3> &eye,
                                     int nodeSize, int topLevel)
{
    using Key = std::vector<std::array<double, 3>>;
    auto path = [&](const Node &node) {
        Key out;
        const int index[3] = { node.ix, node.iy, node.iz };
        for (int level = std::max(topLevel, node.level); level >= node.level; --level) {
            const int shift = level - node.level;
            const double cell = double(nodeSize) * double(levelScale(level));
            std::array<double, 3> step;
            for (int i = 0; i < 3; ++i) {
                // Cell k spans [k * cell - 0.5, (k + 1) * cell - 0.5) in cube voxels.
                const double eyeCell = std::floor((eye[i] + 0.5) / cell);
                const int ancestor = shift < 31 ? index[i] >> shift : 0;
                step[i] = -std::fabs(double(ancestor) - eyeCell);
            }
            out.push_back(step);
        }
        return out;
    };
    std::vector<std::pair<Key, Node>> keyed;
    keyed.reserve(nodes.size());
    for (const Node &n : nodes)
        keyed.emplace_back(path(n), n);
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    for (size_t i = 0; i < keyed.size(); ++i)
        nodes[i] = keyed[i].second;
    return nodes;
}

}   // namespace CubeLodGeometry

#endif   // CubeLodGeometry_h

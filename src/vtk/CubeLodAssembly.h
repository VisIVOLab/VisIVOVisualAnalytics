#ifndef CubeLodAssembly_h
#define CubeLodAssembly_h

#include "CubeLodGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

// ---------------------------------------------------------------------------
// CubeLodAssembly — the volumes of a refined 3-D view, before VTK sees them
// (issue #321, explicit refine).
//
// A refined view is a box of fine blocks (nodes of one finer level, read from
// the backend with one ghost sample per side) and at most six coarse pieces
// around it, cut from the preview the window already holds. This header says
// what each of those volumes is: the cube voxels it owns, the samples it
// carries, where it sits in the window's world frame, the cropping planes that
// make it draw exactly its own region, and the order to draw them in. It holds
// no pixels of its own apart from cutSamples, and no VTK, so the whole
// assembly is unit-tested (tests/test_cube_lod_assembly.cpp).
//
// The world frame is the preview's (CubeLodGeometry::toWorld): everything
// already drawn in the 3-D view stays where it is.
// ---------------------------------------------------------------------------
namespace CubeLodAssembly {

using CubeLodGeometry::Box;
using CubeLodGeometry::Crop;
using CubeLodGeometry::Dims;
using CubeLodGeometry::Node;
using CubeLodGeometry::Placement;
using CubeLodGeometry::Sampling;
using CubeLodGeometry::Side;

/// One volume of the refined view.
struct Block
{
    bool fine{ false };
    Side side{ Side::Inner };   ///< a coarse piece's place in the kd-split
    Node node;                  ///< a fine block's node
    /// The cube voxels it draws, half-open. Together the blocks own the cube
    /// exactly once.
    Box owned;
    /// Its samples, as indices of its own grid (the preview, or a level),
    /// ghosts included. At the cube's border they may lie outside the grid,
    /// where the edge sample is repeated.
    Box samples;
    Sampling sampling;          ///< how its grid samples the cube
};

/// The coarse pieces around @p fineBox, each cut from a preview that samples
/// the cube as @p preview: the kd-split of the cube (CubeLodGeometry::kdSplit)
/// with the preview samples each piece needs to draw its region up to its
/// faces (a ghost sample on every side).
inline std::vector<Block> coarsePieces(const Dims &cubeShape, const Sampling &preview,
                                       const Box &fineBox)
{
    std::vector<Block> out;
    for (const CubeLodGeometry::Piece &p : CubeLodGeometry::kdSplit(cubeShape, fineBox)) {
        Block b;
        b.side = p.side;
        b.owned = p.box;
        b.samples = CubeLodGeometry::bracketingSamples(p.box, preview);
        b.sampling = preview;
        out.push_back(b);
    }
    return out;
}

/// The fine block of @p node (nodes of @p nodeSize level voxels), with
/// @p ghost samples per side, before it is read. Its sampling is set from the
/// backend's answer (pyramid block means or a strided read). With
/// @p levelClip (samples of the node's level, as CubeLodPlan::Plan::levelBox)
/// the node is cut to it: it reads and owns only that part.
inline Block fineBlock(const Dims &cubeShape, int nodeSize, const Node &node, int ghost,
                       const std::optional<Box> &levelClip = std::nullopt)
{
    Block b;
    b.fine = true;
    b.node = node;
    Box level = CubeLodGeometry::nodeLevelBox(cubeShape, nodeSize, node);
    if (levelClip)
        level = CubeLodGeometry::intersect(level, *levelClip);
    b.owned = CubeLodGeometry::cubeBoxOfLevelBox(level, cubeShape, node.level);
    for (int i = 0; i < 3; ++i) {
        b.samples.lo[i] = level.lo[i] - ghost;
        b.samples.hi[i] = level.hi[i] + ghost;
    }
    b.sampling = CubeLodGeometry::Sampling::pyramidLevel(node.level);
    return b;
}

/// True when the block's samples reach its owned region's faces: every point
/// it draws lies between two of its samples, so nothing is extrapolated.
inline bool covers(const Block &b)
{
    const Box need = CubeLodGeometry::bracketingSamples(b.owned, b.sampling);
    for (int i = 0; i < 3; ++i) {
        if (need.lo[i] < b.samples.lo[i] || need.hi[i] > b.samples.hi[i])
            return false;
    }
    return !b.owned.empty();
}

/// Samples per axis of the block.
inline Dims dims(const Block &b)
{
    return { b.samples.hi[0] - b.samples.lo[0], b.samples.hi[1] - b.samples.lo[1],
             b.samples.hi[2] - b.samples.lo[2] };
}

/// Copy @p box (grid indices, x fastest) out of @p grid, whose shape is
/// @p gridShape: indices outside the grid take the nearest edge sample, as
/// the backend's ghosts do at the cube's border.
inline std::vector<float> cutSamples(const float *grid, const Dims &gridShape, const Box &box)
{
    std::vector<float> out;
    if (!grid || box.empty() || gridShape[0] < 1 || gridShape[1] < 1 || gridShape[2] < 1)
        return out;
    out.resize(std::size_t(box.count()));
    const std::size_t nx = std::size_t(gridShape[0]);
    const std::size_t nxy = nx * std::size_t(gridShape[1]);
    std::size_t k = 0;
    for (int z = box.lo[2]; z < box.hi[2]; ++z) {
        const std::size_t zz = std::size_t(std::clamp(z, 0, gridShape[2] - 1));
        for (int y = box.lo[1]; y < box.hi[1]; ++y) {
            const std::size_t yy = std::size_t(std::clamp(y, 0, gridShape[1] - 1));
            const float *row = grid + zz * nxy + yy * nx;
            for (int x = box.lo[0]; x < box.hi[0]; ++x)
                out[k++] = row[std::clamp(x, 0, gridShape[0] - 1)];
        }
    }
    return out;
}

/// Where the block sits in the window's world frame (the preview's) and what
/// it draws there.
struct Geometry
{
    Placement placement;   ///< origin of sample 0 and spacing, world units
    Dims dims{ 0, 0, 0 };
    Crop drawn{};          ///< the world box it draws (its owned region)
};

inline Geometry geometryOf(const Block &b, const Dims &cubeShape, const Sampling &preview)
{
    Geometry g;
    g.placement = CubeLodGeometry::placeBlock(b.samples.lo, b.sampling, preview);
    g.dims = dims(b);
    g.drawn = CubeLodGeometry::cropToWorld(CubeLodGeometry::ownedCrop(b.owned, cubeShape), preview);
    return g;
}

/// The world box @p g draws within an extra clip @p userCrop (world units, as
/// a headset's clip box), or nothing when the two do not overlap.
inline std::optional<Crop> clipped(const Geometry &g, const std::optional<Crop> &userCrop)
{
    if (!userCrop)
        return g.drawn;
    Crop out;
    for (int axis = 0; axis < 3; ++axis) {
        out[2 * axis] = std::max(g.drawn[2 * axis], (*userCrop)[2 * axis]);
        out[2 * axis + 1] = std::min(g.drawn[2 * axis + 1], (*userCrop)[2 * axis + 1]);
        if (!(out[2 * axis] < out[2 * axis + 1]))
            return std::nullopt;
    }
    return out;
}

/// How far (world units) a face is moved off the eye: a hundredth of a
/// preview sample, under a sixth of the ghost reach beyond a face of a
/// level-0 block under a stride-8 preview (half of its 0.125 spacing).
inline constexpr double kEyeFaceClearance = 1e-2;

/// @p drawn with every face that lies within @p clearance of the eye moved
/// to @p clearance from it, on the side it was on.
///
/// Why: two blocks meet on a shared face, each cropped to its side of it
/// with its own float32 planes, so between them there is a sliver one
/// rounding wide (or an overlap). A ray that runs inside that face never
/// crosses it: it lies in the sliver neither block draws (or both do), and
/// the pixels of those rays show a line of whatever is behind. That happens whenever the eye is on the face's
/// plane, which is the default camera's place: it sits on the cube's centre
/// planes, where two nodes of a level meet. Moving the face off the eye puts
/// those rays inside one block. The function depends only on the face's
/// value and the eye, so the two blocks of a face move it to the same place
/// and still meet without a gap or an overlap, as long as every block of a set
/// is given the same @p clearance; keep it well under the ghost reach beyond
/// each face (half a block spacing), so nothing is extrapolated.
///
/// In a parallel projection the rays are parallel to every face along the
/// projection direction, not only to the faces through the camera: a face
/// that falls exactly on a pixel column can still show, in the float32
/// rounding between its two blocks. Perspective views (the viewer's default)
/// have only the faces through the eye.
inline Crop clearOfEye(Crop drawn, const std::array<double, 3> &eye,
                       double clearance = kEyeFaceClearance)
{
    for (int axis = 0; axis < 3; ++axis) {
        const double e = eye[axis];
        if (!std::isfinite(e))
            continue;
        const double lo = drawn[size_t(2 * axis)];
        const double hi = drawn[size_t(2 * axis + 1)];
        for (int k = 2 * axis; k <= 2 * axis + 1; ++k) {
            const double face = drawn[size_t(k)];
            if (std::fabs(face - e) < clearance)
                drawn[size_t(k)] = face <= e ? e - clearance : e + clearance;
        }
        // A sliver thinner than the clearance with the eye in it would turn
        // inside out: it keeps its faces (it is next to invisible anyway).
        if (!(drawn[size_t(2 * axis)] < drawn[size_t(2 * axis + 1)])) {
            drawn[size_t(2 * axis)] = lo;
            drawn[size_t(2 * axis + 1)] = hi;
        }
    }
    return drawn;
}

/// The cropping planes to give the block's mapper so it draws @p drawn.
///
/// No kFaceEps pull here, only the float32 rounding that keeps each face
/// inside its block: a sliver of 1e-3 between neighbours is crossed by rays
/// running almost along the face (the eye just off its plane) over a tenth
/// of their length, which showed as a dark wedge along the face at high zoom.
/// The ray lying exactly in a face, which the pull was there for, is now
/// kept off every face by clearOfEye.
inline Crop cropPlanes(const Geometry &g, const Crop &drawn)
{
    return CubeLodGeometry::vtkCropPlanes(drawn, g.placement.origin, g.placement.spacing, g.dims, 0.0);
}

/// Indices into @p blocks in back-to-front order for an eye at @p eyeCube
/// (cube voxels): the kd-split's order around @p fineBox, the fine box itself
/// drawn in its octree order (fine nodes of @p nodeSize level voxels).
inline std::vector<int> drawOrder(const std::vector<Block> &blocks, const Box &fineBox,
                                  const std::array<double, 3> &eyeCube, int nodeSize)
{
    std::vector<CubeLodGeometry::Piece> pieces;
    std::vector<Node> nodes;
    int level = 0;
    for (const Block &b : blocks) {
        if (b.fine) {
            nodes.push_back(b.node);
            level = std::max(level, b.node.level);
        } else {
            pieces.push_back({ b.side, b.owned });
        }
    }
    auto indexOfSide = [&blocks](Side s) {
        for (std::size_t i = 0; i < blocks.size(); ++i)
            if (!blocks[i].fine && blocks[i].side == s)
                return int(i);
        return -1;
    };
    auto indexOfNode = [&blocks](const Node &n) {
        for (std::size_t i = 0; i < blocks.size(); ++i)
            if (blocks[i].fine && blocks[i].node == n)
                return int(i);
        return -1;
    };
    std::vector<int> out;
    out.reserve(blocks.size());
    for (Side s : CubeLodGeometry::kdBackToFront(fineBox, eyeCube, pieces)) {
        if (s != Side::Inner) {
            out.push_back(indexOfSide(s));
            continue;
        }
        for (const Node &n : CubeLodGeometry::backToFront(nodes, eyeCube, nodeSize, level))
            out.push_back(indexOfNode(n));
    }
    out.erase(std::remove(out.begin(), out.end(), -1), out.end());
    return out;
}

}   // namespace CubeLodAssembly

#endif   // CubeLodAssembly_h

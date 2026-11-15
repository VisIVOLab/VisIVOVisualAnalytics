#ifndef CubeLodPlan_h
#define CubeLodPlan_h

#include "CubeLodGeometry.h"

#include <array>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// CubeLodPlan — which finer level of the pyramid, over which box of the cube,
// the 3-D view should show on top of the coarse preview (issue #321).
//
// Pure: the caller measures the view (the part of the cube in the frustum, the
// focal point, how many cube voxels one screen pixel covers there) and the
// machine (detail budget, GPU 3-D texture side), and gets back a level and the
// nodes to read. Nothing here knows about VTK, Qt or the backend.
// ---------------------------------------------------------------------------
namespace CubeLodPlan {

using CubeLodGeometry::Box;
using CubeLodGeometry::Dims;
using CubeLodGeometry::Node;

struct Inputs
{
    Dims cubeShape{ 0, 0, 0 };   ///< full-resolution voxels, x, y, z
    int levelCount{ 0 };         ///< levels 0 .. levelCount - 1 can be read
    int previewStride{ 1 };      ///< cube voxels per preview sample
    /// The part of the cube in view, cube voxels (frustum ∩ cube); empty when
    /// the cube is out of view.
    Box visibleBox;
    /// The camera's focal point, cube voxels (it may lie off the cube).
    std::array<double, 3> focalPoint{ 0.0, 0.0, 0.0 };
    /// Cube voxels one screen pixel covers at the focal depth.
    double voxelsPerPixel{ 1.0 };
    /// GPU bytes the fine set may take, float32, ghost samples included.
    std::int64_t budgetBytes{ 512LL * 1024 * 1024 };
    /// GL_MAX_3D_TEXTURE_SIZE: no block may be longer on any axis, ghosts included.
    int maxTextureSide{ 2048 };
    /// Most blocks in the fine set (each is one ray-casting pass).
    int maxBlocks{ 16 };
    /// Node side in level voxels (brick-aligned: 128 or 256).
    int nodeSize{ 256 };
};

enum class Reason {
    Refined,          ///< the whole box at the level the zoom asks for (or the budget allows)
    Shrunk,           ///< a box around the focal point: the budget cannot hold the whole box
    NotInView,        ///< the box is empty: nothing of the cube to refine
    PreviewFineEnough,///< the preview already has the detail the view can show
    BudgetTooSmall,   ///< not even one node fits the budget
    BadInputs,        ///< node size, texture side, block count or cube shape unusable
};

struct Plan
{
    bool refine{ false };
    int level{ -1 };
    std::vector<Node> nodes;   ///< x fastest; empty when !refine
    /// The node side the nodes are on (effectiveNodeSize): turn a node into
    /// its samples with this, not with Inputs::nodeSize.
    int nodeSize{ 0 };
    Box cubeBox;               ///< cube voxels the nodes cover (empty when !refine)
    Reason reason{ Reason::BadInputs };
};

/// The ghost samples per side every block carries.
inline constexpr int kGhost = 1;

/// The node side actually used: @p in.nodeSize, halved until a node and its
/// ghosts fit the GPU's 3-D texture side (halving keeps nodes on the brick
/// grid). 0 when nothing fits.
int effectiveNodeSize(const Inputs &in);

/// GPU bytes of @p nodes (float32, ghost samples included).
std::int64_t nodeBytes(const Inputs &in, const std::vector<Node> &nodes);

/// floor(log2(voxelsPerPixel)), at least 0: the level whose samples are about
/// one screen pixel apart at the focal depth.
int pixelLevel(double voxelsPerPixel);

/// The plan for the current view.
///
/// The level is the coarser of the one the zoom asks for (pixelLevel) and the
/// finest one whose nodes covering the visible box fit the budget and the
/// block count. When that level is no finer than the preview (2^l >= stride),
/// there are two cases: if the zoom itself asks for no more than the preview
/// has, nothing is refined; if only the budget is in the way, the coarsest
/// level finer than the preview is used over a box around the focal point
/// (clamped into the visible box), grown node by node while it fits. A zoom
/// asking for a level the pyramid does not have (coarser than its last, yet
/// finer than the preview) gets the pyramid's last level.
Plan planFocus(const Inputs &in);

/// The plan for an explicit box of the cube (the "Refine selected box"
/// action): the finest level whose nodes covering @p cubeBox fit, regardless
/// of the zoom; when none finer than the preview fits, a box of that level
/// around the box's centre. @p in.visibleBox, focalPoint and voxelsPerPixel
/// are not used.
Plan planBox(const Box &cubeBox, const Inputs &in);

/// A short sentence for the status line or a tooltip.
const char *reasonText(Reason reason);

}   // namespace CubeLodPlan

#endif   // CubeLodPlan_h

#include "CubeLodPlan.h"

#include <algorithm>
#include <cmath>

namespace CubeLodPlan {

using namespace CubeLodGeometry;

int effectiveNodeSize(const Inputs &in)
{
    if (in.nodeSize < 1)
        return 0;
    // Halved, not trimmed: a node of a half or a quarter of a brick-aligned
    // side is still brick-aligned, so its reads stay on whole chunks.
    int side = in.nodeSize;
    while (side > 1 && side + 2 * kGhost > in.maxTextureSide)
        side /= 2;
    return side + 2 * kGhost <= in.maxTextureSide ? side : 0;
}

std::int64_t nodeBytes(const Inputs &in, const std::vector<Node> &nodes, const Box *levelBox)
{
    const int side = effectiveNodeSize(in);
    std::int64_t bytes = 0;
    for (const Node &n : nodes) {
        Box b = nodeLevelBox(in.cubeShape, side, n);
        if (levelBox)
            b = intersect(b, *levelBox);
        if (b.empty())
            continue;
        std::int64_t voxels = 1;
        for (int i = 0; i < 3; ++i)
            voxels *= b.hi[i] - b.lo[i] + 2 * kGhost;
        bytes += voxels * std::int64_t(sizeof(float));
    }
    return bytes;
}

std::int64_t planBytes(const Inputs &in, const Plan &plan)
{
    return nodeBytes(in, plan.nodes, &plan.levelBox);
}

Box blockLevelBox(const Plan &plan, const Dims &cubeShape, const Node &node)
{
    return intersect(nodeLevelBox(cubeShape, plan.nodeSize, node), plan.levelBox);
}

int pixelLevel(double voxelsPerPixel)
{
    if (!(voxelsPerPixel >= 1.0))   // also NaN
        return 0;
    // An infinite or absurd value (a degenerate camera) is simply "very coarse".
    return int(std::min(62.0, std::floor(std::log2(voxelsPerPixel))));
}

const char *reasonText(Reason reason)
{
    switch (reason) {
    case Reason::Refined:
        return "Finer detail over the part of the cube in view";
    case Reason::Shrunk:
        return "Finer detail around the focal point: the detail budget cannot hold all of the view";
    case Reason::BoxShrunk:
        return "Finer detail around the box's centre: the detail budget cannot hold all of the box";
    case Reason::NotInView:
        return "No part of the cube is in view";
    case Reason::PreviewFineEnough:
        return "The preview already shows all the detail the view can resolve";
    case Reason::BudgetTooSmall:
        return "The detail budget is too small for one block of a finer level";
    case Reason::BadInputs:
        break;
    }
    return "Level of detail is unavailable for this cube";
}

namespace {

/// True when the nodes of @p range, clipped to @p levelBox (samples of
/// @p level), fit the block count and the budget; they go to @p out. The count
/// is checked first, so a huge range is never listed only to be rejected.
bool fits(const Inputs &in, const Box &range, int level, const Box &levelBox, std::vector<Node> *out)
{
    if (range.empty() || range.count() > in.maxBlocks)
        return false;
    std::vector<Node> nodes = nodesIn(range, level);
    if (nodeBytes(in, nodes, &levelBox) > in.budgetBytes)
        return false;
    if (out)
        *out = std::move(nodes);
    return true;
}

Plan refined(const Inputs &in, int level, const Box &range, const Box &levelBox,
             std::vector<Node> nodes, Reason reason)
{
    Plan p;
    p.refine = true;
    p.level = level;
    p.nodes = std::move(nodes);
    p.nodeSize = effectiveNodeSize(in);
    p.cubeBox = intersect(cubeBoxOfRange(range, in.cubeShape, level, p.nodeSize),
                          cubeBoxOfLevelBox(levelBox, in.cubeShape, level));
    p.levelBox = levelBox;
    p.reason = reason;
    return p;
}

Plan notRefined(Reason reason)
{
    Plan p;
    p.reason = reason;
    return p;
}

/// The nodes of @p level around @p focal within @p box: the focal point's node,
/// grown by one layer of nodes at a time (x below, x above, y below, ...) for
/// as long as the result fits. The nodes are clipped to @p levelBox.
Plan shrinkAround(const Inputs &in, const Box &box, const std::array<double, 3> &focal, int level,
                  const Box &levelBox, Reason reason)
{
    const int side = effectiveNodeSize(in);
    const Box range = nodeRangeCovering(box, in.cubeShape, level, side);
    const std::int64_t cubeSide = std::int64_t(side) * levelScale(level);
    Box cur;
    for (int i = 0; i < 3; ++i) {
        // The focal voxel, held inside the box (the camera may look past the cube).
        // An infinite one goes to that side of the box, a NaN to its middle.
        const double f = std::isnan(focal[i]) ? (box.lo[i] + box.hi[i] - 1) / 2.0 : std::floor(focal[i] + 0.5);
        const double v = std::clamp(f, double(box.lo[i]), double(box.hi[i] - 1));
        cur.lo[i] = std::clamp(int(std::int64_t(v) / cubeSide), range.lo[i], range.hi[i] - 1);
        cur.hi[i] = cur.lo[i] + 1;
    }
    std::vector<Node> nodes;
    if (!fits(in, cur, level, levelBox, &nodes))
        return notRefined(Reason::BudgetTooSmall);
    for (bool grew = true; grew;) {
        grew = false;
        for (int axis = 0; axis < 3; ++axis) {
            for (const bool up : { false, true }) {
                Box next = cur;
                if (up && next.hi[axis] < range.hi[axis])
                    ++next.hi[axis];
                else if (!up && next.lo[axis] > range.lo[axis])
                    --next.lo[axis];
                else
                    continue;
                std::vector<Node> grown;
                if (fits(in, next, level, levelBox, &grown)) {
                    cur = next;
                    nodes = std::move(grown);
                    grew = true;
                }
            }
        }
    }
    return refined(in, level, cur, levelBox, std::move(nodes), reason);
}

bool usable(const Inputs &in)
{
    for (int n : in.cubeShape)
        if (n < 1)
            return false;
    return in.levelCount >= 1 && in.previewStride >= 1 && in.maxBlocks >= 1 && in.budgetBytes > 0
            && effectiveNodeSize(in) >= 1;
}

/// Shared by planFocus and planBox: the finest level from @p minLevel on whose
/// nodes covering @p box fit, else a box around @p focal. With @p clipToBox the
/// nodes are clipped to the box (an explicit box is refined as chosen, not as
/// the whole nodes it touches); without, they are whole (a view's plan keeps
/// the brick-aligned nodes its cache and later plans share).
Plan plan(const Inputs &in, const Box &wanted, const std::array<double, 3> &focal, int minLevel,
          bool clipToBox)
{
    if (!usable(in))
        return notRefined(Reason::BadInputs);
    const Box box = intersect(wanted, wholeBox(in.cubeShape));
    if (box.empty())
        return notRefined(Reason::NotInView);
    // A level whose samples are as far apart as the preview's adds nothing.
    if (levelScale(minLevel) >= in.previewStride)
        return notRefined(Reason::PreviewFineEnough);
    int coarsest = 0;   // the coarsest readable level still finer than the preview
    while (coarsest + 1 < in.levelCount && levelScale(coarsest + 1) < in.previewStride)
        ++coarsest;
    const int side = effectiveNodeSize(in);
    auto levelBoxOf = [&](int level) {
        return clipToBox ? levelBoxCovering(box, in.cubeShape, level)
                         : wholeBox(levelShape(in.cubeShape, level));
    };
    for (int level = std::min(minLevel, coarsest); level <= coarsest; ++level) {
        const Box range = nodeRangeCovering(box, in.cubeShape, level, side);
        const Box levelBox = levelBoxOf(level);
        std::vector<Node> nodes;
        if (fits(in, range, level, levelBox, &nodes))
            return refined(in, level, range, levelBox, std::move(nodes), Reason::Refined);
    }
    return shrinkAround(in, box, focal, coarsest, levelBoxOf(coarsest),
                        clipToBox ? Reason::BoxShrunk : Reason::Shrunk);
}

}   // namespace

Plan planFocus(const Inputs &in)
{
    return plan(in, in.visibleBox, in.focalPoint, pixelLevel(in.voxelsPerPixel), false);
}

Plan planBox(const Box &cubeBox, const Inputs &in)
{
    std::array<double, 3> centre;
    for (int i = 0; i < 3; ++i)
        centre[i] = (double(cubeBox.lo[i]) + double(cubeBox.hi[i]) - 1.0) / 2.0;
    return plan(in, cubeBox, centre, 0, true);
}

}   // namespace CubeLodPlan

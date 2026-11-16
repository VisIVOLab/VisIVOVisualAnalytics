// test_cube_lod_plan.cpp
// Unit tests for CubeLodPlan: the level and the nodes the 3-D view refines on
// top of the coarse preview (issue #321). The level follows the zoom (cube
// voxels per screen pixel at the focal depth), coarsens until the nodes fit
// the detail budget and the block count, is never as coarse as the preview,
// and falls back to a box around the focal point when only the budget is in
// the way.

#include "TestCubeLodPlan.h"
#include "vtk/CubeLodPlan.h"

#include <QTest>

using namespace CubeLodPlan;
using CubeLodGeometry::levelScale;
using CubeLodGeometry::levelShapes;
using CubeLodGeometry::nodeLevelBox;
using CubeLodGeometry::wholeBox;

namespace {

constexpr std::int64_t MB = 1024 * 1024;

/// A 2048^3 cube with a 256^3 preview (stride 8) and a pyramid of 256 bricks
/// (levels 0..3), a 512 MB budget.
Inputs bigCube()
{
    Inputs in;
    in.cubeShape = { 2048, 2048, 2048 };
    in.levelCount = int(levelShapes(in.cubeShape, 256).size());
    in.previewStride = 8;
    in.visibleBox = wholeBox(in.cubeShape);
    in.focalPoint = { 1024.0, 1024.0, 1024.0 };
    in.voxelsPerPixel = 1.0;
    in.budgetBytes = 512 * MB;
    in.maxTextureSide = 2048;
    in.maxBlocks = 16;
    in.nodeSize = 256;
    return in;
}

bool contains(const Box &outer, const Box &inner)
{
    for (int i = 0; i < 3; ++i)
        if (inner.lo[i] < outer.lo[i] || inner.hi[i] > outer.hi[i])
            return false;
    return true;
}

}   // namespace

void TestCubeLodPlan::pixelLevel_isFloorLog2()
{
    QCOMPARE(pixelLevel(0.3), 0);
    QCOMPARE(pixelLevel(1.0), 0);
    QCOMPARE(pixelLevel(1.99), 0);
    QCOMPARE(pixelLevel(2.0), 1);
    QCOMPARE(pixelLevel(7.9), 2);
    QCOMPARE(pixelLevel(8.0), 3);
    QCOMPARE(pixelLevel(std::nan("")), 0);
    QCOMPARE(pixelLevel(-4.0), 0);
    QCOMPARE(pixelLevel(std::numeric_limits<double>::infinity()), 62);
}

void TestCubeLodPlan::level_followsTheZoom()
{
    // One node's worth of the cube in view, plenty of budget: the level is
    // the one whose samples are about a screen pixel apart.
    Inputs in = bigCube();
    in.visibleBox = Box{ { 1024, 1024, 1024 }, { 1280, 1280, 1280 } };
    in.focalPoint = { 1100.0, 1100.0, 1100.0 };
    const std::pair<double, int> zoom[] = { { 0.4, 0 }, { 1.0, 0 }, { 2.5, 1 }, { 4.0, 2 }, { 7.0, 2 } };
    for (const auto &[vpp, level] : zoom) {
        in.voxelsPerPixel = vpp;
        const Plan p = planFocus(in);
        QVERIFY(p.refine);
        QCOMPARE(p.reason, Reason::Refined);
        QCOMPARE(p.level, level);
        QCOMPARE(int(p.nodes.size()), 1);
        QVERIFY(contains(p.cubeBox, in.visibleBox));
    }
}

void TestCubeLodPlan::level_coarsensToFitTheBudget()
{
    // The whole cube in view at full zoom: level 0 is 512 nodes and level 1
    // 64, both over 16 blocks; level 2 is 8 nodes of 258^3 floats (549 MB).
    Inputs in = bigCube();
    in.budgetBytes = 1024 * MB;
    Plan p = planFocus(in);
    QVERIFY(p.refine);
    QCOMPARE(p.reason, Reason::Refined);
    QCOMPARE(p.level, 2);
    QCOMPARE(int(p.nodes.size()), 8);
    QCOMPARE(p.cubeBox, wholeBox(in.cubeShape));
    QVERIFY(nodeBytes(in, p.nodes) <= in.budgetBytes);
    QCOMPARE(nodeBytes(in, p.nodes), 8LL * 258 * 258 * 258 * 4);
    // The zoom asking for a coarser level wins over a finer one that fits.
    in.visibleBox = Box{ { 0, 0, 0 }, { 256, 256, 256 } };
    in.voxelsPerPixel = 4.0;
    p = planFocus(in);
    QCOMPARE(p.level, 2);
}

void TestCubeLodPlan::noRefine_whenTheLevelIsAsCoarseAsThePreview()
{
    Inputs in = bigCube();
    in.visibleBox = Box{ { 1024, 1024, 1024 }, { 1280, 1280, 1280 } };
    for (const double vpp : { 8.0, 9.5, 64.0, 1e9 }) {
        in.voxelsPerPixel = vpp;
        const Plan p = planFocus(in);
        QVERIFY(!p.refine);
        QCOMPARE(p.reason, Reason::PreviewFineEnough);
        QVERIFY(p.nodes.empty());
        QVERIFY(p.cubeBox.empty());
    }
    // A preview of stride 4: level 2 (2^2 = 4) adds nothing either.
    in.previewStride = 4;
    in.voxelsPerPixel = 4.0;
    QCOMPARE(planFocus(in).reason, Reason::PreviewFineEnough);
    in.voxelsPerPixel = 3.9;
    QCOMPARE(planFocus(in).level, 1);
    // The preview is the cube: never.
    in.previewStride = 1;
    in.voxelsPerPixel = 1.0;
    QCOMPARE(planFocus(in).reason, Reason::PreviewFineEnough);
}

void TestCubeLodPlan::pyramidWithFewLevels_usesItsLast()
{
    // The zoom asks for level 2, the pyramid has levels 0 and 1 only, the
    // preview is stride 8: level 1 is still finer than the preview.
    Inputs in = bigCube();
    in.levelCount = 2;
    in.visibleBox = Box{ { 1024, 1024, 1024 }, { 1280, 1280, 1280 } };
    in.voxelsPerPixel = 4.0;
    const Plan p = planFocus(in);
    QVERIFY(p.refine);
    QCOMPARE(p.level, 1);
}

void TestCubeLodPlan::shrink_aroundTheFocalPoint()
{
    // The whole cube in view, 300 MB: even level 2 (the coarsest finer than
    // the stride-8 preview) does not fit whole, so a box of it around the
    // focal point does.
    Inputs in = bigCube();
    in.budgetBytes = 300 * MB;
    in.focalPoint = { 1500.0, 300.0, 1000.0 };
    const Plan p = planFocus(in);
    QVERIFY(p.refine);
    QCOMPARE(p.reason, Reason::Shrunk);
    QCOMPARE(p.level, 2);
    QVERIFY(!p.nodes.empty());
    QVERIFY(int(p.nodes.size()) <= in.maxBlocks);
    QVERIFY(nodeBytes(in, p.nodes) <= in.budgetBytes);
    QVERIFY(contains(p.cubeBox, Box{ { 1500, 300, 1000 }, { 1501, 301, 1001 } }));
    QVERIFY(contains(wholeBox(in.cubeShape), p.cubeBox));
    QVERIFY(p.cubeBox != wholeBox(in.cubeShape));
    // Grown while it fits: one more node would not.
    QVERIFY(nodeBytes(in, p.nodes) + 258LL * 258 * 258 * 4 > in.budgetBytes);
    QVERIFY(QString::fromLatin1(reasonText(p.reason)).contains(u"focal point"));
}

void TestCubeLodPlan::snapping_isOutwardToTheNodeGrid()
{
    Inputs in = bigCube();
    in.visibleBox = Box{ { 100, 50, 1000 }, { 700, 90, 1001 } };
    in.focalPoint = { 400.0, 70.0, 1000.0 };
    in.voxelsPerPixel = 4.0;   // level 2: nodes of 1024 cube voxels
    Plan p = planFocus(in);
    QCOMPARE(p.level, 2);
    QCOMPARE(p.cubeBox, (Box{ { 0, 0, 0 }, { 1024, 1024, 1024 } }));
    in.voxelsPerPixel = 1.0;   // level 0: nodes of 256
    p = planFocus(in);
    QCOMPARE(p.level, 0);
    QCOMPARE(p.cubeBox, (Box{ { 0, 0, 768 }, { 768, 256, 1024 } }));
    QCOMPARE(int(p.nodes.size()), 3);
    QVERIFY(contains(p.cubeBox, in.visibleBox));
}

void TestCubeLodPlan::textureSide_capsTheNodes()
{
    Inputs in;
    in.cubeShape = { 600, 600, 600 };
    in.levelCount = 3;
    in.previewStride = 4;
    in.visibleBox = wholeBox(in.cubeShape);
    in.focalPoint = { 300.0, 300.0, 300.0 };
    in.budgetBytes = 4096 * MB;
    in.maxBlocks = 1000;
    in.nodeSize = 256;
    in.maxTextureSide = 130;   // nodes of 128 + 2 ghosts
    QCOMPARE(effectiveNodeSize(in), 128);
    const Plan p = planFocus(in);
    QVERIFY(p.refine);
    QCOMPARE(p.level, 0);
    QCOMPARE(int(p.nodes.size()), 125);   // ceil(600/128)^3
    QCOMPARE(p.nodeSize, 128);            // the side the nodes are on
    for (const Node &n : p.nodes) {
        const Box b = nodeLevelBox(in.cubeShape, effectiveNodeSize(in), n);
        for (int i = 0; i < 3; ++i)
            QVERIFY(b.hi[i] - b.lo[i] + 2 * kGhost <= in.maxTextureSide);
    }
    // Halved, not trimmed to 198: nodes stay on the brick grid.
    in.maxTextureSide = 200;
    QCOMPARE(effectiveNodeSize(in), 128);
    in.maxTextureSide = 258;
    QCOMPARE(effectiveNodeSize(in), 256);
    // A side that leaves no room for a node is unusable.
    in.maxTextureSide = 2;
    QCOMPARE(planFocus(in).reason, Reason::BadInputs);
}

void TestCubeLodPlan::maxBlocks_isRespected()
{
    Inputs in;
    in.cubeShape = { 600, 600, 600 };
    in.levelCount = 3;
    in.previewStride = 4;
    in.visibleBox = wholeBox(in.cubeShape);
    in.focalPoint = { 10.0, 590.0, 300.0 };
    in.budgetBytes = 4096 * MB;
    in.maxBlocks = 8;
    in.nodeSize = 128;
    // Level 0 is 125 nodes, level 1 (300^3) 27: neither fits 8 blocks, and
    // level 2 is as coarse as the preview: 8 nodes of level 1 near the focus.
    const Plan p = planFocus(in);
    QVERIFY(p.refine);
    QCOMPARE(p.reason, Reason::Shrunk);
    QCOMPARE(p.level, 1);
    QVERIFY(int(p.nodes.size()) <= 8);
    QVERIFY(contains(p.cubeBox, Box{ { 10, 590, 300 }, { 11, 591, 301 } }));
}

void TestCubeLodPlan::cameraOffTheCube()
{
    Inputs in = bigCube();
    // Nothing of the cube in the frustum.
    in.visibleBox = Box{};
    QCOMPARE(planFocus(in).reason, Reason::NotInView);
    in.visibleBox = Box{ { 3000, 0, 0 }, { 4000, 10, 10 } };   // off the cube
    QCOMPARE(planFocus(in).reason, Reason::NotInView);
    QVERIFY(QString::fromLatin1(reasonText(Reason::NotInView)).contains(u"view"));
    // A corner in view, the focal point far off the cube: the box is held
    // inside the visible part, at the corner nearest the focal point.
    in.visibleBox = Box{ { 0, 0, 0 }, { 2048, 2048, 2048 } };
    in.focalPoint = { -5000.0, 9000.0, std::nan("") };
    in.budgetBytes = 300 * MB;
    const Plan p = planFocus(in);
    QVERIFY(p.refine);
    QCOMPARE(p.reason, Reason::Shrunk);
    QVERIFY(contains(p.cubeBox, Box{ { 0, 2047, 0 }, { 1, 2048, 1 } }));
    // An infinite focal point goes to that side of the visible part.
    in.focalPoint = { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), 5.0 };
    const Plan q = planFocus(in);
    QCOMPARE(q.reason, Reason::Shrunk);
    QVERIFY(contains(q.cubeBox, Box{ { 2047, 0, 5 }, { 2048, 1, 6 } }));
    // A NaN one goes to the middle of the visible part: with room for one
    // level-2 node (1024 cube voxels), z in [1024, 2048) picks node 1.
    in.visibleBox = Box{ { 0, 0, 1024 }, { 2048, 2048, 2048 } };
    in.focalPoint = { 10.0, 10.0, std::nan("") };
    in.budgetBytes = 100 * MB;
    const Plan r = planFocus(in);
    QCOMPARE(r.reason, Reason::Shrunk);
    QCOMPARE(int(r.nodes.size()), 1);
    QCOMPARE(r.nodes.front(), (Node{ 2, 0, 0, 1 }));
}

void TestCubeLodPlan::budgetTooSmall_forOneNode()
{
    Inputs in = bigCube();
    in.budgetBytes = 1000;
    const Plan p = planFocus(in);
    QVERIFY(!p.refine);
    QCOMPARE(p.reason, Reason::BudgetTooSmall);
}

void TestCubeLodPlan::badInputs()
{
    Inputs in = bigCube();
    in.nodeSize = 0;
    QCOMPARE(planFocus(in).reason, Reason::BadInputs);
    in = bigCube();
    in.cubeShape = { 0, 10, 10 };
    QCOMPARE(planFocus(in).reason, Reason::BadInputs);
    in = bigCube();
    in.levelCount = 0;
    QCOMPARE(planFocus(in).reason, Reason::BadInputs);
    in = bigCube();
    in.maxBlocks = 0;
    QCOMPARE(planBox(Box{ { 0, 0, 0 }, { 10, 10, 10 } }, in).reason, Reason::BadInputs);
    QVERIFY(!QString::fromLatin1(reasonText(Reason::BadInputs)).isEmpty());
}

void TestCubeLodPlan::planBox_refinesTheSelectedBox()
{
    Inputs in = bigCube();
    // The zoom does not matter for an explicit box: the finest level that fits.
    in.voxelsPerPixel = 64.0;
    // The box itself, not the node it lies in: the node is cut to the box.
    Plan p = planBox(Box{ { 1030, 1030, 1030 }, { 1270, 1270, 1270 } }, in);
    QVERIFY(p.refine);
    QCOMPARE(p.reason, Reason::Refined);
    QCOMPARE(p.level, 0);
    QCOMPARE(p.cubeBox, (Box{ { 1030, 1030, 1030 }, { 1270, 1270, 1270 } }));
    QCOMPARE(p.levelBox, p.cubeBox);
    QCOMPARE(int(p.nodes.size()), 1);
    QCOMPARE(blockLevelBox(p, in.cubeShape, p.nodes.front()), p.cubeBox);
    QCOMPARE(planBytes(in, p), 242LL * 242 * 242 * 4);
    // The box the GUI tester chose, 512..1535 on every axis (1024^3 voxels).
    // Level 0 and level 1 (8 blocks of 256^3 + ghosts, 549 MB) do not fit
    // 512 MB; level 2 holds all of it in 8 blocks of 128^3, each a corner of
    // a node cut to the box. Not the half cube around its centre that the
    // whole nodes of level 2 would have been.
    p = planBox(Box{ { 512, 512, 512 }, { 1536, 1536, 1536 } }, in);
    QVERIFY(p.refine);
    QCOMPARE(p.reason, Reason::Refined);
    QCOMPARE(p.level, 2);
    QCOMPARE(p.cubeBox, (Box{ { 512, 512, 512 }, { 1536, 1536, 1536 } }));
    QCOMPARE(int(p.nodes.size()), 8);
    for (const Node &n : p.nodes) {
        const Box b = blockLevelBox(p, in.cubeShape, n);
        QCOMPARE(b.count(), 128LL * 128 * 128);
    }
    QCOMPARE(planBytes(in, p), 8LL * 130 * 130 * 130 * 4);
    // A box on no level's grid is rounded outward to the level's samples only.
    p = planBox(Box{ { 513, 700, 900 }, { 1535, 1101, 1300 } }, in);
    QCOMPARE(p.level, 1);
    QCOMPARE(p.cubeBox, (Box{ { 512, 700, 900 }, { 1536, 1102, 1300 } }));
    // The whole cube: the finest that fits, as for the view.
    in.budgetBytes = 1024 * MB;
    p = planBox(wholeBox(in.cubeShape), in);
    QCOMPARE(p.level, 2);
    QCOMPARE(p.cubeBox, wholeBox(in.cubeShape));
    // Too big for the budget at any finer level: part of the box around its
    // centre, said as such (not "around the focal point").
    in.budgetBytes = 300 * MB;
    p = planBox(wholeBox(in.cubeShape), in);
    QCOMPARE(p.reason, Reason::BoxShrunk);
    QVERIFY(contains(p.cubeBox, Box{ { 1024, 1024, 1024 }, { 1025, 1025, 1025 } }));
    QVERIFY(planBytes(in, p) <= in.budgetBytes);
    QVERIFY(QString::fromLatin1(reasonText(Reason::BoxShrunk)).contains(u"box"));
    // Shrunk within the box: never outside what was chosen.
    p = planBox(Box{ { 100, 100, 100 }, { 1900, 1900, 1900 } }, in);
    QCOMPARE(p.reason, Reason::BoxShrunk);
    QVERIFY(contains(Box{ { 100, 100, 100 }, { 1900, 1900, 1900 } }, p.cubeBox));
    // Outside the cube, or a full-resolution preview: nothing.
    QCOMPARE(planBox(Box{ { -10, -10, -10 }, { 0, 5, 5 } }, in).reason, Reason::NotInView);
    in.previewStride = 1;
    QCOMPARE(planBox(Box{ { 0, 0, 0 }, { 5, 5, 5 } }, in).reason, Reason::PreviewFineEnough);
}

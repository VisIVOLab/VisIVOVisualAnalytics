// test_cube_lod_auto.cpp
// Unit tests for CubeLodAuto and CubeLodPick (issue #321, phase 4). What they
// guard: the part of the cube the camera sees and the zoom, as the planner
// gets them (a box off by a voxel or a stride refines the wrong blocks); the
// fetch decision (a settled camera must not read again what is shown or on
// its way, and must leave a chosen box alone); the block cache's eviction;
// and the pick through a refined view, which must land on the fine grid
// inside the box and on the preview elsewhere.

#include "TestCubeLodAuto.h"
#include "CubeLodAuto.h"
#include "CubeLodPick.h"

#include <QTest>

#include <cmath>
#include <limits>
#include <string>

using namespace CubeLodAuto;
using CubeLodGeometry::Box;

namespace {

bool sameBox(const Box &b, const Box &expected)
{
    return b.lo == expected.lo && b.hi == expected.hi;
}

CubeLodPlan::Inputs bigCube()
{
    CubeLodPlan::Inputs in;
    in.cubeShape = { 2048, 2048, 2048 };
    in.levelCount = 4;
    in.previewStride = 8;
    in.budgetBytes = 512LL << 20;
    in.maxTextureSide = 2048;
    in.maxBlocks = 16;
    in.nodeSize = 256;
    return in;
}

CubeLodPlan::Plan planOf(int level, std::vector<Node> nodes)
{
    CubeLodPlan::Plan p;
    p.refine = !nodes.empty();
    p.level = level;
    p.nodeSize = 256;
    p.nodes = std::move(nodes);
    p.reason = p.refine ? CubeLodPlan::Reason::Refined : CubeLodPlan::Reason::PreviewFineEnough;
    return p;
}

}   // namespace

void TestCubeLodAuto::visibleBox_coversEveryTouchedVoxel()
{
    // A strided preview, every 4th voxel: world w is cube voxel 4 w.
    const Sampling s = Sampling::strided(4);
    const Box b = visibleCubeBox({ 1.2, 3.0, -10.0, 1000.0, 0.0, 0.0 }, s, { 100, 100, 100 });
    // x: cube 4.8 .. 12 touches voxels 5 .. 12; y clamps to the cube; z is the
    // single voxel 0 (a flat box still touches a voxel).
    QVERIFY(sameBox(b, Box{ { 5, 0, 0 }, { 13, 100, 1 } }));
}

void TestCubeLodAuto::visibleBox_blockMeanPreview()
{
    // Block means of 4: sample k stands for cube voxel 4 k + 1.5.
    const Sampling s = Sampling::blockMean(4);
    const Box b = visibleCubeBox({ 0.0, 1.0, 0.0, 1.0, 0.0, 1.0 }, s, { 100, 100, 100 });
    // World 0 is cube 1.5 (voxel 2, its boundary rounds up), world 1 is 5.5.
    QVERIFY(sameBox(b, Box{ { 2, 2, 2 }, { 7, 7, 7 } }));
    // The cube's own world bounds give back the whole cube.
    const auto whole = cubeWorldBounds({ 100, 60, 20 }, s);
    QVERIFY(sameBox(visibleCubeBox(whole, s, { 100, 60, 20 }), Box{ { 0, 0, 0 }, { 100, 60, 20 } }));
}

void TestCubeLodAuto::visibleBox_offTheCubeOrNotFinite()
{
    const Sampling s = Sampling::strided(4);
    QVERIFY(visibleCubeBox({ 200, 300, 0, 1, 0, 1 }, s, { 100, 100, 100 }).empty());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(visibleCubeBox({ nan, 3, 0, 1, 0, 1 }, s, { 100, 100, 100 }).empty());
    const double inf = std::numeric_limits<double>::infinity();
    QVERIFY(visibleCubeBox({ inf, -inf, 0, 1, 0, 1 }, s, { 100, 100, 100 }).empty());
}

void TestCubeLodAuto::voxelsPerPixel_perspectiveAndParallel()
{
    Camera c;
    c.viewAngleDeg = 90.0;
    c.focalDistance = 50.0;
    c.viewportHeightPx = 100;
    // 100 world units over 100 pixels at the focal depth, 4 cube voxels each.
    QCOMPARE(voxelsPerPixel(c, Sampling::strided(4)), 4.0);
    c.parallel = true;
    c.parallelScale = 25.0;
    QCOMPARE(voxelsPerPixel(c, Sampling::blockMean(4)), 2.0);
}

void TestCubeLodAuto::voxelsPerPixel_unusableCamera()
{
    Camera c;
    c.viewportHeightPx = 0;
    QVERIFY(std::isinf(voxelsPerPixel(c, Sampling::strided(4))));
    c.viewportHeightPx = 100;
    c.focalDistance = 0.0;
    QVERIFY(std::isinf(voxelsPerPixel(c, Sampling::strided(4))));
    c.focalDistance = 1.0;
    QVERIFY(std::isinf(voxelsPerPixel(c, Sampling{ 0.0, 0.0 })));
    // "Infinitely coarse" plans nothing finer than the preview.
    CubeLodPlan::Inputs in = bigCube();
    in.visibleBox = CubeLodGeometry::wholeBox(in.cubeShape);
    in.voxelsPerPixel = voxelsPerPixel(Camera{}, Sampling::blockMean(8));
    QVERIFY(!CubeLodPlan::planFocus(in).refine);
}

void TestCubeLodAuto::visibleBoxAndZoom_driveThePlan()
{
    // 2048^3 behind a 256^3 block-mean preview. Zoomed in on world 100..124
    // (cube 803.5 .. 995.5, inside node 3 of level 0 on every axis) at 1.5
    // cube voxels per pixel: one level-0 node.
    const Sampling s = Sampling::blockMean(8);
    CubeLodPlan::Inputs in = bigCube();
    in.visibleBox = visibleCubeBox({ 100, 124, 100, 124, 100, 124 }, s, in.cubeShape);
    QVERIFY(sameBox(in.visibleBox, Box{ { 804, 804, 804 }, { 997, 997, 997 } }));
    Camera c;
    c.parallel = true;
    c.viewportHeightPx = 800;
    c.parallelScale = 1.5 * 800 / 2.0 / 8.0;   // 1.5 cube voxels per pixel
    in.voxelsPerPixel = voxelsPerPixel(c, s);
    QCOMPARE(in.voxelsPerPixel, 1.5);
    in.focalPoint = { 900, 900, 900 };
    const CubeLodPlan::Plan zoomed = CubeLodPlan::planFocus(in);
    QVERIFY(zoomed.refine);
    QCOMPARE(zoomed.level, 0);
    QCOMPARE(int(zoomed.nodes.size()), 1);
    QVERIFY(zoomed.nodes[0] == (Node{ 0, 3, 3, 3 }));
    QCOMPARE(decide(zoomed, State{}), Action::Fetch);

    // Zoomed out to 9 voxels per pixel: the preview's 8 already show it all,
    // and the set the camera had asked for goes.
    c.parallelScale = 9.0 * 800 / 2.0 / 8.0;
    in.voxelsPerPixel = voxelsPerPixel(c, s);
    const CubeLodPlan::Plan out = CubeLodPlan::planFocus(in);
    QVERIFY(!out.refine);
    State shown;
    shown.shown = signatureOf(zoomed);
    QCOMPARE(decide(out, shown), Action::Clear);
}

void TestCubeLodAuto::signature_ignoresNodeOrder()
{
    const auto a = planOf(1, { { 1, 0, 0, 0 }, { 1, 1, 0, 0 }, { 1, 0, 1, 0 } });
    const auto b = planOf(1, { { 1, 0, 1, 0 }, { 1, 0, 0, 0 }, { 1, 1, 0, 0 } });
    QVERIFY(signatureOf(a) == signatureOf(b));
    QVERIFY(signatureOf(a) != signatureOf(planOf(1, { { 1, 0, 0, 0 } })));
    // Another level is another set, even over the same node indices.
    QVERIFY(signatureOf(planOf(2, { { 2, 0, 0, 0 } })) != signatureOf(planOf(1, { { 1, 0, 0, 0 } })));
    QVERIFY(signatureOf(planOf(1, {})).empty());
}

void TestCubeLodAuto::decide_table()
{
    const auto p1 = planOf(1, { { 1, 0, 0, 0 } });
    const auto p2 = planOf(1, { { 1, 1, 0, 0 } });
    const auto none = planOf(-1, {});
    const Signature s1 = signatureOf(p1), s2 = signatureOf(p2);

    // A chosen box wins over the camera, whatever the plan.
    State held;
    held.explicitHeld = true;
    QCOMPARE(decide(p1, held), Action::Keep);
    QCOMPARE(decide(none, held), Action::Keep);

    // Nothing to refine.
    QCOMPARE(decide(none, State{}), Action::Keep);
    State showing;
    showing.shown = s1;
    QCOMPARE(decide(none, showing), Action::Clear);
    State reading;
    reading.pending = s1;
    QCOMPARE(decide(none, reading), Action::Clear);

    // The same set as on screen or on its way: nothing new to read.
    QCOMPARE(decide(p1, showing), Action::Keep);
    QCOMPARE(decide(p1, reading), Action::Keep);
    // Back to what is shown while another set is read: drop that one.
    State both;
    both.shown = s1;
    both.pending = s2;
    QCOMPARE(decide(p1, both), Action::Cancel);
    QCOMPARE(decide(p2, both), Action::Keep);
    // Anything else is read.
    QCOMPARE(decide(p2, showing), Action::Fetch);
    QCOMPARE(decide(p2, State{}), Action::Fetch);
}

void TestCubeLodAuto::lru_evictsLeastRecentlyUsed()
{
    LruCache<std::string, int> cache(10);
    cache.insert("a", 1, 4);
    cache.insert("b", 2, 4);
    QVERIFY(cache.find("a"));   // a is now the most recent
    cache.insert("c", 3, 4);    // 12 > 10: the least recent (b) goes
    QVERIFY(!cache.contains("b"));
    QCOMPARE(cache.keys(), (std::vector<std::string>{ "c", "a" }));
    QCOMPARE(cache.bytes(), std::int64_t(8));
    QCOMPARE(*cache.find("a"), 1);
    QVERIFY(!cache.find("b"));
    // Replacing a key takes its new size.
    cache.insert("a", 5, 2);
    QCOMPARE(cache.bytes(), std::int64_t(6));
    QCOMPARE(*cache.find("a"), 5);
    cache.remove("c");
    QCOMPARE(cache.size(), std::size_t(1));
    cache.clear();
    QCOMPARE(cache.bytes(), std::int64_t(0));
    QCOMPARE(cache.size(), std::size_t(0));
}

void TestCubeLodAuto::lru_capacityAndOversize()
{
    LruCache<std::string, int> cache(10);
    cache.insert("a", 1, 3);
    cache.insert("b", 2, 3);
    cache.insert("c", 3, 3);
    // Larger than the whole cache: not kept, and nothing else is pushed out.
    cache.insert("huge", 4, 11);
    QVERIFY(!cache.contains("huge"));
    QCOMPARE(cache.size(), std::size_t(3));
    // A smaller capacity keeps the most recent that fit.
    cache.setCapacity(6);
    QCOMPARE(cache.keys(), (std::vector<std::string>{ "c", "b" }));
    cache.setCapacity(0);
    QCOMPARE(cache.size(), std::size_t(0));
}

void TestCubeLodAuto::lru_blockKeysSeparateDatasetsAndLevels()
{
    LruCache<BlockKey, int> cache(100);
    const CubeLodGeometry::Dims cube{ 2048, 2048, 2048 };
    cache.insert(blockKey("cube-a", 256, Node{ 0, 1, 2, 3 }, cube), 1, 1);
    QVERIFY(cache.contains(blockKey("cube-a", 256, Node{ 0, 1, 2, 3 }, cube)));
    QVERIFY(!cache.contains(blockKey("cube-b", 256, Node{ 0, 1, 2, 3 }, cube)));
    QVERIFY(!cache.contains(blockKey("cube-a", 256, Node{ 1, 1, 2, 3 }, cube)));
    QVERIFY(!cache.contains(blockKey("cube-a", 128, Node{ 0, 1, 2, 3 }, cube)));
    QVERIFY(!cache.contains(blockKey("cube-a", 256, Node{ 0, 3, 2, 1 }, cube)));
    // A node cut to a chosen box is another block than the whole node; the
    // whole node keyed by its samples is the same one.
    const CubeLodGeometry::Box whole = CubeLodGeometry::nodeLevelBox(cube, 256, Node{ 0, 1, 2, 3 });
    QVERIFY(cache.contains(blockKey("cube-a", 256, Node{ 0, 1, 2, 3 }, whole)));
    CubeLodGeometry::Box part = whole;
    part.lo[0] += 10;
    QVERIFY(!cache.contains(blockKey("cube-a", 256, Node{ 0, 1, 2, 3 }, part)));
}

namespace {

/// A refined view along x: a coarse piece (spacing 1) owning x in
/// [-0.5, 3.5], and a fine block (spacing 0.5) owning [3.5, 7.5]; one voxel
/// thick in y and z. Each has one bright voxel.
std::vector<CubeLodPick::Piece> lineOfTwo(double coarseBright, double fineBright)
{
    CubeLodPick::Piece coarse;
    coarse.grid.extent[1] = 4;   // samples 0..4 (4 is a ghost)
    coarse.grid.extent[3] = 0;
    coarse.grid.extent[5] = 0;
    coarse.owned = { -0.5, 3.5, -0.5, 0.5, -0.5, 0.5 };
    coarse.sample = [coarseBright](int x, int, int) { return x == 2 ? coarseBright : 0.0; };

    CubeLodPick::Piece fine;
    fine.grid.extent[1] = 8;
    fine.grid.extent[3] = 1;
    fine.grid.extent[5] = 1;
    fine.grid.origin[0] = 3.25;   // sample k at 3.25 + 0.5 k
    fine.grid.origin[1] = -0.25;
    fine.grid.origin[2] = -0.25;
    for (double &sp : fine.grid.spacing)
        sp = 0.5;
    fine.owned = { 3.5, 7.5, -0.5, 0.5, -0.5, 0.5 };
    fine.sample = [fineBright](int x, int, int) { return x == 3 ? fineBright : 0.0; };   // world 4.75
    return { coarse, fine };
}

}   // namespace

void TestCubeLodAuto::pick_fineInsideTheBoxCoarseElsewhere()
{
    using C = RayVoxelPick::Criterion;
    const auto pieces = lineOfTwo(0.0, 10.0);
    // From +x, the fine block is met first, and its bright voxel is a fine one.
    const double a0[3] = { 9.0, 0.0, 0.0 }, a1[3] = { -1.0, 0.0, 0.0 };
    const CubeLodPick::Result r = CubeLodPick::along(pieces, a0, a1, C::FirstAboveThreshold, 1.0);
    QVERIFY(r.hit);
    QCOMPARE(r.piece, 1);
    QCOMPARE(r.voxel.voxel[0], 3);
    QCOMPARE(r.voxel.world[0], 4.75);

    // The same field brighter on the coarse side: a ray that only crosses the
    // coarse piece picks on the preview's grid.
    const auto other = lineOfTwo(5.0, 0.0);
    const double b0[3] = { -1.0, 0.0, 0.0 }, b1[3] = { 3.0, 0.0, 0.0 };
    const CubeLodPick::Result c = CubeLodPick::along(other, b0, b1, C::FirstAboveThreshold, 1.0);
    QVERIFY(c.hit);
    QCOMPARE(c.piece, 0);
    QCOMPARE(c.voxel.world[0], 2.0);

    // Nothing above the threshold anywhere: no hit.
    QVERIFY(!CubeLodPick::along(lineOfTwo(0.0, 0.0), a0, a1, C::FirstAboveThreshold, 1.0).hit);
    // The coarse piece's ghost sample (x = 4, under the fine block) is never
    // walked: the ray is cut at the piece's face.
    CubeLodPick::Piece ghostBright = lineOfTwo(0.0, 0.0)[0];
    ghostBright.sample = [](int x, int, int) { return x == 4 ? 100.0 : 0.0; };
    QVERIFY(!CubeLodPick::along({ ghostBright }, b0, a0, C::FirstAboveThreshold, 1.0).hit);
}

void TestCubeLodAuto::pick_firstAboveThresholdStopsAtTheFront()
{
    using C = RayVoxelPick::Criterion;
    const auto pieces = lineOfTwo(5.0, 10.0);
    const double l0[3] = { -1.0, 0.0, 0.0 }, l1[3] = { 9.0, 0.0, 0.0 };
    const double r0[3] = { 9.0, 0.0, 0.0 }, r1[3] = { -1.0, 0.0, 0.0 };
    // From -x the coarse voxel is in front; from +x the fine one.
    QCOMPARE(CubeLodPick::along(pieces, l0, l1, C::FirstAboveThreshold, 1.0).piece, 0);
    QCOMPARE(CubeLodPick::along(pieces, r0, r1, C::FirstAboveThreshold, 1.0).piece, 1);
}

void TestCubeLodAuto::pick_maximumAcrossPieces()
{
    using C = RayVoxelPick::Criterion;
    const auto pieces = lineOfTwo(5.0, 10.0);
    const double l0[3] = { -1.0, 0.0, 0.0 }, l1[3] = { 9.0, 0.0, 0.0 };
    const CubeLodPick::Result mx = CubeLodPick::along(pieces, l0, l1, C::Maximum, 0.0);
    QVERIFY(mx.hit);
    QCOMPARE(mx.piece, 1);
    QCOMPARE(mx.voxel.value, 10.0);
    // Blanks are skipped: with the fine voxel blank, the coarse one wins.
    const CubeLodPick::Result blanked = CubeLodPick::along(pieces, l0, l1, C::Maximum, 0.0,
                                                           [](double v) { return v > 9.0; });
    QCOMPARE(blanked.piece, 0);
    QCOMPARE(blanked.voxel.value, 5.0);
}

void TestCubeLodAuto::motion_framesTellMovesFromStills()
{
    // Every way the camera moves draws frames: a wheel step (one frame), a
    // drag (many), a Camera preset (one), with the same pose in between.
    MotionTracker m;
    CameraPose p;
    p.position = { 0, 0, 500 };
    p.focalPoint = { 128, 128, 128 };
    p.viewUp = { 0, 1, 0 };
    p.viewAngle = 30;
    QCOMPARE(m.frame(p), MotionTracker::Frame::First);   // a scene's first frame is no motion
    QVERIFY(!m.moving());
    QCOMPARE(m.frame(p), MotionTracker::Frame::Still);
    QVERIFY(!m.settle());                                // nothing moved: nothing ended
    // A wheel step: the camera dollies, one frame.
    p.position[2] = 450;
    QCOMPARE(m.frame(p), MotionTracker::Frame::Started);
    QVERIFY(m.moving());
    QCOMPARE(m.frame(p), MotionTracker::Frame::Still);   // a redraw (a block swap) is not a move
    QVERIFY(m.moving());
    QVERIFY(m.settle());                                 // the timer ran out: the zoom is over
    QVERIFY(!m.moving());
    // A drag: many frames, each a new pose.
    for (int i = 1; i <= 5; ++i) {
        p.viewUp = { std::sin(0.01 * i), std::cos(0.01 * i), 0 };
        QCOMPARE(m.frame(p), i == 1 ? MotionTracker::Frame::Started : MotionTracker::Frame::Moving);
    }
    QVERIFY(m.settle());
    // A preset that only changes the projection, or the zoom of a parallel one.
    p.parallel = true;
    QCOMPARE(m.frame(p), MotionTracker::Frame::Started);
    QVERIFY(m.settle());
    p.parallelScale = 64;
    QCOMPARE(m.frame(p), MotionTracker::Frame::Started);
    QVERIFY(m.settle());
    // A resized view, camera unchanged: it sees another part at another zoom.
    p.viewport = { 1200, 800 };
    QCOMPARE(m.frame(p), MotionTracker::Frame::Started);
    QVERIFY(m.settle());
    // A new scene starts over.
    m.reset();
    QCOMPARE(m.frame(p), MotionTracker::Frame::First);
}

void TestCubeLodAuto::settle_waitsWhileAButtonIsHeld()
{
    QCOMPARE(settleStep(true, true, 400), SettleStep::Wait);     // a drag paused, still held
    QCOMPARE(settleStep(true, false, 400), SettleStep::Plan);    // released, or a wheel zoom
    QCOMPARE(settleStep(false, false, 400), SettleStep::Plan);   // Clear Refined Box restarts the timer
    QCOMPARE(settleStep(false, true, 400), SettleStep::Plan);    // a click that moved nothing
    // A button the application still thinks is held long after the camera
    // stopped (a release lost to a menu): planned anyway.
    QCOMPARE(settleStep(true, true, kMaxHeldWaitMs), SettleStep::Plan);
    QCOMPARE(settleStep(true, true, kMaxHeldWaitMs - 1), SettleStep::Wait);
}

void TestCubeLodAuto::decide_explicitBoxSurvivesSettling()
{
    // A box chosen with Refine Selected Box, cut to the box, is shown: no
    // camera plan may replace it until it is cleared.
    CubeLodPlan::Inputs in = bigCube();
    in.visibleBox = CubeLodGeometry::wholeBox(in.cubeShape);
    in.focalPoint = { 1024.0, 1024.0, 1024.0 };
    in.voxelsPerPixel = 1.0;
    const CubeLodPlan::Plan chosen
            = CubeLodPlan::planBox(Box{ { 512, 512, 512 }, { 1536, 1536, 1536 } }, in);
    QVERIFY(chosen.refine);
    const CubeLodPlan::Plan view = CubeLodPlan::planFocus(in);
    QVERIFY(view.refine);
    State held;
    held.explicitHeld = true;
    QCOMPARE(decide(view, held), Action::Keep);
    CubeLodPlan::Plan none;
    QCOMPARE(decide(none, held), Action::Keep);
    // A box cut from a node is another set than the whole node.
    QVERIFY(signatureOf(chosen) != signatureOf(CubeLodPlan::planBox(Box{ { 0, 0, 0 }, { 2048, 2048, 2048 } }, in)));
}

#include "TestRayVoxelPick.h"

#include <QTest>

#include <array>
#include <cmath>
#include <vector>

void TestRayVoxelPick::init()
{ m_data.assign(static_cast<std::size_t>(kNx) * kNy * kNz, 0.); }

/// MIP shows the brightest sample along the ray, so a click must return it —
/// not the nearest voxel, and not the one on the displayed slice.
void TestRayVoxelPick::maximum_returnsTheBrightestVoxelAlongTheRay()
{
        set(10, 10, 7,  0.5);
        set(10, 10, 31, 2.0);   // the real peak, far along the ray
        set(10, 10, 12, 1.0);

        const double p0[3] = { 10., 10., -5. };    // looking down +z, i.e. down velocity
        const double p1[3] = { 10., 10., 50. };
        const auto r = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Maximum,
                                           0., sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[2], 31);
        QCOMPARE(r.value, 2.0);
    }

/// The whole point of the feature: the peak may lie in a channel far from
/// the one on the cutting plane. Picking must not silently answer with the
/// displayed channel.
void TestRayVoxelPick::maximum_findsAPeakThatIsNotOnTheDisplayedSlice()
{
        set(4, 6, 38, 3.0);
        const double p0[3] = { 4., 6., -5. };
        const double p1[3] = { 4., 6., 50. };
        const auto r = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Maximum,
                                           0., sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[0], 4);
        QCOMPARE(r.voxel[1], 6);
        QCOMPARE(r.voxel[2], 38);   // nowhere near a mid-cube slice
    }

void TestRayVoxelPick::minimum_returnsTheFaintestVoxel()
{
        set(3, 3, 5,  -1.0);
        set(3, 3, 20, -4.0);
        const double p0[3] = { 3., 3., -5. };
        const double p1[3] = { 3., 3., 50. };
        const auto r = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Minimum,
                                           0., sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[2], 20);
        QCOMPARE(r.value, -4.0);
    }

/// Composite shows the FRONT surface. A brighter voxel hidden behind it is
/// not what the user clicked, so it must not win.
void TestRayVoxelPick::composite_stopsAtTheFirstVoxelAboveThreshold()
{
        set(8, 8, 10, 1.5);   // the front surface
        set(8, 8, 30, 9.9);   // brighter, but behind it
        const double p0[3] = { 8., 8., -5. };
        const double p1[3] = { 8., 8., 50. };
        const auto r = RayVoxelPick::along(
                grid(), p0, p1, RayVoxelPick::Criterion::FirstAboveThreshold, 1.0, sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[2], 10);
        QCOMPARE(r.value, 1.5);
    }

/// Reverse the ray and the front surface is the other one — the criterion is
/// about the viewer's side, not about z increasing.
void TestRayVoxelPick::composite_frontSurfaceFollowsTheRayDirection()
{
        set(8, 8, 10, 1.5);
        set(8, 8, 30, 9.9);
        const double p0[3] = { 8., 8., 50. };   // camera on the far side now
        const double p1[3] = { 8., 8., -5. };
        const auto r = RayVoxelPick::along(
                grid(), p0, p1, RayVoxelPick::Criterion::FirstAboveThreshold, 1.0, sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[2], 30);
    }

void TestRayVoxelPick::composite_missesWhenNothingReachesTheThreshold()
{
        set(8, 8, 10, 0.2);
        const double p0[3] = { 8., 8., -5. };
        const double p1[3] = { 8., 8., 50. };
        const auto r = RayVoxelPick::along(
                grid(), p0, p1, RayVoxelPick::Criterion::FirstAboveThreshold, 1.0, sampler());
        QVERIFY(!r.hit);
    }

/// Side-on: the ray crosses SKY positions at roughly fixed velocity. The
/// answer is still the brightest sample, but now it identifies a position,
/// which is why the caller reports the channel back to the user.
void TestRayVoxelPick::maximum_worksOnASideOnRay()
{
        set(15, 9, 22, 5.0);
        const double p0[3] = { -5., 9., 22. };   // along +x at fixed y, z
        const double p1[3] = { 30., 9., 22. };
        const auto r = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Maximum,
                                           0., sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[0], 15);
        QCOMPARE(r.voxel[2], 22);
    }

/// A ray that never enters the cube must report a miss, not the nearest voxel.
void TestRayVoxelPick::missesTheCubeEntirely()
{
        set(10, 10, 10, 5.0);
        const double p0[3] = { 100., 100., -5. };
        const double p1[3] = { 100., 100., 50. };
        const auto r = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Maximum,
                                           0., sampler());
        QVERIFY(!r.hit);
    }

/// NaN blanks are skipped, not treated as a maximum or as zero.
void TestRayVoxelPick::ignoresNonFiniteSamples()
{
        for (int z = 0; z < kNz; ++z)
            set(2, 2, z, std::nan(""));
        set(2, 2, 17, 1.25);
        const double p0[3] = { 2., 2., -5. };
        const double p1[3] = { 2., 2., 50. };
        const auto r = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Maximum,
                                           0., sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[2], 17);
        QCOMPARE(r.value, 1.25);
    }

/// A one-voxel spike must not be stepped over — that is what fixes the
/// sampling rate at two steps per voxel.
void TestRayVoxelPick::doesNotStepOverASingleVoxelSpike()
{
        for (int z = 0; z < kNz; ++z) {
            const double p0[3] = { 6., 6., -5. };
            const double p1[3] = { 6., 6., 50. };
            init();
            set(6, 6, z, 7.0);
            const auto r = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Maximum,
                                               0., sampler());
            QVERIFY2(r.hit, qPrintable(QStringLiteral("missed the spike at z=%1").arg(z)));
            QCOMPARE(r.voxel[2], z);
        }
    }

/// The returned world position must map back to the voxel that was picked,
/// or the marker drawn in the 3-D view lands somewhere else.
void TestRayVoxelPick::worldPositionMatchesThePickedVoxel()
{
        RayVoxelPick::Grid g = grid();
        g.origin[0] = -10.; g.origin[1] = 5.; g.origin[2] = 100.;
        g.spacing[0] = 2.;  g.spacing[1] = 3.; g.spacing[2] = 0.5;
        set(7, 4, 13, 1.0);
        const double p0[3] = { g.origin[0] + 7 * 2., g.origin[1] + 4 * 3., g.origin[2] - 10. };
        const double p1[3] = { g.origin[0] + 7 * 2., g.origin[1] + 4 * 3., g.origin[2] + 50. };
        const auto r = RayVoxelPick::along(g, p0, p1, RayVoxelPick::Criterion::Maximum,
                                           0., sampler());
        QVERIFY(r.hit);
        QCOMPARE(r.voxel[2], 13);
        QCOMPARE(r.world[0], g.origin[0] + 7 * 2.);
        QCOMPARE(r.world[1], g.origin[1] + 4 * 3.);
        QCOMPARE(r.world[2], g.origin[2] + 13 * 0.5);
    }

void TestRayVoxelPick::rejectsADegenerateGrid()
{
        RayVoxelPick::Grid g = grid();
        g.spacing[1] = 0.;
        const double p0[3] = { 0., 0., 0. }, p1[3] = { 1., 1., 1. };
        QVERIFY(!RayVoxelPick::along(g, p0, p1, RayVoxelPick::Criterion::Maximum, 0.,
                                     sampler()).hit);
    }

void TestRayVoxelPick::visitsEveryVoxelOnALongAxis()
{
    // A 10000-channel axial ray: put the only signal at each index in turn and
    // require it to be found. A fixed-step march with a step cap misses these.
    RayVoxelPick::Grid g;
    g.extent[0] = 0; g.extent[1] = 0;
    g.extent[2] = 0; g.extent[3] = 0;
    g.extent[4] = 0; g.extent[5] = 9999;
    std::vector<double> col(10000, 0.);
    auto s = [&col](int, int, int z) {
        return (z >= 0 && z < 10000) ? col[static_cast<std::size_t>(z)] : std::nan("");
    };
    const double p0[3] = { 0., 0., -50. };
    const double p1[3] = { 0., 0., 10050. };
    for (int z : { 0, 1, 3, 7, 4095, 4096, 4097, 9998, 9999 }) {
        std::fill(col.begin(), col.end(), 0.);
        col[static_cast<std::size_t>(z)] = 1.0;
        const auto r = RayVoxelPick::along(g, p0, p1, RayVoxelPick::Criterion::Maximum, 0., s);
        QVERIFY2(r.hit, qPrintable(QStringLiteral("no hit at z=%1").arg(z)));
        QCOMPARE(r.voxel[2], z);
    }
}

void TestRayVoxelPick::handlesAnExtentThatDoesNotStartAtZero()
{
    RayVoxelPick::Grid g;
    g.extent[0] = 10; g.extent[1] = 19;
    g.extent[2] = 10; g.extent[3] = 19;
    g.extent[4] = 100; g.extent[5] = 109;
    g.origin[0] = g.origin[1] = g.origin[2] = 0.;
    g.spacing[0] = g.spacing[1] = g.spacing[2] = 1.;
    // world = origin + index*spacing, so voxel (14, 13, 105) is at (14, 13, 105).
    auto s = [](int x, int y, int z) {
        return (x == 14 && y == 13 && z == 105) ? 5.0 : 0.0;
    };
    const double p0[3] = { 14., 13., 90. };
    const double p1[3] = { 14., 13., 120. };
    const auto r = RayVoxelPick::along(g, p0, p1, RayVoxelPick::Criterion::Maximum, 0., s);
    QVERIFY(r.hit);
    QCOMPARE(r.voxel[0], 14);
    QCOMPARE(r.voxel[1], 13);
    QCOMPARE(r.voxel[2], 105);
    QCOMPARE(r.world[2], 105.);
}

void TestRayVoxelPick::blankTestExcludesTheFiniteSentinel()
{
    // The cube viewer rewrites NaN to rangeMin - 0.01*range BEFORE this code
    // sees it, so the sentinel is finite and, for MinIP, the smallest thing on
    // the ray. Without the blank test the pick is always a blank.
    const double sentinel = -99.0;
    set(5, 5, 8, sentinel);
    set(5, 5, 12, -2.0);   // the real minimum among actual data
    const double p0[3] = { 5., 5., -5. };
    const double p1[3] = { 5., 5., 50. };

    const auto naive = RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::Minimum,
                                           0., sampler());
    QVERIFY(naive.hit);
    QCOMPARE(naive.voxel[2], 8);   // picks the blank — the defect being guarded

    const auto guarded = RayVoxelPick::along(
            grid(), p0, p1, RayVoxelPick::Criterion::Minimum, 0., sampler(),
            [sentinel](double v) { return v <= sentinel; });
    QVERIFY(guarded.hit);
    QCOMPARE(guarded.voxel[2], 12);
}

void TestRayVoxelPick::obliqueRayVisitsAContiguousChain()
{
    // Record the walk on a diagonal and require each step to move by exactly one
    // along exactly one axis — the definition of visiting every crossed voxel.
    std::vector<std::array<int, 3>> seen;
    auto s = [&seen](int x, int y, int z) {
        seen.push_back({ x, y, z });
        return 0.0;   // nothing above threshold: force the full traversal
    };
    const double p0[3] = { -5., -5., -5. };
    const double p1[3] = { 25., 25., 45. };
    RayVoxelPick::along(grid(), p0, p1, RayVoxelPick::Criterion::FirstAboveThreshold,
                        1e9, s);
    QVERIFY2(seen.size() > 10, "the diagonal should cross many voxels");
    for (std::size_t i = 1; i < seen.size(); ++i) {
        int moved = 0;
        for (int a = 0; a < 3; ++a) {
            const int dd = std::abs(seen[i][a] - seen[i - 1][a]);
            QVERIFY2(dd <= 1, "a step skipped a voxel");
            if (dd)
                ++moved;
        }
        QVERIFY2(moved >= 1, "a step went nowhere");
    }
    // A perfect diagonal crosses corners, so the three axes advance TOGETHER. A
    // walk that stepped one axis at a time would visit about three times as many
    // cells, and the extra ones are touched only at a point.
    QVERIFY2(seen.size() < 1.5 * kNz,
             "the diagonal visited corner-only cells: axes are not advancing together");
}

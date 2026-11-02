// test_region_full_res.cpp
// See TestRegionFullRes.h.

#include "TestRegionFullRes.h"

#include "RegionFullRes.h"

#include <QJsonArray>
#include <QTest>

#include <cmath>

using visivo::DisplayGrid;
using visivo::regionInSourcePixels;

void TestRegionFullRes::atFullResolutionTheRegionIsUnchanged()
{
    const DisplayGrid g;   // origin 0, spacing 1
    const auto box = regionInSourcePixels(u"box"_qs, g, { 10, 20 }, { 4, 30 }, {}, 0);
    QCOMPARE(box[u"frame"_qs].toString(), u"image"_qs);
    QCOMPARE(box[u"center"_qs].toArray()[0].toDouble(), 7.0);
    QCOMPARE(box[u"center"_qs].toArray()[1].toDouble(), 25.0);
    // The canonical box |x - c| <= w/2 then holds pixels 4..10 and 20..30.
    QCOMPARE(box[u"size"_qs].toArray()[0].toDouble(), 6.0);
    QCOMPARE(box[u"size"_qs].toArray()[1].toDouble(), 10.0);
}

void TestRegionFullRes::aBoxCoversWholeBlocks()
{
    // Averaged level 3: voxel v is source pixels 8v..8v+7, centred at 8v+3.5.
    const DisplayGrid g{ 3.5, 3.5, 8.0, 8.0, 0, 0 };
    const auto box = regionInSourcePixels(u"box"_qs, g, { 2, 1 }, { 4, 1 }, {}, 0);
    const double cx = box[u"center"_qs].toArray()[0].toDouble();
    const double w = box[u"size"_qs].toArray()[0].toDouble();
    const double h = box[u"size"_qs].toArray()[1].toDouble();
    // Voxels 2..4 are source pixels 16..39; row 1 is 8..15.
    QVERIFY(cx - w / 2 <= 16.0 && cx - w / 2 > 15.0);
    QVERIFY(cx + w / 2 >= 39.0 && cx + w / 2 < 40.0);
    QVERIFY(h >= 7.0 && h < 8.0);

    // Sampled level 2 (step 4, voxel v at source pixel 4v): voxels 0..2 cover
    // -2 .. 10 on screen, the near edge in and the far edge out.
    const DisplayGrid sampled{ 0.0, 0.0, 4.0, 4.0, 0, 0 };
    const auto s = regionInSourcePixels(u"box"_qs, sampled, { 0, 0 }, { 2, 0 }, {}, 0);
    const double scx = s[u"center"_qs].toArray()[0].toDouble();
    const double sw = s[u"size"_qs].toArray()[0].toDouble();
    QVERIFY(std::abs((scx - sw / 2) - (-2.0)) < 1e-9);
    QVERIFY(scx + sw / 2 < 10.0 && scx + sw / 2 > 9.999);
}

void TestRegionFullRes::circlesAndAnnuliScaleTheirRadii()
{
    const DisplayGrid g{ 1.5, 1.5, 4.0, 4.0, 0, 0 };
    const auto circle = regionInSourcePixels(u"circle"_qs, g, { 10, 10 }, { 13, 14 }, {}, 0);
    QCOMPARE(circle[u"center"_qs].toArray()[0].toDouble(), 41.5);
    QCOMPARE(circle[u"radius"_qs].toDouble(), 20.0);   // 5 voxels of 4 pixels
    const auto ring = regionInSourcePixels(u"annulus"_qs, g, { 10, 10 }, { 13, 14 }, {}, 2.0);
    QCOMPARE(ring[u"radii"_qs].toArray()[0].toDouble(), 8.0);
    QCOMPARE(ring[u"radii"_qs].toArray()[1].toDouble(), 20.0);
}

void TestRegionFullRes::polygonVerticesGoToBlockCentres()
{
    const DisplayGrid g{ 0.5, 0.5, 2.0, 2.0, 0, 0 };
    const auto poly = regionInSourcePixels(u"polygon"_qs, g, {}, {},
                                           { { 0, 0 }, { 5, 0 }, { 5, 3 } }, 0);
    const QJsonArray points = poly[u"points"_qs].toArray();
    QCOMPARE(points.size(), 3);
    QCOMPARE(points[1].toArray()[0].toDouble(), 10.5);
    QCOMPARE(points[2].toArray()[1].toDouble(), 6.5);
    QVERIFY(regionInSourcePixels(u"star"_qs, g, {}, {}, {}, 0).isEmpty());
}

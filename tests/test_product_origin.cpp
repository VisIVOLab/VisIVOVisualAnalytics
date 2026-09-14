// test_product_origin.cpp
// A product records what a tool measured; this record says WHERE it measured it,
// so opening the product's Session Data row can put the overlay back. The
// encoding is pinned here because a wrong shape compiles perfectly and fails
// silently at the point where the user is trying to understand a number.

#include "TestProductOrigin.h"

#include "ProductOrigin.h"

#include <QTest>

namespace {
// Mirrors both viewers' RegionMode: None, Box, Circle, Polygon, Annulus.
constexpr int kBox = 1, kCircle = 2, kPolygon = 3, kAnnulus = 4;
}   // namespace

void TestProductOrigin::boxAndCircle_roundTripTheirTwoCorners()
{
    for (int mode : { kBox, kCircle }) {
        const QVariantMap o = visivo::encodeRegionOrigin(mode, { 10, 20 }, { 30, 45 }, {}, 0.0);
        QCOMPARE(visivo::originKind(o), QStringLiteral("region"));
        int a[2] = { 0, 0 }, c[2] = { 0, 0 };
        QVERIFY(visivo::originPoint(o, QStringLiteral("anchor"), a, 2));
        QVERIFY(visivo::originPoint(o, QStringLiteral("current"), c, 2));
        QCOMPARE(a[0], 10);
        QCOMPARE(a[1], 20);
        QCOMPARE(c[0], 30);
        QCOMPARE(c[1], 45);
    }
}

void TestProductOrigin::annulus_roundTripsItsInnerRadius()
{
    const QVariantMap o = visivo::encodeRegionOrigin(kAnnulus, { 5, 6 }, { 25, 6 }, {}, 7.5);
    QCOMPARE(visivo::originShape(o), QStringLiteral("annulus"));
    QCOMPARE(visivo::originInnerRadius(o), 7.5);
    // The other shapes carry no radius, and reading one back gives 0 rather
    // than a stale value.
    const QVariantMap box = visivo::encodeRegionOrigin(kBox, { 0, 0 }, { 1, 1 }, {}, 9.0);
    QCOMPARE(visivo::originInnerRadius(box), 0.0);
}

void TestProductOrigin::polygon_keepsEveryVertexAsAPair()
{
    // The same QList::append trap that flattened the result-record rows would
    // flatten these into loose integers.
    const std::vector<std::array<int, 2>> verts{ { 1, 2 }, { 3, 4 }, { 5, 6 } };
    const QVariantMap o = visivo::encodeRegionOrigin(kPolygon, {}, {}, verts, 0.0);
    QCOMPARE(o.value(QStringLiteral("points")).toList().size(), 3);
    const auto back = visivo::originPolygon(o);
    QCOMPARE(back.size(), std::size_t(3));
    QCOMPARE(back.at(1)[0], 3);
    QCOMPARE(back.at(2)[1], 6);
}

void TestProductOrigin::probe_roundTripsItsVoxel()
{
    const QVariantMap o = visivo::encodeProbeOrigin({ 98, 140, 53 });
    QCOMPARE(visivo::originKind(o), QStringLiteral("probe"));
    int v[3] = { 0, 0, 0 };
    QVERIFY(visivo::originPoint(o, QStringLiteral("voxel"), v, 3));
    QCOMPARE(v[0], 98);
    QCOMPARE(v[1], 140);
    QCOMPARE(v[2], 53);
}

void TestProductOrigin::pvPath_roundTripsItsPointsAndWidth()
{
    const QVariantMap o = visivo::encodePvPathOrigin({ { 10, 10 }, { 50, 60 }, { 90, 60 } }, 5);
    QCOMPARE(visivo::originKind(o), QStringLiteral("pvpath"));
    QCOMPARE(visivo::originWidth(o), 5);
    const auto pts = visivo::originPolygon(o);
    QCOMPARE(pts.size(), std::size_t(3));
    QCOMPARE(pts.at(2)[0], 90);
    // Not a region: restoring it as one would close the path into a shape that
    // was never drawn.
    QVERIFY(visivo::originKind(o) != QStringLiteral("region"));
    QCOMPARE(visivo::describeOrigin(o), QStringLiteral("a 3-point PV path, 5 px wide"));
}

void TestProductOrigin::pvPath_needsTwoPointsToBeAPath()
{
    QVERIFY(visivo::encodePvPathOrigin({}, 1).isEmpty());
    QVERIFY(visivo::encodePvPathOrigin({ { 1, 1 } }, 1).isEmpty());
    QVERIFY(!visivo::encodePvPathOrigin({ { 1, 1 }, { 2, 2 } }, 1).isEmpty());
}

void TestProductOrigin::unknownMode_encodesToNothing()
{
    // RegionMode::None, or a future shape this build does not know: an empty
    // record means "no origin recorded", which callers treat as "nothing to
    // restore" rather than restoring a default region at (0,0).
    QVERIFY(visivo::encodeRegionOrigin(0, { 1, 1 }, { 2, 2 }, {}, 0.0).isEmpty());
    QVERIFY(visivo::encodeRegionOrigin(99, { 1, 1 }, { 2, 2 }, {}, 0.0).isEmpty());
}

void TestProductOrigin::malformedRecord_refusesRatherThanRestoringHalfAGeometry()
{
    QVariantMap o{ { QStringLiteral("kind"), QStringLiteral("region") },
                   { QStringLiteral("shape"), QStringLiteral("box") },
                   { QStringLiteral("anchor"), QVariant(QVariantList{ 5 }) } };   // one value
    int a[2] = { -1, -1 };
    QVERIFY(!visivo::originPoint(o, QStringLiteral("anchor"), a, 2));
    QVERIFY(!visivo::originPoint(o, QStringLiteral("current"), a, 2));   // absent
    o.insert(QStringLiteral("anchor"),
             QVariant(QVariantList{ QStringLiteral("x"), QStringLiteral("y") }));
    QVERIFY(!visivo::originPoint(o, QStringLiteral("anchor"), a, 2));    // not numbers
}

void TestProductOrigin::voxelFrame_roundTripsAndRejectsAMalformedStamp()
{
    QVariantMap o = visivo::encodeRegionOrigin(kBox, { 4, 5 }, { 9, 11 }, {}, 0.0);
    o = visivo::withVoxelFrame(o, 2.0, 2.0, -1.0, -1.0);
    double scale[2] = { 0, 0 }, pos[2] = { 0, 0 };
    QVERIFY(visivo::originFrame(o, scale, pos));
    QCOMPARE(scale[0], 2.0);
    QCOMPARE(pos[1], -1.0);

    // A zero scale is not a usable mapping: reading it back must fail rather
    // than hand out a divisor of zero.
    QVariantMap dead = visivo::withVoxelFrame(o, 0.0, 2.0, 0.0, 0.0);
    QVERIFY(!visivo::originFrame(dead, scale, pos));
    QVariantMap truncated = o;
    truncated.insert(QStringLiteral("frame"), QVariantList{ 1.0, 2.0 });
    QVERIFY(!visivo::originFrame(truncated, scale, pos));
}

void TestProductOrigin::remap_movesAGeometryOntoAnotherLodGrid()
{
    // Recorded on a decimated image (spacing 2, first pixel at world 0), read
    // back on the full-resolution one (spacing 1): a box at voxel 10 sits at
    // world 20, which is voxel 20 of the finer grid.
    QVariantMap o = visivo::encodeRegionOrigin(kBox, { 10, 20 }, { 30, 40 }, {}, 0.0);
    o = visivo::withVoxelFrame(o, 2.0, 2.0, 0.0, 0.0);
    const double scale[2] = { 1.0, 1.0 }, pos[2] = { 0.0, 0.0 };
    int a[2] = { 10, 20 };
    visivo::remapOriginVoxel(o, scale, pos, a);
    QCOMPARE(a[0], 20);
    QCOMPARE(a[1], 40);
}

void TestProductOrigin::remap_leavesTheGeometryAloneWithoutAStamp()
{
    // Products recorded before the stamp existed, and grids that did not change,
    // must both come back untouched - a silent shift would be worse than no
    // restore at all.
    const QVariantMap old = visivo::encodeRegionOrigin(kBox, { 7, 8 }, { 9, 10 }, {}, 0.0);
    const double scale[2] = { 2.0, 2.0 }, pos[2] = { 5.0, 5.0 };
    int a[2] = { 7, 8 };
    visivo::remapOriginVoxel(old, scale, pos, a);
    QCOMPARE(a[0], 7);
    QCOMPARE(a[1], 8);

    const QVariantMap same = visivo::withVoxelFrame(old, 2.0, 2.0, 5.0, 5.0);
    int b[2] = { 7, 8 };
    visivo::remapOriginVoxel(same, scale, pos, b);
    QCOMPARE(b[0], 7);
}

void TestProductOrigin::measuredSurface_roundTripsAndReadsInTheDescription()
{
    QVariantMap slice = visivo::encodeRegionOrigin(kBox, { 1, 2 }, { 3, 4 }, {}, 0.0);
    slice = visivo::withMeasuredSurface(slice, QStringLiteral("slice"), 9);
    QCOMPARE(visivo::originSurface(slice), QStringLiteral("slice"));
    QCOMPARE(visivo::originChannel(slice), 9);
    // Channels read 1-based to the user, as everywhere else in the app.
    QVERIFY(visivo::describeOrigin(slice).contains(QStringLiteral("channel 10")));

    QVariantMap moment = visivo::encodeRegionOrigin(kCircle, { 1, 2 }, { 3, 4 }, {}, 0.0);
    moment = visivo::withMeasuredSurface(moment, QStringLiteral("moment"), -1);
    QCOMPARE(visivo::originChannel(moment), -1);
    QVERIFY(visivo::describeOrigin(moment).contains(QStringLiteral("moment map")));

    // A product recorded before the stamp says nothing rather than guessing a
    // channel that would send the viewer somewhere the numbers never came from.
    const QVariantMap old = visivo::encodeRegionOrigin(kBox, { 1, 2 }, { 3, 4 }, {}, 0.0);
    QCOMPARE(visivo::originChannel(old), -1);
    QVERIFY(visivo::originSurface(old).isEmpty());
    QVERIFY(!visivo::describeOrigin(old).contains(QStringLiteral("channel")));
}

void TestProductOrigin::describeOrigin_readsAsASentence()
{
    QCOMPARE(visivo::describeOrigin(visivo::encodeProbeOrigin({ 98, 140, 53 })),
             QStringLiteral("probed at pixel (98, 140), channel 54"));   // 1-based channel
    QCOMPARE(visivo::describeOrigin(
                     visivo::encodeRegionOrigin(kBox, { 10, 20 }, { 30, 45 }, {}, 0.0)),
             QStringLiteral("a box region at (10, 20)"));
    QCOMPARE(visivo::describeOrigin(visivo::encodeRegionOrigin(
                     kPolygon, {}, {}, { { 1, 2 }, { 3, 4 }, { 5, 6 } }, 0.0)),
             QStringLiteral("a polygon of 3 points"));
    QVERIFY(visivo::describeOrigin(QVariantMap{}).isEmpty());
}

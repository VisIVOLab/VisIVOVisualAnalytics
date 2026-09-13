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

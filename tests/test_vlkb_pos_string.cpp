// test_vlkb_pos_string.cpp
// See TestVlkbPosString.h.

#include "TestVlkbPosString.h"

#include "VlkbPosString.h"

#include <QTest>

using namespace visivo;

void TestVlkbPosString::aNegativeLongitudeWrapsByAFullTurnNotAHalf()
{
    // The bug this file exists for: l = −2° was turned into 178°, which is the
    // opposite side of the Galaxy, so a query at the Galactic centre returned
    // datasets from nowhere near it.
    QCOMPARE(normalisedGalacticLongitude(-2.0), 358.0);
    QCOMPARE(normalisedGalacticLongitude(-0.5), 359.5);
    QCOMPARE(normalisedGalacticLongitude(-360.0), 0.0);
    QCOMPARE(normalisedGalacticLongitude(-370.0), 350.0);
}

void TestVlkbPosString::aLongitudePastThreeSixtyComesBackRound()
{
    QCOMPARE(normalisedGalacticLongitude(362.0), 2.0);
    QCOMPARE(normalisedGalacticLongitude(720.0), 0.0);
    QCOMPARE(normalisedGalacticLongitude(23.1498), 23.1498);
}

void TestVlkbPosString::latitudeIsClampedNotWrapped()
{
    // Latitude is not periodic: folding 95° to −85° would move the region to
    // the other side of the pole without saying so.
    QCOMPARE(clampedGalacticLatitude(95.0), 90.0);
    QCOMPARE(clampedGalacticLatitude(-95.0), -90.0);
    QCOMPARE(clampedGalacticLatitude(0.0566), 0.0566);
}

void TestVlkbPosString::aCircleCarriesItsRadiusAsAPositiveNumber()
{
    QCOMPARE(circlePosString(-2.0, 0.5, 0.25), QStringLiteral("CIRCLE 358.000000 0.500000 0.250000"));
    // A negative radius is a typo, not a mirrored cone.
    QCOMPARE(circlePosString(10.0, 0.0, -0.25), QStringLiteral("CIRCLE 10.000000 0.000000 0.250000"));
}

void TestVlkbPosString::aBoxAwayFromZeroIsAnOrdinaryInterval()
{
    const QString pos = rangePosString(23.1498, 0.0566, 1.36535, 0.512);
    QCOMPARE(pos, QStringLiteral("RANGE 21.784450 24.515150 -0.455400 0.568600"));
    QVERIFY(!rangeWrapsThroughZero(23.1498, 1.36535));
}

void TestVlkbPosString::aBoxAcrossZeroIsEmittedWrappedAndFlagged()
{
    // l = 1° ± 2° spans 359°…3°. Emitting 359 → 3 is the RANGE convention for a
    // wrap; "fixing" it to 3 → 359 would select the 356° of sky NOT asked for.
    const QString pos = rangePosString(1.0, 0.0, 2.0, 0.5);
    QCOMPARE(pos, QStringLiteral("RANGE 359.000000 3.000000 -0.500000 0.500000"));
    QVERIFY(rangeWrapsThroughZero(1.0, 2.0));
}

void TestVlkbPosString::nonFiniteInputDoesNotProduceGarbage()
{
    QCOMPARE(normalisedGalacticLongitude(std::numeric_limits<double>::quiet_NaN()), 0.0);
    QCOMPARE(clampedGalacticLatitude(std::numeric_limits<double>::infinity()), 0.0);
    QVERIFY(!circlePosString(std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0).contains(
            QStringLiteral("nan"), Qt::CaseInsensitive));
}

void TestVlkbPosString::coordinatesKeepEnoughPrecisionForASmallBox()
{
    // QString::number's default 6 SIGNIFICANT digits gave 0.001° at l = 123.456,
    // which is 3.6 arcsec — coarser than the cutouts being requested. Six
    // DECIMALS is 3.6 mas.
    const QString pos = rangePosString(123.456789, 0.000123, 0.001, 0.001);
    QVERIFY(pos.contains(QStringLiteral("123.455789")));
    QVERIFY(pos.contains(QStringLiteral("123.457789")));
}

void TestVlkbPosString::aFullCircleBoxCoversEveryLongitude()
{
    // Normalising the two edges of a 360°-wide box independently gives them the
    // same value — RANGE 0 0 — which reads as a box of zero width: the exact
    // opposite of what was asked for.
    QCOMPARE(rangePosString(180.0, 0.0, 180.0, 1.0),
             QStringLiteral("RANGE 0.000000 360.000000 -1.000000 1.000000"));
    QVERIFY(!rangeWrapsThroughZero(180.0, 180.0));
    // Wider than the whole circle is still the whole circle.
    QCOMPARE(rangePosString(0.0, 0.0, 400.0, 2.0),
             QStringLiteral("RANGE 0.000000 360.000000 -2.000000 2.000000"));
}

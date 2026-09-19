// test_sky_overlap.cpp
// See TestSkyOverlap.h.

#include "TestSkyOverlap.h"

#include "SkyOverlap.h"

#include <QTest>

#include <limits>

using SkyOverlap::Bounds;
using SkyOverlap::overlaps;

void TestSkyOverlap::twoImagesOfTheSameFieldOverlap()
{
    // Two Hi-GAL tiles of the same region, offset by half a tile.
    const Bounds a{ 275.0, 276.0, -16.5, -15.5 };
    const Bounds b{ 275.5, 276.5, -16.2, -15.2 };
    QVERIFY(overlaps(a, b));
    QVERIFY(overlaps(b, a));
}

void TestSkyOverlap::adjacentFieldsDoNotOverlap()
{
    // The case the user hit: a layer from a different part of the plane loads,
    // is placed correctly by its WCS — off the edge of the master — and is
    // therefore invisible.
    const Bounds a{ 275.0, 276.0, -16.5, -15.5 };
    const Bounds b{ 280.0, 281.0, -16.5, -15.5 };
    QVERIFY(!overlaps(a, b));
}

void TestSkyOverlap::touchingEdgesAreNotAnOverlap()
{
    // Sharing one edge means sharing no pixels.
    const Bounds a{ 10.0, 11.0, 0.0, 1.0 };
    const Bounds b{ 11.0, 12.0, 0.0, 1.0 };
    QVERIFY(!overlaps(a, b));
}

void TestSkyOverlap::aContainedFieldOverlaps()
{
    const Bounds big{ 10.0, 20.0, -5.0, 5.0 };
    const Bounds small{ 14.0, 15.0, -1.0, 1.0 };
    QVERIFY(overlaps(big, small));
    QVERIFY(overlaps(small, big));
}

void TestSkyOverlap::declinationAloneIsNotEnough()
{
    // Same declination band, opposite sides of the sky.
    const Bounds a{ 10.0, 11.0, -1.0, 1.0 };
    const Bounds b{ 190.0, 191.0, -1.0, 1.0 };
    QVERIFY(!overlaps(a, b));
    // …and the mirror case: same RA, different declination.
    QVERIFY(!overlaps(Bounds{ 10.0, 11.0, 40.0, 41.0 }, Bounds{ 10.0, 11.0, -41.0, -40.0 }));
}

void TestSkyOverlap::aFieldStraddlingRaZeroOverlapsItsNeighbourOnEitherSide()
{
    // wcsrange() reports a wrapping field as ra_min > ra_max. Comparing the two
    // numbers as an ordinary interval says "no overlap" for two images that
    // plainly share sky — which would refuse a perfectly good layer.
    const Bounds wrapped{ 359.0, 1.0, -1.0, 1.0 };
    QVERIFY(overlaps(wrapped, Bounds{ 359.5, 359.8, -0.5, 0.5 }));   // before zero
    QVERIFY(overlaps(wrapped, Bounds{ 0.2, 0.5, -0.5, 0.5 }));       // after zero
    QVERIFY(overlaps(Bounds{ 0.2, 0.5, -0.5, 0.5 }, wrapped));       // order is irrelevant
}

void TestSkyOverlap::twoFieldsStraddlingRaZeroOverlapEachOther()
{
    // Both contain RA = 0, so they cannot miss each other in RA.
    QVERIFY(overlaps(Bounds{ 359.0, 1.0, -1.0, 1.0 }, Bounds{ 350.0, 5.0, -0.5, 0.5 }));
}

void TestSkyOverlap::aWrappedFieldStillMissesAFarAwayOne()
{
    // Wrapping must not become "overlaps everything".
    QVERIFY(!overlaps(Bounds{ 359.0, 1.0, -1.0, 1.0 }, Bounds{ 180.0, 181.0, -1.0, 1.0 }));
}

void TestSkyOverlap::invertedDeclinationBoundsAreReadAsARange()
{
    // A negative CDELT2 can hand back dec_min > dec_max; the range is the same.
    QVERIFY(overlaps(Bounds{ 10.0, 11.0, 1.0, -1.0 }, Bounds{ 10.5, 10.6, -0.5, 0.5 }));
}

void TestSkyOverlap::boundsWithoutAWcsSolutionRefuse()
{
    // A simulation in pixel coordinates has no sky position: placing it by
    // guesswork is worse than refusing it.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!overlaps(Bounds{ nan, nan, nan, nan }, Bounds{ 10.0, 11.0, 0.0, 1.0 }));
    QVERIFY(!overlaps(Bounds{ 10.0, 11.0, 0.0, 1.0 }, Bounds{ nan, nan, nan, nan }));
}

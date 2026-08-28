// test_cube_region_geom.cpp
// Unit tests for CubeRegionGeom's pure ROI hit-testers, made inline in the
// header (E-04) so they can be tested without linking VTK. These drive region
// selection in both the cube and image viewers and were previously untested.

#include "TestCubeRegionGeom.h"
#include "CubeRegionGeom.h"

#include <QTest>

using namespace CubeRegionGeom;

namespace {
using P = std::array<int, 2>;
using Poly = std::vector<std::array<int, 2>>;
}  // namespace

// ── pointInBox ───────────────────────────────────────────────────────────────
void TestCubeRegionGeom::box_insideEdgesInclusive()
{
    const P a{ 0, 0 }, c{ 10, 6 };
    QVERIFY(pointInBox(a, c, 5, 3));     // interior
    QVERIFY(pointInBox(a, c, 0, 0));     // corner (inclusive)
    QVERIFY(pointInBox(a, c, 10, 6));    // opposite corner
    QVERIFY(pointInBox(a, c, 0, 3));     // on an edge
}

void TestCubeRegionGeom::box_outside()
{
    const P a{ 0, 0 }, c{ 10, 6 };
    QVERIFY(!pointInBox(a, c, 11, 3));
    QVERIFY(!pointInBox(a, c, 5, 7));
    QVERIFY(!pointInBox(a, c, -1, -1));
}

void TestCubeRegionGeom::box_anchorCurrentOrderIndependent()
{
    // The box normalises min/max, so swapping anchor/current is identical.
    const P a{ 10, 6 }, c{ 0, 0 };
    QVERIFY(pointInBox(a, c, 5, 3));
    QVERIFY(!pointInBox(a, c, 11, 3));
}

// ── pointInCircle ────────────────────────────────────────────────────────────
void TestCubeRegionGeom::circle_insideAndBoundary()
{
    const P anchor{ 0, 0 }, current{ 5, 0 };   // radius = 5
    QVERIFY(pointInCircle(anchor, current, 0, 0));    // centre
    QVERIFY(pointInCircle(anchor, current, 3, 4));    // on the circle (3-4-5)
    QVERIFY(pointInCircle(anchor, current, 2, 2));    // interior
}

void TestCubeRegionGeom::circle_outside()
{
    const P anchor{ 0, 0 }, current{ 5, 0 };   // radius = 5
    QVERIFY(!pointInCircle(anchor, current, 4, 4));   // dist ~5.66 > 5
    QVERIFY(!pointInCircle(anchor, current, 6, 0));
}

void TestCubeRegionGeom::circle_degenerateRadiusOnlyCentre()
{
    const P anchor{ 3, 3 }, current{ 3, 3 };   // radius 0
    QVERIFY(pointInCircle(anchor, current, 3, 3));
    QVERIFY(!pointInCircle(anchor, current, 4, 3));
}

// ── pointInPolygon ───────────────────────────────────────────────────────────
void TestCubeRegionGeom::polygon_insideSquare()
{
    const Poly sq{ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
    QVERIFY(pointInPolygon(sq, 5, 5));
    QVERIFY(pointInPolygon(sq, 1, 1));
    QVERIFY(pointInPolygon(sq, 8, 2));
}

void TestCubeRegionGeom::polygon_outsideSquare()
{
    const Poly sq{ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
    QVERIFY(!pointInPolygon(sq, 15, 5));
    QVERIFY(!pointInPolygon(sq, 5, -3));
    QVERIFY(!pointInPolygon(sq, -1, -1));
}

void TestCubeRegionGeom::polygon_fewerThan3VerticesIsFalse()
{
    QVERIFY(!pointInPolygon(Poly{}, 0, 0));
    QVERIFY(!pointInPolygon(Poly{ { 0, 0 } }, 0, 0));
    QVERIFY(!pointInPolygon(Poly{ { 0, 0 }, { 5, 5 } }, 2, 2));
}

void TestCubeRegionGeom::polygon_pixelCentreSampling()
{
    // Right triangle (0,0)-(10,0)-(0,10). Sampled at the pixel centre (+0.5), the
    // hypotenuse is x+y = 11, so (4,4)→sum 9 is inside and (6,6)→sum 13 is out.
    const Poly tri{ { 0, 0 }, { 10, 0 }, { 0, 10 } };
    QVERIFY(pointInPolygon(tri, 4, 4));
    QVERIFY(!pointInPolygon(tri, 6, 6));
    QVERIFY(pointInPolygon(tri, 1, 1));
    QVERIFY(!pointInPolygon(tri, 9, 9));
}

void TestCubeRegionGeom::polygon_horizontalEdgeNoCrash()
{
    // The square has horizontal top/bottom edges (yj == yi), which the impl guards
    // against a divide-by-zero. Must return correct results, not NaN/hang.
    const Poly sq{ { 0, 0 }, { 6, 0 }, { 6, 6 }, { 0, 6 } };
    QVERIFY(pointInPolygon(sq, 3, 3));
    QVERIFY(!pointInPolygon(sq, 3, 9));
}

// ── pointInAnnulus ───────────────────────────────────────────────────────────
void TestCubeRegionGeom::annulus_insideRing()
{
    // centre (0,0), outer point (10,0) → outer radius 10; inner radius 4.
    const P centre{ 0, 0 }, outer{ 10, 0 };
    QVERIFY(pointInAnnulus(centre, outer, 4.0, 5, 0));    // r=5 within [4,10]
    QVERIFY(pointInAnnulus(centre, outer, 4.0, 0, 8));    // r=8
    QVERIFY(pointInAnnulus(centre, outer, 4.0, 4, 0));    // r=4 == inner (inclusive)
    QVERIFY(pointInAnnulus(centre, outer, 4.0, 10, 0));   // r=10 == outer (inclusive)
}

void TestCubeRegionGeom::annulus_inHoleIsFalse()
{
    const P centre{ 0, 0 }, outer{ 10, 0 };
    QVERIFY(!pointInAnnulus(centre, outer, 4.0, 0, 0));   // centre, r=0 < inner
    QVERIFY(!pointInAnnulus(centre, outer, 4.0, 2, 0));   // r=2 < inner 4
}

void TestCubeRegionGeom::annulus_outsideOuterIsFalse()
{
    const P centre{ 0, 0 }, outer{ 10, 0 };
    QVERIFY(!pointInAnnulus(centre, outer, 4.0, 11, 0));  // r=11 > outer 10
}

void TestCubeRegionGeom::annulus_degenerateOuterIsFalse()
{
    const P centre{ 5, 5 }, outer{ 5, 5 };   // outer radius 0
    QVERIFY(!pointInAnnulus(centre, outer, 0.0, 5, 5));
}

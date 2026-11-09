#include "TestWcsAxisTicks.h"

#include "gui/WcsAxisTicks.h"

#include <QtTest>

#include <cmath>

using visivo::AxisVoxel;

void TestWcsAxisTicks::theXAxisIsReadAlongTheBottomEdge()
{
    for (double t : { 0.0, 0.25, 0.5, 1.0 }) {
        const AxisVoxel v = visivo::xAxisTickVoxel(t, 10.0, 690.0, 5.0);
        QCOMPARE(v.x, 10.0 + t * 680.0);
        QCOMPARE(v.y, 5.0);   // the axis' own height, whatever t
    }
}

void TestWcsAxisTicks::theYAxisIsReadAlongTheLeftEdge()
{
    for (double t : { 0.0, 0.25, 0.5, 1.0 }) {
        const AxisVoxel v = visivo::yAxisTickVoxel(t, 5.0, 695.0, 10.0);
        QCOMPARE(v.x, 10.0);   // the axis' own column, whatever t
        QCOMPARE(v.y, 5.0 + t * 690.0);
    }
}

void TestWcsAxisTicks::aRotatedFrameLabelsTheLeftEdgeNotTheDiagonal()
{
    // HCG 44 in Galactic coordinates, to first order: b falls by ~1.5° across
    // the 700 columns and rises by ~0.45° up the 700 rows (astropy:
    // b(0,0) = 55.58, b(699,0) = 54.10, b(0,699) = 56.02).
    const auto latitude = [](double x, double y) { return 55.58 - 0.00212 * x + 0.00063 * y; };
    const double xMin = 0.0, xMax = 699.0, yMin = 0.0, yMax = 699.0;
    // The top label: read at the top-left corner, as the hover readout there says.
    const AxisVoxel top = visivo::yAxisTickVoxel(1.0, yMin, yMax, xMin);
    QVERIFY(std::abs(latitude(top.x, top.y) - 56.02) < 0.01);
    // Read on the diagonal it was the top-RIGHT corner's latitude, ~1.5° away:
    // the axis ran the wrong way against the readout.
    QVERIFY(std::abs(latitude(xMax, yMax) - latitude(top.x, top.y)) > 1.0);
    // Labels grow upwards, as b does along the left edge.
    double previous = -1e9;
    for (double t : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
        const AxisVoxel v = visivo::yAxisTickVoxel(t, yMin, yMax, xMin);
        const double b = latitude(v.x, v.y);
        QVERIFY(b > previous);
        previous = b;
    }
}

#include "TestPvPathGeometry.h"

#include "gui/PvPathGeometry.h"

#include <QtTest>

#include <algorithm>
#include <cmath>

using pvpath::Path;
using pvpath::Vertex;

void TestPvPathGeometry::hitVertexPicksTheNearestWithinTolerance()
{
    const Path pts{ { 0, 0 }, { 10, 0 }, { 12, 0 } };
    QCOMPARE(pvpath::hitVertex(pts, { 11.2, 0.5 }, 3.0), 2);   // nearer to 12 than to 10
    QCOMPARE(pvpath::hitVertex(pts, { 0.5, 0.5 }, 3.0), 0);
    QCOMPARE(pvpath::hitVertex(pts, { 5, 5 }, 3.0), -1);
}

void TestPvPathGeometry::hitSegmentReportsSegmentAndPosition()
{
    const Path pts{ { 0, 0 }, { 10, 0 }, { 10, 10 } };
    const auto h = pvpath::hitSegment(pts, { 2.5, 1.0 }, 2.0);
    QCOMPARE(h.segment, 0);
    QVERIFY(std::abs(h.t - 0.25) < 1e-12);
    QVERIFY(std::abs(h.distance - 1.0) < 1e-12);
    QCOMPARE(pvpath::hitSegment(pts, { 11, 6 }, 2.0).segment, 1);
    QCOMPARE(pvpath::hitSegment(pts, { 5, 5 }, 2.0).segment, -1);
}

void TestPvPathGeometry::translateAndRotate()
{
    const Path pts{ { 0, 0 }, { 2, 0 } };
    const Path moved = pvpath::translated(pts, 1.5, -2.0);
    QCOMPARE(moved[1][0], 3.5);
    QCOMPARE(moved[1][1], -2.0);
    const Vertex c = pvpath::centroid(pts);
    QCOMPARE(c[0], 1.0);
    const Path turned = pvpath::rotated(pts, c, M_PI / 2);
    QVERIFY(std::abs(turned[0][0] - 1.0) < 1e-12 && std::abs(turned[0][1] + 1.0) < 1e-12);
    QVERIFY(std::abs(turned[1][0] - 1.0) < 1e-12 && std::abs(turned[1][1] - 1.0) < 1e-12);
    QVERIFY(std::abs(pvpath::length(turned) - 2.0) < 1e-12);   // rigid
    QVERIFY(std::abs(pvpath::angleAround({ 0, 0 }, { 0, 3 }) - M_PI / 2) < 1e-12);
}

void TestPvPathGeometry::insertAndRemoveVertices()
{
    const Path pts{ { 0, 0 }, { 10, 0 } };
    const Path three = pvpath::withVertexInserted(pts, 0, { 5, 1 });
    QCOMPARE(three.size(), std::size_t(3));
    QCOMPARE(three[1][0], 5.0);
    QCOMPARE(pvpath::withVertexInserted(pts, 1, { 5, 1 }).size(), std::size_t(2));   // no segment 1
    QCOMPARE(pvpath::withVertexRemoved(three, 1), pts);
    QCOMPARE(pvpath::withVertexRemoved(pts, 0), pts);   // a path keeps two vertices
}

void TestPvPathGeometry::smoothCurvePassesThroughControlVertices()
{
    const Path control{ { 0, 0 }, { 10, 10 }, { 20, 0 }, { 40, 5 } };
    const Path curve = pvpath::smoothed(control, 8);
    QCOMPARE(curve.size(), std::size_t(3 * 8 + 1));
    for (std::size_t i = 0; i < control.size(); ++i) {
        const Vertex &onCurve = curve[i * 8];
        QVERIFY2(std::hypot(onCurve[0] - control[i][0], onCurve[1] - control[i][1]) < 1e-9,
                 qPrintable(QString::number(i)));
    }
    // Smooth, not a polyline: the midpoint of the first span is off the chord.
    const Vertex &mid = curve[4];
    QVERIFY(std::abs(mid[0] - mid[1]) > 0.1);
    QVERIFY(pvpath::length(curve) > pvpath::length(control));
}

void TestPvPathGeometry::smoothCurveOfTwoVerticesIsTheLine()
{
    const Path two{ { 1, 2 }, { 3, 4 } };
    QCOMPARE(pvpath::smoothed(two, 8), two);
}

void TestPvPathGeometry::descendingSpectralAxisRowsAreReversed()
{
    // 2 samples × 3 channels; emission in channel 2 (row 2 in channel order).
    const std::vector<double> rows{ 0, 0, 0, 0, 7, 9 };
    // CDELT3 < 0: channel 2 is the LOWEST velocity → it must become row 0.
    const auto desc = pvpath::rowsOnUniformSpectralGrid(rows, 2, 3, { 500, 498, 496 });
    QCOMPARE(desc.rows, 3);
    QCOMPARE(desc.values, (std::vector<double>{ 7, 9, 0, 0, 0, 0 }));
    // Ascending axis: unchanged.
    QCOMPARE(pvpath::rowsOnUniformSpectralGrid(rows, 2, 3, { 1, 2, 3 }).values, rows);
    // Inconsistent sizes: untouched rather than garbled.
    QCOMPARE(pvpath::rowsOnUniformSpectralGrid(rows, 2, 3, { 3, 2 }).values, rows);
}

void TestPvPathGeometry::nonLinearSpectralAxisIsPlacedByValue()
{
    // 1 sample × 3 channels at coordinates 0, 1, 10: a uniform plot grid with the
    // finest spacing (1) has 11 rows; rows 0 and 1 are channels 0 and 1, rows
    // 2..5 are nearest to channel 1, rows 6..10 to channel 2. The middle channel
    // is NOT stretched to the middle of the axis (5).
    const auto g = pvpath::rowsOnUniformSpectralGrid({ 100, 200, 300 }, 1, 3, { 0, 1, 10 });
    QCOMPARE(g.rows, 11);
    QCOMPARE(g.values, (std::vector<double>{ 100, 200, 200, 200, 200, 200, 300, 300, 300, 300, 300 }));

    // 5000 mildly non-linear channels: no channel may be dropped.
    std::vector<double> values, spectral;
    for (int i = 0; i < 5000; ++i) {
        values.push_back(i);
        spectral.push_back(i + 1e-5 * i * i);
    }
    const auto big = pvpath::rowsOnUniformSpectralGrid(values, 1, 5000, spectral);
    std::vector<bool> seen(5000, false);
    for (double v : big.values)
        seen[std::size_t(v)] = true;
    QVERIFY(std::all_of(seen.begin(), seen.end(), [](bool b) { return b; }));

    // An outlier coordinate makes the fine grid too large: one row per channel,
    // in value order, none lost.
    const auto far = pvpath::rowsOnUniformSpectralGrid({ 10, 20, 30, 40 }, 1, 4, { 0, 1, 2, 10000 });
    QCOMPARE(far.rows, 4);
    QCOMPARE(far.values, (std::vector<double>{ 10, 20, 30, 40 }));
}

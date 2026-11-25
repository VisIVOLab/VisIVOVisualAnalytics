#include "TestCubeAxesEdgeOn.h"

#include "vtk/CubeAxesEdgeOn.h"

#include <QtTest>

#include <cmath>

namespace {
const double kMid[3] = { 0.5, 0.5, 0.5 };
}

void TestCubeAxesEdgeOn::aViewAlongAnAxisDropsOnlyThatAxis()
{
    for (int axis = 0; axis < 3; ++axis) {
        double eye[3] = { 0.5, 0.5, 0.5 };
        eye[axis] += 9.0;
        const auto s = CubeAxesEdgeOn::axisSines(eye, kMid);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(CubeAxesEdgeOn::readable(s[i]), i != axis);
    }
}

void TestCubeAxesEdgeOn::anObliqueViewKeepsEveryAxis()
{
    const double eye[3] = { 3.0, 2.5, 2.0 };
    const auto s = CubeAxesEdgeOn::axisSines(eye, kMid);
    for (int i = 0; i < 3; ++i)
        QVERIFY(CubeAxesEdgeOn::readable(s[i]));
}

void TestCubeAxesEdgeOn::theAngleDecidesNotTheDistance()
{
    // 12 degrees off the Z axis is squeezed into a streak, 25 degrees is not,
    // however far the camera is.
    const double pi = std::acos(-1.0);
    for (double dist : { 2.0, 50.0 }) {
        const double a12 = 12.0 * pi / 180.0, a25 = 25.0 * pi / 180.0;
        const double eye12[3] = { 0.5 + dist * std::sin(a12), 0.5, 0.5 + dist * std::cos(a12) };
        const double eye25[3] = { 0.5 + dist * std::sin(a25), 0.5, 0.5 + dist * std::cos(a25) };
        QVERIFY(!CubeAxesEdgeOn::readable(CubeAxesEdgeOn::axisSines(eye12, kMid)[2]));
        QVERIFY(CubeAxesEdgeOn::readable(CubeAxesEdgeOn::axisSines(eye25, kMid)[2]));
    }
}

void TestCubeAxesEdgeOn::aParallelViewFollowsItsFocalPoint()
{
    // Panned off the middle of the box, a view along Z still looks along Z.
    const double eye[3] = { 30.5, 0.5, 100.0 };
    const double focal[3] = { 30.5, 0.5, 0.5 };
    const auto s = CubeAxesEdgeOn::axisSines(eye, focal);
    QVERIFY(CubeAxesEdgeOn::readable(s[0]));
    QVERIFY(CubeAxesEdgeOn::readable(s[1]));
    QVERIFY(!CubeAxesEdgeOn::readable(s[2]));
}

void TestCubeAxesEdgeOn::anEyeInTheMiddleOfTheBoxKeepsEveryAxis()
{
    const double eye[3] = { 0.5, 0.5, 0.5 };
    const auto s = CubeAxesEdgeOn::axisSines(eye, kMid);
    for (int i = 0; i < 3; ++i)
        QVERIFY(CubeAxesEdgeOn::readable(s[i]));
}

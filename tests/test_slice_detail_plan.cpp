// test_slice_detail_plan.cpp
// Unit tests for SliceDetailPlan.h (issue #381). The stride rule must be the
// backend's (app/compute/cube.py, plane_view): the client places the samples
// with it before the answer says which stride was used.

#include "TestSliceDetailPlan.h"
#include "SliceDetailPlan.h"

#include <QTest>

using namespace visivo;

void TestSliceDetailPlan::stride_matchesTheBackend()
{
    QCOMPARE(sliceStrideFor(7233, 7233, 2048), 4);
    QCOMPARE(sliceStrideFor(2048, 100, 2048), 1);
    QCOMPARE(sliceStrideFor(2049, 100, 2048), 2);
    QCOMPARE(sliceStrideFor(661, 661, 2048), 1);
    QCOMPARE(sliceStrideFor(7233, 7233, 0), 1);
}

void TestSliceDetailPlan::visible_isClampedToThePlane()
{
    const QRect r = visiblePlanePixels(-500.2, 9000.0, 100.4, 300.6, 7233, 7233);
    QCOMPARE(r, QRect(QPoint(0, 100), QPoint(7232, 301)));
    const QRect flipped = visiblePlanePixels(300.0, 100.0, 50.0, 10.0, 7233, 7233);
    QCOMPARE(flipped, QRect(QPoint(100, 10), QPoint(300, 50)));
}

void TestSliceDetailPlan::visible_missingThePlaneIsEmpty()
{
    QVERIFY(!visiblePlanePixels(8000.0, 9000.0, 0.0, 10.0, 7233, 7233).isValid());
    QVERIFY(!visiblePlanePixels(0.0, 10.0, -50.0, -10.0, 7233, 7233).isValid());
    QVERIFY(!visiblePlanePixels(qQNaN(), 10.0, 0.0, 10.0, 7233, 7233).isValid());
}

void TestSliceDetailPlan::detail_noneWhenNotFinerThanTheChannel()
{
    const QRect whole(0, 0, 7233, 7233);
    QVERIFY(!sliceDetailFor(whole, 7233, 7233, 4).has_value());
    QVERIFY(!sliceDetailFor(QRect(0, 0, 500, 500), 661, 661, 1).has_value());
    QVERIFY(!sliceDetailFor(QRect(), 7233, 7233, 4).has_value());
}

void TestSliceDetailPlan::detail_zoomedInAsksForThePartOnScreen()
{
    const QRect visible(QPoint(2000, 2000), QPoint(4399, 4399));   // 2400 px on screen
    const auto d = sliceDetailFor(visible, 7233, 7233, 4);
    QVERIFY(d.has_value());
    QVERIFY(d->region.contains(visible));
    QCOMPARE(d->region, QRect(QPoint(1400, 1400), QPoint(4999, 4999)));
    QCOMPARE(d->stride, 2);
}

void TestSliceDetailPlan::detail_fullResolutionOnceThePartFits()
{
    const QRect visible(QPoint(7000, 0), QPoint(7232, 999));
    const auto d = sliceDetailFor(visible, 7233, 7233, 4);
    QVERIFY(d.has_value());
    QCOMPARE(d->stride, 1);
    QCOMPARE(d->region.right(), 7232);
    QCOMPARE(d->region.top(), 0);
}

void TestSliceDetailPlan::covers_onlyWhenInsideAndFineEnough()
{
    const SliceDetailWindow have{ QRect(QPoint(1000, 1000), QPoint(3000, 3000)), 1 };
    const SliceDetailWindow want1{ QRect(), 1 };
    const SliceDetailWindow want2{ QRect(), 2 };
    QVERIFY(sliceDetailCovers(have, QRect(QPoint(1500, 1500), QPoint(2500, 2500)), want1));
    QVERIFY(!sliceDetailCovers(have, QRect(QPoint(500, 1500), QPoint(2500, 2500)), want1));
    const SliceDetailWindow coarse{ have.region, 2 };
    QVERIFY(!sliceDetailCovers(coarse, QRect(QPoint(1500, 1500), QPoint(2500, 2500)), want1));
    QVERIFY(sliceDetailCovers(coarse, QRect(QPoint(1500, 1500), QPoint(2500, 2500)), want2));
}

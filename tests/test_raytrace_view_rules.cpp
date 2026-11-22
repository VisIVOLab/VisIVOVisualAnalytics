#include "TestRaytraceViewRules.h"

#include "gui/RaytraceLoadingState.h"
#include "gui/ScalarColourRange.h"

#include <QtTest/QtTest>

#include <cmath>
#include <limits>

using visivo::RaytraceLoadingState;
using Show = visivo::RaytraceLoadingState::Show;

void TestRaytraceViewRules::nanTakesTheNanColour()
{
    const auto linear = visivo::scalarColourRange(-1.0, 1.0, false);
    const auto log = visivo::scalarColourRange(1.0, 100.0, true);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!visivo::colourPosition(nan, linear).has_value());
    QVERIFY(!visivo::colourPosition(nan, log).has_value());
    // 0 is data, not NaN: mid-table in a range about 0 (what NaN used to show).
    QCOMPARE(*visivo::colourPosition(0.0, linear), 0.5);
    // Infinities clamp to the table's ends, as the stills' lookup table does.
    QCOMPARE(*visivo::colourPosition(std::numeric_limits<double>::infinity(), linear), 1.0);
    QCOMPARE(*visivo::colourPosition(-std::numeric_limits<double>::infinity(), linear), 0.0);
}

void TestRaytraceViewRules::cgsDensitiesKeepTheirRange()
{
    // A gas density in g/cm³: the former shader floored at 1e-30, so the
    // whole range took the lowest colour.
    const auto log = visivo::scalarColourRange(1e-34, 1e-26, true);
    QVERIFY(log.log);
    QCOMPARE(log.lo, -34.0);
    QCOMPARE(log.span, 8.0);
    QVERIFY(std::abs(*visivo::colourPosition(1e-30, log) - 0.5) < 1e-12);
    QVERIFY(std::abs(*visivo::colourPosition(1e-33, log) - 0.125) < 1e-12);
    // Linear: a span of 1e-35 is a range, not a flat one.
    const auto linear = visivo::scalarColourRange(1e-35, 2e-35, false);
    QVERIFY(linear.span > 0.0);
    QVERIFY(std::abs(*visivo::colourPosition(1.5e-35, linear) - 0.5) < 1e-9);
    // And so is it in the float the shader works in.
    QVERIFY(float(linear.span) > 0.0f);
}

void TestRaytraceViewRules::aLogScaleNeedsAPositiveLowEnd()
{
    // A log range reaching zero is drawn linear, as the stills draw it.
    const auto r = visivo::scalarColourRange(0.0, 10.0, true);
    QVERIFY(!r.log);
    QCOMPARE(*visivo::colourPosition(5.0, r), 0.5);
    // On a log scale, a value at or below zero takes the lowest colour.
    const auto log = visivo::scalarColourRange(1.0, 10.0, true);
    QCOMPARE(*visivo::colourPosition(0.0, log), 0.0);
    QCOMPARE(*visivo::colourPosition(-3.0, log), 0.0);
}

void TestRaytraceViewRules::aFlatRangeTakesTheLowestColour()
{
    const auto flat = visivo::scalarColourRange(2.0, 2.0, false);
    QCOMPARE(flat.span, 0.0);
    QCOMPARE(*visivo::colourPosition(2.0, flat), 0.0);
    QCOMPARE(*visivo::colourPosition(7.0, flat), 0.0);
    const auto bad = visivo::scalarColourRange(std::nan(""), 1.0, false);
    QCOMPARE(bad.span, 0.0);
}

void TestRaytraceViewRules::theFirstLoadCoversTheView()
{
    RaytraceLoadingState s;
    QCOMPARE(s.show(), Show::Nothing);
    s.begin();
    QCOMPARE(s.show(), Show::Cover);
    QVERIFY(s.busy());
    s.frameShown();
    QCOMPARE(s.show(), Show::Nothing);
    QVERIFY(s.hasFrame());
}

void TestRaytraceViewRules::aRefetchOverAFrameIsABadge()
{
    RaytraceLoadingState s;
    s.begin();
    s.frameShown();
    // A radius or filter change: the frame stays, a badge says it is coming.
    s.begin();
    QCOMPARE(s.show(), Show::Badge);
    // Superseded by settings the frame already shows: nothing to wait for.
    s.finish();
    QCOMPARE(s.show(), Show::Nothing);
    s.begin();
    s.frameShown();
    QCOMPARE(s.show(), Show::Nothing);
}

void TestRaytraceViewRules::aStaleFrameDoesNotEndAFetch()
{
    RaytraceLoadingState s;
    s.begin();
    s.frameShown();
    // Fetch B begins; the frame of A's data (or a still) is painted after.
    s.begin();
    s.frameShown(true);
    QCOMPARE(s.show(), Show::Badge);
    QVERIFY(s.busy());
    // Before any frame at all: a still shown during the first fetch.
    RaytraceLoadingState first;
    first.begin();
    first.frameShown(true);
    QVERIFY(first.busy() && first.hasFrame());
    QCOMPARE(first.show(), Show::Badge);
    // B's own frame ends it.
    s.frameShown(false);
    QCOMPARE(s.show(), Show::Nothing);
}

void TestRaytraceViewRules::errorsNeverLeaveASpinner()
{
    RaytraceLoadingState s;
    s.begin();
    s.fail();
    QCOMPARE(s.show(), Show::Cover);   // nothing drawn: said over the view
    QVERIFY(!s.busy() && s.error());
    s.begin();
    QVERIFY(s.busy() && !s.error());
    s.frameShown();
    s.begin();
    s.fail();
    QCOMPARE(s.show(), Show::Badge);   // the frame stays, the badge says why
    QVERIFY(!s.busy());
    // The next frame clears it.
    s.begin();
    s.frameShown();
    QCOMPARE(s.show(), Show::Nothing);
}

void TestRaytraceViewRules::aGpuFailureCoversUntilTheBackendsFrame()
{
    RaytraceLoadingState s;
    s.begin();
    s.frameShown();
    // The GPU view could not start: the backend's frame page is empty.
    s.frameLost();
    s.begin();
    QCOMPARE(s.show(), Show::Cover);
    s.fail();   // its request failed: no spinner left behind
    QCOMPARE(s.show(), Show::Cover);
    QVERIFY(!s.busy());
    s.begin();
    s.frameShown();
    QCOMPARE(s.show(), Show::Nothing);
}

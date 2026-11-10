#include "TestBusyFeedback.h"

#include "gui/BusyTracker.h"
#include "gui/SelectionMarkerSize.h"

#include <QtTest>

#include <cmath>

using namespace Qt::StringLiterals;
using visivo::BusyOutcome;
using visivo::BusyTracker;

void TestBusyFeedback::oneScopeShowsAndSaysItsOutcome()
{
    BusyTracker t;
    QVERIFY(!t.showing());
    const int open = t.begin(visivo::openingTexts(u"stars.csv"_s));
    QVERIFY(t.showing());
    QCOMPARE(t.busyText(), u"Opening stars.csv…"_s);
    QCOMPARE(t.finish(open, BusyOutcome::Done), std::optional<QString>(u"Opened stars.csv."_s));
    QVERIFY(!t.showing());
    // Its outcome was said: nothing to clear when it ends.
    QVERIFY(t.end(open).clearText.isEmpty());
    QCOMPARE(t.operations(), 0);
}

void TestBusyFeedback::nestedScopesJoinTheirOperation()
{
    BusyTracker t;
    const int reopen = t.begin(visivo::openingTexts(u"cube.fits"_s), 1);
    // A step begun at the same event-loop level joins it.
    const int open = t.begin(visivo::openingTexts(u"cube.fits"_s), 1);
    QCOMPARE(open, reopen);
    QCOMPARE(t.operations(), 1);
    // The step's failure ends the operation at once, before the outer returns.
    QVERIFY(t.finish(open, BusyOutcome::Failed));
    QVERIFY(!t.showing());
    QCOMPARE(t.finish(reopen, BusyOutcome::Failed), std::nullopt);   // said once
    t.end(open);
    QCOMPARE(t.operations(), 1);   // the outer scope still holds it
    t.end(reopen);
    QCOMPARE(t.operations(), 0);
}

void TestBusyFeedback::aClickInANestedLoopIsItsOwnOperation()
{
    BusyTracker t;
    const int slow = t.begin(visivo::openingTexts(u"big.csv"_s), 1);
    // Another open clicked while big.csv waits in a nested event loop.
    const int other = t.begin(visivo::openingTexts(u"small.fits"_s), 2);
    QVERIFY(other != slow);
    QCOMPARE(t.finish(other, BusyOutcome::Done),
             std::optional<QString>(u"Opened small.fits."_s));
    t.end(other);
    QVERIFY(t.showing());   // big.csv still loads
    QCOMPARE(t.busyText(), u"Opening big.csv…"_s);
    QCOMPARE(t.finish(slow, BusyOutcome::Done), std::optional<QString>(u"Opened big.csv."_s));
}

void TestBusyFeedback::aDetachedOpenOutlivesItsCaller()
{
    // Case A: a VBT open in flight (detached), then X is opened and finishes.
    BusyTracker t;
    const int dispatch = t.begin(visivo::openingTexts(u"points.bin"_s), 1);
    const int vbt = t.begin(visivo::openingTexts(u"points"_s), 1, /*detached=*/true);
    QVERIFY(vbt != dispatch);
    // The dispatcher returns while the worker reads: no outcome of its own.
    QCOMPARE(t.end(dispatch).clearText, u"Opening points.bin…"_s);
    QVERIFY(t.showing());
    const int x = t.begin(visivo::openingTexts(u"x.fits"_s), 1);
    QVERIFY(x != vbt);   // not joined to the detached one
    QCOMPARE(t.finish(x, BusyOutcome::Done), std::optional<QString>(u"Opened x.fits."_s));
    t.end(x);
    QVERIFY(t.showing());   // the VBT still loads
    QCOMPARE(t.busyText(), u"Opening points…"_s);
    QCOMPARE(t.finish(vbt, BusyOutcome::Failed),
             std::optional<QString>(u"Could not open points."_s));
    t.end(vbt);
    QVERIFY(!t.showing());
    QCOMPARE(t.operations(), 0);
}

void TestBusyFeedback::aDetachedOutcomeIsItsOwn()
{
    // Case B: the VBT finishes while X runs.
    BusyTracker t;
    const int vbt = t.begin(visivo::openingTexts(u"points"_s), 1, true);
    const int x = t.begin(visivo::openingTexts(u"x.fits"_s), 1);
    QCOMPARE(t.finish(vbt, BusyOutcome::Done), std::optional<QString>(u"Opened points."_s));
    QVERIFY(t.showing());   // X is still opening
    QCOMPARE(t.busyText(), u"Opening x.fits…"_s);
    t.end(vbt);   // removes the VBT's entry, not X's
    QCOMPARE(t.operations(), 1);
    QCOMPARE(t.finish(x, BusyOutcome::Done), std::optional<QString>(u"Opened x.fits."_s));
}

void TestBusyFeedback::aScopeWithoutOutcomeClearsItsText()
{
    BusyTracker t;
    const int open = t.begin(visivo::openingTexts(u"x.vbt"_s));
    QCOMPARE(t.end(open).clearText, u"Opening x.vbt…"_s);   // a chooser dismissed
    QVERIFY(!t.showing());
    // An unknown or ended token does nothing.
    QVERIFY(t.end(open).clearText.isEmpty());
    QCOMPARE(t.finish(open, BusyOutcome::Done), std::nullopt);
}

void TestBusyFeedback::aPauseHidesWithoutEnding()
{
    BusyTracker t;
    const int open = t.begin(visivo::openingTexts(u"a.fits"_s));
    t.pause();   // a modal box comes up
    QVERIFY(!t.showing());
    QVERIFY(t.paused());
    t.resume();
    QVERIFY(t.showing());
    t.resume();   // unbalanced: harmless
    QVERIFY(!t.paused());
    QVERIFY(t.finish(open, BusyOutcome::Done));
}

void TestBusyFeedback::detailFollowsTheSentence()
{
    BusyTracker t;
    const int open = t.begin(visivo::openingTexts(u"gone.csv"_s));
    QCOMPARE(t.finish(open, BusyOutcome::Failed, u"the file is no longer there"_s),
             std::optional<QString>(u"Could not open gone.csv: the file is no longer there"_s));
    const int task = t.begin(visivo::taskTexts(u"Building the mosaic"_s));
    QCOMPARE(t.finish(task, BusyOutcome::Cancelled),
             std::optional<QString>(u"Building the mosaic: cancelled."_s));
}

void TestBusyFeedback::worldPerPixelForBothProjections()
{
    // Parallel: the view's height is 2 × scale, whatever the depth.
    QCOMPARE(visivo::worldPerPixel(true, 30.0, 50.0, 1.0e6, 500.0), 0.2);
    // Perspective, 90°: at depth d the view is 2d tall.
    QVERIFY(std::abs(visivo::worldPerPixel(false, 90.0, 0.0, 10.0, 400.0) - 0.05) < 1e-12);
    // Behind the camera, no viewport, a broken angle: unknown.
    QCOMPARE(visivo::worldPerPixel(false, 30.0, 0.0, -1.0, 400.0), 0.0);
    QCOMPARE(visivo::worldPerPixel(false, 30.0, 0.0, 10.0, 0.0), 0.0);
    QCOMPARE(visivo::worldPerPixel(false, 180.0, 0.0, 10.0, 400.0), 0.0);
}

void TestBusyFeedback::markerKeepsToItsPixelRange()
{
    const visivo::MarkerPixelRange points = visivo::markerPixelRange(false);
    const visivo::MarkerPixelRange glyphs = visivo::markerPixelRange(true);
    QVERIFY(points.maxPx < glyphs.maxPx);
    // One world unit per pixel: a radius of 1000 (half the view) comes down
    // to the bound, a tiny one goes up to a visible ring.
    QCOMPARE(visivo::clampMarkerRadius(1000.0, 1.0, points), points.maxPx);
    QCOMPARE(visivo::clampMarkerRadius(1.0e-6, 1.0, points), points.minPx);
    QCOMPARE(visivo::clampMarkerRadius(20.0, 1.0, glyphs), 20.0);
    // Not a number, or nothing: the smallest ring.
    QCOMPARE(visivo::clampMarkerRadius(std::nan(""), 2.0, points), points.minPx * 2.0);
    QCOMPARE(visivo::clampMarkerRadius(0.0, 2.0, glyphs), glyphs.minPx * 2.0);
}

void TestBusyFeedback::unknownScaleLeavesTheMarkerAlone()
{
    QCOMPARE(visivo::clampMarkerRadius(123.0, 0.0, visivo::markerPixelRange(false)), 123.0);
}

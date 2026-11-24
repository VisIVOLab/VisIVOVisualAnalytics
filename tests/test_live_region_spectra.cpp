// test_live_region_spectra.cpp
// See TestLiveRegionSpectra.h.

#include "TestLiveRegionSpectra.h"

#include "LiveRegionSpectra.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace visivo::liveregion;
using visivo::regions::Region;
using visivo::regions::Shape;

namespace {
Region box(int id, const QString &name, const QString &color = QString())
{
    Region r;
    r.id = id;
    r.shape = Shape::Box;
    r.name = name;
    r.color = color;
    r.center = { 10.0 * id, 5.0 };
    r.size = { 4.0, 6.0 };
    return r;
}
}   // namespace

void TestLiveRegionSpectra::theFirstRequestRunsAtOnce()
{
    Throttle t(150);
    QCOMPARE(int(t.request(1000).action), int(Throttle::Action::Run));
    QVERIFY(!t.armed());
}

void TestLiveRegionSpectra::aBurstRunsOnceAndThenOnTheTrailingEdge()
{
    Throttle t(150);
    QCOMPARE(int(t.request(1000).action), int(Throttle::Action::Run));
    // 40 ms later: too soon, a timer for the remaining 110 ms.
    const auto armed = t.request(1040);
    QCOMPARE(int(armed.action), int(Throttle::Action::Arm));
    QCOMPARE(armed.delayMs, 110);
    // More moves while the timer runs wait for it: one run, not one per move.
    QCOMPARE(int(t.request(1060).action), int(Throttle::Action::Wait));
    QCOMPARE(int(t.request(1100).action), int(Throttle::Action::Wait));
    t.fired(1150);
    QVERIFY(!t.armed());
    // The trailing run counts as a run: the next move right after is held back again.
    QCOMPARE(int(t.request(1160).action), int(Throttle::Action::Arm));
}

void TestLiveRegionSpectra::aRequestAfterTheIntervalRunsAgain()
{
    Throttle t(150);
    t.request(1000);
    QCOMPARE(int(t.request(1150).action), int(Throttle::Action::Run));
    QCOMPARE(int(t.request(5000).action), int(Throttle::Action::Run));
}

void TestLiveRegionSpectra::aRunOutsideTheThrottleRestartsItsClock()
{
    Throttle t(150);
    t.request(1000);
    QCOMPARE(int(t.request(1050).action), int(Throttle::Action::Arm));
    // The release did the work itself: no timer is owed and the clock restarts.
    t.ranNow(1080);
    QVERIFY(!t.armed());
    const auto d = t.request(1100);
    QCOMPARE(int(d.action), int(Throttle::Action::Arm));
    QCOMPARE(d.delayMs, 130);
}

void TestLiveRegionSpectra::onlyTheLatestTicketIsCurrent()
{
    LatestWins w;
    const auto a = w.next();
    QVERIFY(w.isCurrent(a));
    const auto b = w.next();
    QVERIFY(!w.isCurrent(a));
    QVERIFY(w.isCurrent(b));
    QVERIFY(!w.isCurrent(0));   // no ticket was ever taken
}

void TestLiveRegionSpectra::aSwitchedOffRequestIsNeverCurrent()
{
    LatestWins w;
    const auto a = w.next();
    w.invalidate();
    QVERIFY(!w.isCurrent(a));
}

void TestLiveRegionSpectra::theCacheDropsTheOldestFirst()
{
    SpectrumCache c(2);
    c.insert(u"a"_s, { 1.0 });
    c.insert(u"b"_s, { 2.0 });
    c.insert(u"a"_s, { 1.5 });   // replaced, not a third entry
    QCOMPARE(c.size(), std::size_t(2));
    c.insert(u"c"_s, { 3.0 });   // "a" was first in
    QVERIFY(!c.find(u"a"_s));
    QVERIFY(c.find(u"b"_s));
    QCOMPARE((*c.find(u"c"_s))[0], 3.0);
    c.clear();
    QCOMPARE(c.size(), std::size_t(0));
}

void TestLiveRegionSpectra::theSignatureFollowsTheGeometryNotTheName()
{
    Region a = box(1, u"Box 1"_s);
    Region renamed = a;
    renamed.name = u"Core"_s;
    renamed.color = u"#ff0000"_s;
    QCOMPARE(geometrySignature(a), geometrySignature(renamed));
    Region moved = a;
    moved.center[0] += 1.0;
    QVERIFY(geometrySignature(a) != geometrySignature(moved));
}

void TestLiveRegionSpectra::theOverlayLeavesOutTheSelectedAndWhatHasNoPixels()
{
    Region line = box(3, u"Line 3"_s);
    line.shape = Shape::Line;
    Region hidden = box(4, u"Hidden"_s);
    hidden.visible = false;
    const std::vector<Region> list{ box(1, u"Box 1"_s, u"#ff8800"_s), box(2, u"Box 2"_s),
                                    line, hidden, box(5, QString()) };
    const auto plan = overlayPlan(list, 2);
    QCOMPARE(plan.size(), std::size_t(2));
    QCOMPARE(plan[0].regionId, 1);
    QCOMPARE(plan[0].name, u"Box 1"_s);
    QCOMPARE(plan[0].colour, u"#ff8800"_s);   // the region's own colour
    QCOMPARE(plan[1].regionId, 5);
    QCOMPARE(plan[1].name, u"Region 5"_s);    // unnamed: called by its id
    QCOMPARE(plan[1].colour, defaultRegionColour());
}

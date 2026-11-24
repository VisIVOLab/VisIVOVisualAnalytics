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
    // It shares the default colour with the selected region: both are
    // moved to the palette, so that the curves differ.
    QVERIFY(plan[1].colour != defaultRegionColour());
    QVERIFY(plan[1].colour != overlayColours(list, 2).at(2));
}

void TestLiveRegionSpectra::regionsWithTheSameColourGetDistinctOnes()
{
    // Four boxes with the default colour and one with its own.
    const std::vector<Region> list{ box(1, u"A"_s), box(2, u"B"_s), box(3, u"C"_s),
                                    box(4, u"D"_s), box(5, u"E"_s, u"#ff8800"_s) };
    const auto colours = overlayColours(list, 1);
    QCOMPARE(colours.at(5), u"#ff8800"_s);   // unique: kept
    QSet<QString> seen;
    for (const Region &r : list) {
        QVERIFY(colours.at(r.id) != defaultRegionColour());
        seen.insert(colours.at(r.id));
    }
    QCOMPARE(seen.size(), 5);
    // The plan carries the same colours (they are what the legend shows).
    for (const auto &e : overlayPlan(list, 1))
        QCOMPARE(e.colour, colours.at(e.regionId));
    // Stable: removing a region that did not collide leaves the others alone.
    std::vector<Region> fewer = list;
    fewer.pop_back();
    const auto again = overlayColours(fewer, 1);
    for (int id : { 1, 2, 3, 4 })
        QCOMPARE(again.at(id), colours.at(id));
    // A palette colour a region holds by itself is not given to another.
    std::vector<Region> clash{ box(1, u"A"_s), box(2, u"B"_s),
                               box(3, u"C"_s, overlayPalette().at(2)) };
    const auto c2 = overlayColours(clash, 1);
    QVERIFY(c2.at(1) != c2.at(3));
    QVERIFY(c2.at(2) != c2.at(3));
    QVERIFY(c2.at(1) != c2.at(2));
    // Alone with a default colour: it keeps it.
    QCOMPARE(overlayColours({ box(1, u"A"_s) }, 1).at(1), defaultRegionColour());
}

void TestLiveRegionSpectra::keepAllTakesEveryRegionWithPixels()
{
    Region line = box(2, u"Line 2"_s);
    line.shape = Shape::Line;
    Region hidden = box(3, u"Hidden"_s);
    hidden.visible = false;
    const std::vector<Region> list{ box(1, u"Box 1"_s), line, hidden, box(4, u"Box 4"_s) };
    const std::vector<int> want{ 1, 3, 4 };
    QVERIFY(keepAllTargets(list) == want);
    QVERIFY(keepAllTargets({}).empty());
}

void TestLiveRegionSpectra::aKeptSpectrumIsNotKeptTwice()
{
    KeptLedger ledger;
    const auto exists = [](const QString &id) { return id != u"gone"_s; };
    const QString k = KeptLedger::key(u"sig"_s, u"cube"_s);
    QVERIFY(ledger.find(1, k, exists).isEmpty());
    ledger.record(1, k, u"spectrum1"_s);
    QCOMPARE(ledger.find(1, k, exists), u"spectrum1"_s);
    // The region moved, or the cube changed: a new spectrum.
    QVERIFY(ledger.find(1, KeptLedger::key(u"sig2"_s, u"cube"_s), exists).isEmpty());
    QVERIFY(ledger.find(1, KeptLedger::key(u"sig"_s, u"cube2"_s), exists).isEmpty());
    QVERIFY(ledger.find(2, k, exists).isEmpty());
    // The product was deleted from Session Data: made again on demand.
    ledger.record(2, k, u"gone"_s);
    QVERIFY(ledger.find(2, k, exists).isEmpty());
}

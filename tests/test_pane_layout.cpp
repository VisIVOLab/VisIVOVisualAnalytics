// test_pane_layout.cpp
// Unit tests for the 1 / 2 / 4 pane layout rules the cube and particle windows share
// (PaneLayoutModel.h) and the particle panes' widget-free state (ParticlePaneState.h).
// The cube window's own pane behaviour was moved onto the same rules, so these are
// also the guard that the layouts it shows did not change.

#include "TestPaneLayout.h"

#include "ParticlePaneState.h"
#include "PaneLayoutModel.h"

#include <QJsonArray>
#include <QTest>

using namespace Qt::StringLiterals;

using namespace visivo::panes;
using namespace visivo::particlepanes;
using Slots = std::array<bool, kSlots>;

void TestPaneLayout::normalisedCount_acceptsOnlyOneTwoFour()
{
    QCOMPARE(normalisedCount(1), 1);
    QCOMPARE(normalisedCount(2), 2);
    QCOMPARE(normalisedCount(4), 4);
    QCOMPARE(normalisedCount(3), 1);
    QCOMPARE(normalisedCount(0), 1);
    QCOMPARE(normalisedCount(-2), 1);
}

void TestPaneLayout::visibleSlots_perLayout()
{
    QCOMPARE(visibleSlots(1, false), (Slots{ true, false, false, false }));
    QCOMPARE(visibleSlots(2, false), (Slots{ true, true, false, false }));
    QCOMPARE(visibleSlots(4, false), (Slots{ true, true, true, true }));
    QCOMPARE(visibleSlots(4, true), (Slots{ true, true, true, true }));
    QCOMPARE(visibleSlots(7, false), (Slots{ true, false, false, false }));
}

void TestPaneLayout::visibleSlots_twoPanesStackedUseTheLeftColumn()
{
    QCOMPARE(visibleSlots(2, true), (Slots{ true, false, true, false }));
}

void TestPaneLayout::columns_leftHoldsSlotsZeroAndTwo()
{
    QVERIFY(inLeftColumn(0));
    QVERIFY(!inLeftColumn(1));
    QVERIFY(inLeftColumn(2));
    QVERIFY(!inLeftColumn(3));
}

void TestPaneLayout::growth_goesOneTwoFour()
{
    QCOMPARE(grownCount(1), 2);
    QCOMPARE(grownCount(2), 4);
    QCOMPARE(grownCount(4), 4);
    QVERIFY(canGrow(1));
    QVERIFY(canGrow(2));
    QVERIFY(!canGrow(4));
}

void TestPaneLayout::newPane_takesTheFirstFreeVisibleSlot()
{
    int count = 0;
    QCOMPARE(slotForNewPane(4, false, Slots{ true, false, true, false }, &count), 1);
    QCOMPARE(count, 4);
    QCOMPARE(slotForNewPane(2, false, Slots{ true, false, false, false }, &count), 1);
    QCOMPARE(count, 2);
}

void TestPaneLayout::newPane_growsWhenEverySlotIsTaken()
{
    int count = 0;
    QCOMPARE(slotForNewPane(1, false, Slots{ true, false, false, false }, &count), 1);
    QCOMPARE(count, 2);   // 1 -> 2: the second pane goes beside the first
    QCOMPARE(slotForNewPane(2, false, Slots{ true, true, false, false }, &count), 2);
    QCOMPARE(count, 4);   // 2 -> 4
}

void TestPaneLayout::newPane_stackedPairGrowsIntoTheRightColumn()
{
    int count = 0;
    // A stacked pair holds slots 0 and 2; the next pane takes slot 1, not a slot that
    // would move either view.
    QCOMPARE(slotForNewPane(2, true, Slots{ true, false, true, false }, &count), 1);
    QCOMPARE(count, 4);
}

void TestPaneLayout::newPane_refusedWhenFull()
{
    int count = 0;
    QCOMPARE(slotForNewPane(4, false, Slots{ true, true, true, true }, &count), -1);
    QCOMPARE(count, 0);   // untouched
}

void TestPaneLayout::survivor_activePaneMovesToSlotZero()
{
    QCOMPARE(survivorSwapSlot(1, 4, 3), 3);
    QCOMPARE(survivorSwapSlot(1, 2, 1), 1);
    QCOMPARE(survivorSwapSlot(1, 4, 0), -1);   // slot 0 already survives
    QCOMPARE(survivorSwapSlot(2, 4, 3), -1);   // only a shrink to one pane swaps
    QCOMPARE(survivorSwapSlot(1, 1, 0), -1);
}

namespace {
QList<QuantityChoice> gasColumns()
{
    return { { u"builtin:fixed"_s, QString() },
             { u"raw:0"_s, u"x"_s },
             { u"raw:1"_s, u"y"_s },
             { u"raw:2"_s, u"z"_s },
             { u"raw:3"_s, u"vx"_s },
             { u"raw:4"_s, u"mass"_s },
             { u"raw:5"_s, u"Temperature"_s },
             { u"raw:6"_s, u"rho"_s },
             { u"raw:7"_s, u"entropy"_s },
             { u"raw:8"_s, u"id"_s } };
}
const QStringList kNotQuantities{ u"x"_s, u"y"_s, u"z"_s, u"vx"_s, u"vy"_s, u"vz"_s };
} // namespace

void TestPaneLayout::nextQuantity_prefersDensityThenTemperatureThenEntropy()
{
    const auto choices = gasColumns();
    QSet<QString> used;
    int item = nextQuantity(choices, used, kNotQuantities);
    QCOMPARE(choices.at(item).name, u"rho"_s);
    used.insert(choices.at(item).key);
    item = nextQuantity(choices, used, kNotQuantities);
    QCOMPARE(choices.at(item).name, u"Temperature"_s);   // case-insensitive
    used.insert(choices.at(item).key);
    item = nextQuantity(choices, used, kNotQuantities);
    QCOMPARE(choices.at(item).name, u"entropy"_s);
}

void TestPaneLayout::nextQuantity_skipsShownAndCoordinates()
{
    const auto choices = gasColumns();
    const QSet<QString> used{ u"raw:6"_s, u"raw:5"_s, u"raw:7"_s };
    const int item = nextQuantity(choices, used, kNotQuantities);
    // Of what is left only mass is preferred; x, y, z and vx are never offered.
    QCOMPARE(choices.at(item).name, u"mass"_s);
}

void TestPaneLayout::nextQuantity_fallsBackToAnyColumn()
{
    const QList<QuantityChoice> choices{ { u"raw:0"_s, u"x"_s }, { u"raw:1"_s, u"hsml"_s } };
    const int item = nextQuantity(choices, {}, kNotQuantities);
    QCOMPARE(choices.at(item).name, u"hsml"_s);
}

void TestPaneLayout::nextQuantity_noneLeft()
{
    const QList<QuantityChoice> choices{ { u"raw:0"_s, u"x"_s }, { u"raw:1"_s, u"rho"_s } };
    QCOMPARE(nextQuantity(choices, QSet<QString>{ u"raw:1"_s }, kNotQuantities), -1);
    QCOMPARE(nextQuantity({}, {}, kNotQuantities), -1);
}

void TestPaneLayout::session_roundTrips()
{
    PaneSession s;
    s.count = 4;
    s.active = 2;
    s.panes = { { u"rho"_s, true }, { u"temperature"_s, true }, { u"entropy"_s, false }, { {}, true } };
    QCOMPARE(fromJson(toJson(s)), s);
}

void TestPaneLayout::session_toleratesGarbage()
{
    const PaneSession empty = fromJson({});
    QCOMPARE(empty.count, 1);
    QCOMPARE(empty.active, 0);
    QVERIFY(empty.panes.isEmpty());

    const PaneSession odd = fromJson({ { u"count"_s, 3 }, { u"active"_s, 9 },
                                       { u"panes"_s, QJsonArray{ 5, QJsonObject{ { u"quantity"_s, u"rho"_s } } } } });
    QCOMPARE(odd.count, 1);                  // not a layout: one pane shown, the rest kept
    QCOMPARE(odd.panes.size(), 2);
    QCOMPARE(odd.panes[1].quantity, u"rho"_s);
    QVERIFY(odd.panes[1].linked);            // linked unless said otherwise
    QVERIFY(odd.active >= 0 && odd.active < odd.panes.size());
}

void TestPaneLayout::session_countCoversThePaneList()
{
    QJsonArray many;
    for (int i = 0; i < 9; ++i)
        many.append(QJsonObject{ { u"quantity"_s, QString::number(i) } });
    // Four quantities kept in a one-pane layout: the list is capped at the grid's slots,
    // the layout stays as saved (the other panes wait in hidden slots).
    const PaneSession s = fromJson({ { u"count"_s, 1 }, { u"panes"_s, many } });
    QCOMPARE(s.count, 1);
    QCOMPARE(s.panes.size(), 4);
    const QJsonArray four{ QJsonObject{}, QJsonObject{}, QJsonObject{}, QJsonObject{} };
    const PaneSession two = fromJson({ { u"count"_s, 2 }, { u"panes"_s, four } });
    QCOMPARE(two.count, 2);
    QCOMPARE(two.panes.size(), 4);
}

void TestPaneLayout::stackedForWidth_followsTheWindowUntilTheGridIsLaidOut()
{
    // A grid not laid out yet (width 0): the window decides, so a reopened wide window
    // shows a pair side by side whatever orientation it was saved in.
    QVERIFY(!stackedForWidth(0, 3000));
    QVERIFY(stackedForWidth(0, 800));
    // Once laid out, the grid's own width decides.
    QVERIFY(stackedForWidth(700, 3000));
    QVERIFY(!stackedForWidth(1600, 800));
}

void TestPaneLayout::layoutSlots_visiblePanesStay()
{
    const Slots taken{ true, true, false, false };
    const std::array<int, kSlots> to = slotsForLayout(taken, visibleSlots(2, false));
    QCOMPARE(to, (std::array<int, kSlots>{ 0, 1, 2, 3 }));
}

void TestPaneLayout::layoutSlots_fourToTwoSideBySideKeepsPaneBesideTheFirst()
{
    // Four panes shrunk to two side by side show slots {0, 1}; the second pane sat in slot 2.
    const Slots taken{ true, false, true, false };
    const std::array<int, kSlots> to = slotsForLayout(taken, visibleSlots(2, false));
    QCOMPARE(to[0], 0);
    QCOMPARE(to[2], 1);
}

void TestPaneLayout::layoutSlots_stackedPairWidenedMovesTheLowerPane()
{
    // A stacked pair (slots 0 and 2) in a window that was widened: side by side shows {0, 1}.
    const Slots taken{ true, false, true, false };
    QCOMPARE(slotsForLayout(taken, visibleSlots(2, true))[2], 2);    // stacked: stays
    QCOMPARE(slotsForLayout(taken, visibleSlots(2, false))[2], 1);   // wide: moves up
    // And the other way: a side by side pair in a narrowed window.
    const Slots wide{ true, true, false, false };
    QCOMPARE(slotsForLayout(wide, visibleSlots(2, true))[1], 2);
}

void TestPaneLayout::layoutSlots_morePanesThanRoomStayHidden()
{
    // Three panes in a two-pane layout: the third has no free visible slot and stays put.
    const Slots taken{ true, true, true, false };
    const std::array<int, kSlots> to = slotsForLayout(taken, visibleSlots(2, false));
    QCOMPARE(to[0], 0);
    QCOMPARE(to[1], 1);
    QCOMPARE(to[2], 2);
    // Panes in hidden slots fill the free visible ones in slot order, never two in one.
    const Slots many{ false, true, true, true };
    const std::array<int, kSlots> m = slotsForLayout(many, visibleSlots(2, false));
    QCOMPARE(m[1], 1);
    QCOMPARE(m[2], 0);
    QCOMPARE(m[3], 3);
}

void TestPaneLayout::session_hostGoesFirstWhateverItsSlot()
{
    // The host's window sat in slot 2 (the layout moved it); the session lists it first.
    std::array<PaneSlotState, kSlots> cells;
    cells[0] = { true, u"temperature"_s, true };
    cells[1] = { true, u"entropy"_s, false };
    cells[2] = { true, u"rho"_s, true };
    const PaneSession s = makeSession(4, 0, 2, cells);
    QCOMPARE(s.count, 4);
    QCOMPARE(s.panes.size(), 3);
    QCOMPARE(s.panes[0].quantity, u"rho"_s);
    QCOMPARE(s.panes[1].quantity, u"temperature"_s);
    QCOMPARE(s.panes[2].quantity, u"entropy"_s);
    QVERIFY(!s.panes[2].linked);
    // It reads back as the same session.
    QCOMPARE(fromJson(toJson(s)), s);
}

void TestPaneLayout::session_activeFollowsItsPaneInTheNewOrder()
{
    std::array<PaneSlotState, kSlots> cells;
    cells[0] = { true, u"temperature"_s, true };
    cells[2] = { true, u"rho"_s, true };
    QCOMPARE(makeSession(2, 2, 2, cells).active, 0);   // the host is active: first
    QCOMPARE(makeSession(2, 0, 2, cells).active, 1);   // the other pane: second
}

void TestPaneLayout::session_noHostSlotKeepsSlotOrder()
{
    std::array<PaneSlotState, kSlots> cells;
    cells[1] = { true, u"b"_s, true };
    cells[3] = { true, u"d"_s, true };
    const PaneSession s = makeSession(4, 3, -1, cells);
    QCOMPARE(s.panes.size(), 2);
    QCOMPARE(s.panes[0].quantity, u"b"_s);
    QCOMPARE(s.active, 1);
}

void TestPaneLayout::session_keepsEachPanesSlotAndTheLayout()
{
    // Four quantities in slots 0-3 (LogMass is the host's), a stacked two-pane layout that
    // shows slots 0 and 2, and the pane in slot 2 active.
    std::array<PaneSlotState, kSlots> cells;
    cells[0] = { true, u"LogMass"_s, true };
    cells[1] = { true, u"LogT"_s, true };
    cells[2] = { true, u"LogD"_s, true };
    cells[3] = { true, u"vx"_s, true };
    const PaneSession s = makeSession(2, 2, 0, cells, true);
    QCOMPARE(s.count, 2);
    QVERIFY(s.stacked);
    QCOMPARE(s.panes.size(), 4);
    QCOMPARE(s.panes[2].quantity, u"LogD"_s);
    QCOMPARE(s.panes[2].slot, 2);
    QCOMPARE(s.active, 2);
    const PaneSession back = fromJson(toJson(s));
    QCOMPARE(back, s);
    QCOMPARE(back.panes[1].slot, 1);
    QVERIFY(back.stacked);
    // The host sat in slot 2: it is listed first and says so.
    const PaneSession moved = makeSession(4, 3, 2, cells);
    QCOMPARE(moved.panes[0].quantity, u"LogD"_s);
    QCOMPARE(moved.panes[0].slot, 2);
    QCOMPARE(moved.active, 3);     // slot 3 is the last pane in that order
    QVERIFY(!moved.stacked);
    // Only a two-pane layout is stacked.
    QVERIFY(!makeSession(4, 0, 0, cells, true).stacked);
    QVERIFY(!fromJson({ { u"count"_s, 4 }, { u"stacked"_s, true } }).stacked);
}

void TestPaneLayout::session_withoutSlotsReadsAsNotKept()
{
    // What an earlier build saved: quantity and link only, no slot, no orientation.
    const PaneSession old = fromJson(
            { { u"version"_s, 1 }, { u"count"_s, 2 }, { u"active"_s, 1 },
              { u"panes"_s, QJsonArray{ QJsonObject{ { u"quantity"_s, u"rho"_s } },
                                         QJsonObject{ { u"quantity"_s, u"u"_s },
                                                      { u"slot"_s, 9 } } } } });
    QCOMPARE(old.panes.size(), 2);
    QCOMPARE(old.panes[0].slot, -1);
    QCOMPARE(old.panes[1].slot, -1);     // out of range reads as not kept
    QVERIFY(!old.stacked);
    QCOMPARE(old.active, 1);
}

void TestPaneLayout::restoredSlots_putEachPaneBack()
{
    std::array<PaneSlotState, kSlots> cells;
    cells[0] = { true, u"LogMass"_s, true };
    cells[1] = { true, u"LogT"_s, true };
    cells[2] = { true, u"LogD"_s, true };
    cells[3] = { true, u"vx"_s, true };
    // Saved stacked, restored in a window that is stacked too: slots as they were.
    const PaneSession s = makeSession(2, 2, 0, cells, true);
    QCOMPARE(restoredSlots(s, 4, true), (QList<int>{ 0, 1, 2, 3 }));
    // Fewer panes made than were saved: the first ones, still unique.
    QCOMPARE(restoredSlots(s, 2, true), (QList<int>{ 0, 1 }));
    // A session whose host sat in slot 2.
    const PaneSession moved = makeSession(4, 3, 2, cells);
    QCOMPARE(restoredSlots(moved, 4, false), (QList<int>{ 2, 0, 1, 3 }));
}

void TestPaneLayout::restoredSlots_followTheOrientationOfNow()
{
    std::array<PaneSlotState, kSlots> cells;
    cells[0] = { true, u"LogMass"_s, true };
    cells[1] = { true, u"LogT"_s, true };
    cells[2] = { true, u"LogD"_s, true };
    cells[3] = { true, u"vx"_s, true };
    // Saved stacked (slots 0 and 2 shown), restored side by side (0 and 1 shown): the shown
    // pair stays the shown pair and the hidden ones stay hidden.
    const PaneSession stacked = makeSession(2, 2, 0, cells, true);
    const QList<int> to = restoredSlots(stacked, 4, false);
    QCOMPARE(to, (QList<int>{ 0, 2, 1, 3 }));
    QVERIFY(visibleSlots(2, false)[to[0]]);
    QVERIFY(visibleSlots(2, false)[to[2]]);      // LogD, the active pane
    QVERIFY(!visibleSlots(2, false)[to[1]]);
    QVERIFY(!visibleSlots(2, false)[to[3]]);
    // And the other way round.
    const PaneSession wide = makeSession(2, 1, 0, cells, false);
    const QList<int> back = restoredSlots(wide, 4, true);
    QCOMPARE(back, (QList<int>{ 0, 2, 1, 3 }));
    QVERIFY(visibleSlots(2, true)[back[1]]);     // the second pane
    // Four panes (or one) have no orientation: nothing moves.
    QCOMPARE(restoredSlots(makeSession(4, 0, 0, cells, false), 4, true),
             (QList<int>{ 0, 1, 2, 3 }));
}

void TestPaneLayout::restoredSlots_unusableSessionKeepsCreationOrder()
{
    PaneSession s;
    s.count = 2;
    s.panes = { { u"a"_s, true, -1 }, { u"b"_s, true, -1 } };
    QVERIFY(restoredSlots(s, 2, false).isEmpty());          // no slots kept
    s.panes = { { u"a"_s, true, 1 }, { u"b"_s, true, 1 } };
    QVERIFY(restoredSlots(s, 2, false).isEmpty());          // a slot twice
    s.panes = { { u"a"_s, true, 0 }, { u"b"_s, true, 1 } };
    QVERIFY(restoredSlots(s, 3, false).isEmpty());          // more made than saved
    QVERIFY(restoredSlots(s, 0, false).isEmpty());
    QCOMPARE(restoredSlots(s, 2, false), (QList<int>{ 0, 1 }));
}

void TestPaneLayout::filterTargets_linkedPanesOfTheSameTable()
{
    std::array<PaneFilterLink, kSlots> panes;
    panes[0] = { true, true, true };
    panes[1] = { true, true, true };
    panes[2] = { true, false, true };    // unlinked: keeps its own filters
    panes[3] = { true, true, false };    // another table
    QCOMPARE(filterTargets(0, panes), (QList<int>{ 1 }));
}

void TestPaneLayout::filterTargets_anyLinkedPaneCanBeTheSource()
{
    // The filter was edited in a pane that is not the host: the others (host included) follow.
    std::array<PaneFilterLink, kSlots> panes;
    panes[0] = { true, true, true };
    panes[1] = { true, true, true };
    panes[2] = { true, true, true };
    QCOMPARE(filterTargets(2, panes), (QList<int>{ 0, 1 }));
    QCOMPARE(filterTargets(1, panes), (QList<int>{ 0, 2 }));
}

void TestPaneLayout::filterTargets_unlinkedSourceKeepsItsFiltersToItself()
{
    std::array<PaneFilterLink, kSlots> panes;
    panes[0] = { true, true, true };
    panes[1] = { true, false, true };
    QVERIFY(filterTargets(1, panes).isEmpty());
    QVERIFY(filterTargets(3, panes).isEmpty());    // an empty slot is no source
    QVERIFY(filterTargets(-1, panes).isEmpty());
}

void TestPaneLayout::filterTargets_unlinkedPaneIsNeverATarget()
{
    // Four panes, the fourth unlinked (it is the active one when its filter is edited):
    // a filter edited in a linked pane reaches the other linked panes, not the fourth.
    std::array<PaneFilterLink, kSlots> panes;
    panes[0] = { true, true, true };
    panes[1] = { true, true, true };
    panes[2] = { true, true, true };
    panes[3] = { true, false, true };
    QCOMPARE(filterTargets(0, panes), (QList<int>{ 1, 2 }));
    QCOMPARE(filterTargets(2, panes), (QList<int>{ 0, 1 }));
    // And the fourth's own filter reaches nobody.
    QVERIFY(filterTargets(3, panes).isEmpty());
}

void TestPaneLayout::filterTargets_pairWithOneUnlinkedPane()
{
    // The host (slot 0) is the unlinked one: its filter stays with it, and the linked pane
    // beside it has no one to share with.
    std::array<PaneFilterLink, kSlots> panes;
    panes[0] = { true, false, true };
    panes[1] = { true, true, true };
    panes[2] = { true, true, true };
    QVERIFY(filterTargets(0, panes).isEmpty());
    QCOMPARE(filterTargets(1, panes), (QList<int>{ 2 }));
    QCOMPARE(filterTargets(2, panes), (QList<int>{ 1 }));
}

// ── GridState: pop-out / dock-back / maximise rules the cube window always had ──

void TestPaneLayout::grid_detachDockMaximiseState()
{
    GridState g;
    QVERIFY(!g.maximised);
    QCOMPARE(g.detachedCount(), 0);
    QVERIFY(g.canDetach(1));
    QVERIFY(g.detach(1));
    QVERIFY(g.isDetached(1));
    QCOMPARE(g.detachedCount(), 1);
    // Out already: a second pop-out is refused and changes nothing.
    QVERIFY(!g.canDetach(1));
    QVERIFY(!g.detach(1));
    QCOMPARE(g.detachedCount(), 1);
    QVERIFY(g.dock(1));
    QVERIFY(!g.isDetached(1));
    // Back in the grid, it can go out again.
    QVERIFY(g.detach(1));
}

void TestPaneLayout::grid_detachRefusedWhileMaximised()
{
    GridState g;
    g.maximise();
    QVERIFY(g.maximised);
    QVERIFY(!g.canDetach(0));
    QVERIFY(!g.detach(0));
    QCOMPARE(g.detachedCount(), 0);
    g.restore();
    QVERIFY(!g.maximised);
    QVERIFY(g.detach(0));
}

void TestPaneLayout::grid_dockIsIdempotent()
{
    GridState g;
    // Docking a pane that is not out (a stray close event, a second click) does nothing.
    QVERIFY(!g.dock(2));
    QVERIFY(g.detach(2));
    QVERIFY(g.dock(2));
    QVERIFY(!g.dock(2));
    QCOMPARE(g.detachedCount(), 0);
}

void TestPaneLayout::grid_invalidSlotsAreRefused()
{
    GridState g;
    QVERIFY(!g.detach(-1));
    QVERIFY(!g.detach(kSlots));
    QVERIFY(!g.isDetached(-1));
    QVERIFY(!g.isDetached(kSlots));
    QVERIFY(!g.dock(kSlots));
    QCOMPARE(g.detachedCount(), 0);
}

void TestPaneLayout::grid_panesPopOutIndependently()
{
    GridState g;
    QVERIFY(g.detach(0));
    QVERIFY(g.detach(3));
    QVERIFY(g.isDetached(0));
    QVERIFY(!g.isDetached(1));
    QVERIFY(g.isDetached(3));
    QCOMPARE(g.detachedCount(), 2);
    // Maximising with panes out keeps them out; docking one leaves the other.
    g.maximise();
    QVERIFY(g.dock(0));
    QVERIFY(g.isDetached(3));
}

void TestPaneLayout::pauseStream_onlyAHiddenPaneOfAVisibleGrid()
{
    // A pane the layout hides, in a window that is up: its stream is given back.
    QVERIFY(pauseStream(/*inGrid=*/true, /*viewVisible=*/false, /*hostVisible=*/true));
    // Shown: it streams.
    QVERIFY(!pauseStream(true, true, true));
    // A window of its own (no grid), or one that is itself hidden or closing: left alone.
    QVERIFY(!pauseStream(false, false, true));
    QVERIFY(!pauseStream(true, false, false));
    // The grace is long enough for a layout change to move views through hidden.
    QVERIFY(kPauseGraceMs >= 1000);
}

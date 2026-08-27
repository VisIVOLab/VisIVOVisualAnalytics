// test_linked_window_registry.cpp
// Unit tests for LinkedWindowRegistry<W> — the linked-view peer registry
// extracted from vtkWindowCube in E-04 step 2. Uses a fake QObject window so the
// test carries no VTK/UI dependency (matching the suite's Qt-only design).

#include "TestLinkedWindowRegistry.h"
#include "LinkedWindowRegistry.h"

#include <QTest>

namespace {
// Minimal stand-in for a viewer window: LinkedWindowRegistry only needs a
// QObject-derived type so QPointer can track its lifetime.
class FakeWin : public QObject
{
    Q_OBJECT
};
}  // namespace

void TestLinkedWindowRegistry::add_increasesSize()
{
    LinkedWindowRegistry<FakeWin> reg;
    QCOMPARE(reg.size(), 0);
    FakeWin a, b;
    reg.add(&a);
    reg.add(&b);
    QCOMPARE(reg.size(), 2);
}

void TestLinkedWindowRegistry::remove_decreasesSize()
{
    LinkedWindowRegistry<FakeWin> reg;
    FakeWin a, b;
    reg.add(&a);
    reg.add(&b);
    reg.remove(&a);
    QCOMPARE(reg.size(), 1);
    QCOMPARE(reg.snapshot().first().data(), &b);
}

void TestLinkedWindowRegistry::remove_absentIsNoOp()
{
    LinkedWindowRegistry<FakeWin> reg;
    FakeWin a, ghost;
    reg.add(&a);
    reg.remove(&ghost);          // not registered → no effect
    QCOMPARE(reg.size(), 1);
    QCOMPARE(reg.snapshot().first().data(), &a);
}

void TestLinkedWindowRegistry::snapshot_returnsAllInOrder()
{
    LinkedWindowRegistry<FakeWin> reg;
    FakeWin a, b, c;
    reg.add(&a);
    reg.add(&b);
    reg.add(&c);
    const auto snap = reg.snapshot();
    QCOMPARE(snap.size(), 3);
    QCOMPARE(snap.at(0).data(), &a);
    QCOMPARE(snap.at(1).data(), &b);
    QCOMPARE(snap.at(2).data(), &c);
}

void TestLinkedWindowRegistry::snapshot_excludesSelf_viaCallerPredicate()
{
    // The registry does not exclude self; the broadcast loop does. Verify the
    // exact `if (p && p != self)` pattern the window uses picks only the peers.
    LinkedWindowRegistry<FakeWin> reg;
    FakeWin self, peer1, peer2;
    reg.add(&self);
    reg.add(&peer1);
    reg.add(&peer2);
    QList<FakeWin *> peers;
    for (const auto &p : reg.snapshot())
        if (p && p != &self)
            peers.append(p.data());
    QCOMPARE(peers.size(), 2);
    QVERIFY(peers.contains(&peer1));
    QVERIFY(peers.contains(&peer2));
    QVERIFY(!peers.contains(&self));
}

void TestLinkedWindowRegistry::snapshot_autoNullsDestroyedWindow()
{
    // Safety property the broadcast loop relies on: a peer destroyed after the
    // snapshot is taken auto-nulls, so `if (p && ...)` skips it instead of
    // dereferencing a dangling pointer.
    LinkedWindowRegistry<FakeWin> reg;
    FakeWin a;
    auto *b = new FakeWin;
    reg.add(&a);
    reg.add(b);
    auto snap = reg.snapshot();
    QCOMPARE(snap.size(), 2);
    QVERIFY(snap.at(1) != nullptr);   // b alive in the snapshot
    delete b;                          // destroy the peer while the snapshot is held
    QVERIFY(snap.at(1) == nullptr);   // QPointer auto-nulled → loop guard skips it
    QVERIFY(snap.at(0) != nullptr);   // a still alive
    // A live iteration with the null-guard yields only the survivor.
    int live = 0;
    for (const auto &p : snap)
        if (p)
            ++live;
    QCOMPARE(live, 1);
}

#include "test_linked_window_registry.moc"

#pragma once
// TestLinkedWindowRegistry.h
// QTest class for LinkedWindowRegistry<W> (E-04 god-object step 2).
// Header-only, GUI/VTK-free: instantiated with a fake QObject window type.

#include <QObject>

class TestLinkedWindowRegistry : public QObject
{
    Q_OBJECT

private slots:
    void add_increasesSize();
    void remove_decreasesSize();
    void remove_absentIsNoOp();
    void snapshot_returnsAllInOrder();
    void snapshot_excludesSelf_viaCallerPredicate();
    void snapshot_autoNullsDestroyedWindow();
};

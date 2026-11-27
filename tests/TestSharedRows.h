#pragma once
// TestSharedRows.h
// QTest class for SharedRows: the rows a particle window's panes share (#604).

#include <QObject>

class TestSharedRows : public QObject
{
    Q_OBJECT

private slots:
    void copy_sharesTheRows();
    void read_neverCopies();
    void mut_leavesTheOthersAlone();
    void mut_onAPrivateHandleCopiesNothing();
    void assignFromVector_rebindsOnlyThisHandle();
    void empty_handleIsUsable();
    void conversionToVector_isTheSameStorage();
    void resetThenMut_startsFresh();
    void appendThroughMut_afterSharing();
    void update_copiesNothingWhenNoRowNeedsIt();
    void update_writesFromTheFirstRowThatNeedsIt();
    void update_leavesTheOthersTheirRows();
};

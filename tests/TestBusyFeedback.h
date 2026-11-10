#pragma once
// TestBusyFeedback.h
// QTest class for the rules behind a window's busy indicator (BusyTracker.h)
// and the screen size of the 3-D catalogue viewer's selection marker
// (SelectionMarkerSize.h), issue #159. Header-only logic; no window.

#include <QObject>

class TestBusyFeedback : public QObject
{
    Q_OBJECT

private slots:
    void oneScopeShowsAndSaysItsOutcome();
    void nestedScopesJoinTheirOperation();
    void aClickInANestedLoopIsItsOwnOperation();
    void aDetachedOpenOutlivesItsCaller();
    void aDetachedOutcomeIsItsOwn();
    void aScopeWithoutOutcomeClearsItsText();
    void aPauseHidesWithoutEnding();
    void detailFollowsTheSentence();
    void worldPerPixelForBothProjections();
    void markerKeepsToItsPixelRange();
    void unknownScaleLeavesTheMarkerAlone();
};

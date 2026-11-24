#pragma once
// TestLiveRegionSpectra.h
// QTest class for the live region spectrum's throttle, latest-wins tickets,
// full-resolution cache and overlay plan (issue #521). Header-only logic; no window.

#include <QObject>

class TestLiveRegionSpectra : public QObject
{
    Q_OBJECT

private slots:
    void theFirstRequestRunsAtOnce();
    void aBurstRunsOnceAndThenOnTheTrailingEdge();
    void aRequestAfterTheIntervalRunsAgain();
    void aRunOutsideTheThrottleRestartsItsClock();
    void onlyTheLatestTicketIsCurrent();
    void aSwitchedOffRequestIsNeverCurrent();
    void theCacheDropsTheOldestFirst();
    void theSignatureFollowsTheGeometryNotTheName();
    void theOverlayLeavesOutTheSelectedAndWhatHasNoPixels();
};

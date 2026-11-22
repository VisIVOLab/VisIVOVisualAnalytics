#pragma once
// TestSnapshotTimelineSteps.h
// QTest class for SnapshotTimelineSteps (issue #446): the snapshot timeline's
// gap skipping, the quantity kept by name across outputs, the camera kept by
// rebuilds that follow a step, the order a replacement view takes its state
// in, and the copilot routing of views that share a dataset. Header-only pure
// logic plus CopilotActionRouter (QtCore); no GUI/VTK link.

#include <QObject>

class TestSnapshotTimelineSteps : public QObject
{
    Q_OBJECT

private slots:
    void multiPieceCatalogue_hasNoTimeline();
    void step_skipsOutputsThatCannotOpen();
    void step_withoutMarks_takesTheNextOutput();
    void step_pastTheLastOpenable_isNone();
    void slider_landsOnTheNearestOpenable();
    void seriesReply_invalidNeverHidesAValidSeries();
    void seriesReply_keepsTheOutputsThatFailedHere();
    void quantity_isRestoredByName();
    void quantity_missingInTheOutput_isEmpty();
    void quantity_builtInKey_isKept();
    void rebuild_framesFirstThenKeepsTheStepCamera();
    void representation_receiversNeedEveryFlag();
    void restore_unlinkedPointView_doesNotTurnLinkedSplatsIntoPoints();
    void restore_oldOrder_didReachTheLinkedView();
    void copilot_routesEachComparisonViewByItsOwnKey();
};

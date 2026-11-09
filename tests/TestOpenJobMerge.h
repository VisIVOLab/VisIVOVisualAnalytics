#pragma once
// TestOpenJobMerge.h
// QTest class for "Last 5 jobs" with the client's own opens merged in
// (OpenJobMerge.h): matching by time, client-only refusals, order and cap.

#include <QObject>

class TestOpenJobMerge : public QObject
{
    Q_OBJECT

private slots:
    void anOpenTakesTheClientsTime();
    void aRefusalBeforeTheBackendIsListed();
    void theOpenRouteIsPreferredOverTheDecompression();
    void aCancelledDecompressionMatchesItsTask();
    void twoOpensClaimTheirOwnJobs();
    void anOpenWithoutItsBackendJobIsNotInvented();
    void otherJobsAreLeftAlone();
    void timeOrderAndCap();
    void anOpenAfterTheAnswerWaitsForTheNext();
    void anEarlierOpenDoesNotTakeTheNextOnesJob();
    void aMatchedOpenIsOrderedByItsOwnEnd();
};

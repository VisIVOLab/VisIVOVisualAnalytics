#ifndef TestBackendTaskFollow_h
#define TestBackendTaskFollow_h

#include <QObject>

class TestBackendTaskFollow : public QObject
{
    Q_OBJECT

private slots:
    void aLongTaskIsFollowedToItsEnd();
    void theBackOffStopsAtItsCeiling();
    void aFailedTaskDoesNotFallBackToSync();
    void aCancelledTaskDoesNotFallBackToSync();
    void aLostTaskIsCancelledAndDoesNotFallBackToSync();
    void aShortOutageIsRidden();
    void anAbortSendsOneDeleteAndStopsPolling();
    void anAbortTheBackendRefusesIsReported();
    void anAbortThatCutsAReadShortIsNotAnOutage();
    void anAbortRacingACompletedAnswerWins();
    void taskCreationKeepsTheHttpStatusForTheFallback();
    void aMissingTaskRouteFallsBackToSync();
    void otherCreationFailuresDoNotFallBack();
    void aCancelDuringCreationOnAnOldBackendStartsNothing();
    void aPendingTaskReportsQueued();
    void theProgressTextSaysStateAndTime();
};

#endif // TestBackendTaskFollow_h

#pragma once
// TestTaskQuitDrain.h
// QTest class for the quit drain (TaskQuitDrain.h): quitting aborts every
// followed task and returns only once its worker has finished cancelling.

#include <QObject>

class TestTaskQuitDrain : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void drainWaitsForTheWorkersCancellation();
    void aFollowStartedWhileQuittingIsAbortedAtOnce();
    void drainIsBoundedByItsTimeout();
    void nothingToDrainReturnsAtOnce();
    void aSecondDrainWaitsOnlyWhatIsLeft();
    void aWorkerStillCreatingItsTaskIsWaitedFor();
    void queuedWorkRunsAndIsWaitedFor();
    void aWorkerWaitingOnAQueuedFutureFinishes();
    void aQueuedFollowerOfACreatedTaskIsAborted();
    void noTaskIsCreatedWhileQuitting();
    void aFollowedTaskIsCancelledOnTheBackendBeforeTheDrainEnds();
    void theCreateCallsSendNothingWhileQuitting();
    void aSlowRequestIsDroppedOnQuit();
    void theQuitsDeleteIsBounded();
    void aSourceFinderRunIsCancelledOnQuit();
    void aTaskWhoseCreateIsStillAnsweringIsCancelled_data();
    void aTaskWhoseCreateIsStillAnsweringIsCancelled();
    // Ray-traced renders, followed on the GUI thread (RenderTaskHandle, #149).
    void aRunningRenderGetsOneDeleteOnQuit();
    void aRenderAnsweringAtQuitIsDeletedByItsWorker();
    void aRenderRequestedWhileQuittingSendsNothing();
    void aRenderForgottenWhileCreatingIsDeleted();
    void aCancellerIsCalledOnEveryDrainAndUnregisters();
};

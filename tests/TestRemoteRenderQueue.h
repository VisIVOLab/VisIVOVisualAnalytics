#pragma once
// TestRemoteRenderQueue.h
// QTest class for waiting in the backend's render queue (issue #4): reading the
// create_session answers (a place vs a pyramid build, 410, 503), the place kept
// between requests, and what a stale answer leaves to give back.
// A live stream that drops: its reconnection (issue #411) and the watchdog that
// tells a silent stream. Header-only logic; no window, no network.

#include <QObject>

class TestRemoteRenderQueue : public QObject
{
    Q_OBJECT

private slots:
    void aSessionIsRead();
    void aPlaceInTheQueueIsNotAPyramidBuild();
    void aPyramidBuildIsNotAPlace();
    void aPlaceWithoutIdIsAFailure();
    void goneAndRefusedAreTold();
    void theFirstRequestAsksToWaitWithoutAPlace();
    void thePlaceGoesBackUntilASlotIsFree();
    void aGonePlaceAsksAgainAtTheBack();
    void aRefusalDropsThePlace();
    void aFailureGivesThePlaceBack();
    void aStaleSessionIsDeleted();
    void aStalePlaceIsCancelled();
    void stoppingGivesThePlaceUpOnce();
    void pollDelayStaysWithinTheExpiry();
    void theBannerSaysWhereTheViewIs();
    void aKilledSessionIsReplacedThenRetryIsOffered();
    void anotherDropOffersRetry();
    void aDropThatWasNotLiveIsLeftToTheSwitch();
    void anUnreachableBackendIsAskedAgainWithGrowingWaits();
    void aFailedAttemptWaitsThenAsksAgain();
    void aLinkThatComesAndGoesIsNotReconnectedForEver();
    void cancellingEndsTheReconnection();
    void theReconnectionSaysWhereItIs();
    void aSilentStreamIsTakenForLost();
    void aLateCheckIsNotSilence();
    void bothTransportsSayHowTheStreamEnded();
    void aFailedReplacementSaysWhyWithoutBlamingTheBackend();
    void onlyAReplacementsFailureOffersRetry();
    void aDropCheckAfterLocalChangesNothing();
};

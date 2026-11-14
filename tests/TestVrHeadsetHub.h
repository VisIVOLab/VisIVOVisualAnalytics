#pragma once
// TestVrHeadsetHub.h
// The desktop's side of the headset hub (issue #238, backend #236): what the
// hub's routes answer and what the desktop makes of it (app/VrHeadsetHub.h),
// BackendClient's three calls and the shared HeadsetHubLink against a fake
// backend, and the request bodies against the backend's own request models.

#include <QObject>

class TestVrHeadsetHub : public QObject
{
    Q_OBJECT

private slots:
    void presenceIsParsedNewestFirst();
    void anOldBackendIsNotAMissingHeadset();
    void statusTextSaysWhatIsOnShow();
    void longNamesGetBreakPoints();
    void showBodyCarriesTheCubeButNoToken();
    void commandAnswersAreUnderstood();
    void activityIsParsed();
    void lookAndClipAreCoalescedPerCube();
    void theListIsCappedNewestFirst();
    void aRestartedBackendsEventsComeFirst();
    void productsOpenWhereTheyBelong();
    void productRecipeBecomesProvenance();
    void productIdIsStableAcrossOpens();
    void aMomentOfASelectionNamesItsMask();
    void spectrumFileIsRead();
    void spectrumFitsTableIsReadLikeTheCsv();
    void apertureFitsTableIsAMean();
    void brokenFitsTablesAreRefused();
    void lookAndClipAreRead();
    void colormapsMapToTheDesktops();
    void followingTheLookIsOnUntilUnticked();
    void theLookIsFollowedWhileTheCubeIsOnShow();
    void aFollowedLookIsNeverAppliedTwice();
    void followedChangesAreSaid();
    void clientCallsTheHubRoutes();
    void linkFollowsPresenceAndActivity();
    void linkStopsQuietlyOnAnOldBackend();
    void bodiesMatchTheBackendsRequestModels();
};

#pragma once
// TestVrPair.h
// QTest class for pairing a headset by code in "Open in VR Headset…" (issue
// #214): what the /v1/vr/pair routes answer and what the dialog makes of it
// (app/VrPair.h), the QR code of the page (app/VrPairQr.h), the settings
// handed to a backend started here, and BackendClient's three calls against
// a fake backend.

#include <QObject>

class TestVrPair : public QObject
{
    Q_OBJECT

private slots:
    void requestBodyCarriesTheLinkValuesButTheToken();
    void expiryIsReadAsEpochOrIso();
    void expiryFollowsTheServersClock();
    void createAnswerIsUnderstood();
    void conflictReasonsOpenTheUsbWay_data();
    void conflictReasonsOpenTheUsbWay();
    void otherRefusalsAreFailures_data();
    void otherRefusalsAreFailures();
    void statusAnswerIsParsed();
    void codeIsGroupedForReading();
    void countdownRoundsUp();
    void qrPayloadIsThePageWithThisCode();
    void qrEncodesThePayload();
    void waitingThenConnectedThenDisconnected();
    void unusedCodeExpires();
    void codeUsedJustBeforeExpiryStillConnects();
    void codeStillWaitingBesideAnEarlierHeadset();
    void usedCodeWithoutHeadsetIsLost();
    void usedCodeThenHeadsetConnects();
    void otherDatasetsHeadsetIsNotOurs();
    void droppedPollChangesNothing();
    void pairingSwitchedOffOpensTheUsbWay();
    void closingRevokesOnlyAnUnusedCode();
    void statusTextFollowsTheState();
    void silentBackendIsSaid();
    void pollGateLetsOnePollOutAndResets();
    void headsetConnectedBeforeTheCodeIsNotTheNewOne();
    void onlyARealRefreshOfTheCurrentTokenIsKept();
    void serviceSettingsReachTheBackendUnlessTheEnvironmentSetsThem();
    void clientCallsTheThreeRoutes();
    void aFailingPollIsLoggedOncePerOutage();
};

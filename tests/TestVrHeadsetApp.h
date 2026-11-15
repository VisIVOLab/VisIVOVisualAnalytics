#pragma once
// TestVrHeadsetApp.h
// The VisIVO VR headset app from the desktop (issue #255): the APK and its
// visivo-vr.json, the adb commands and their output, the assetlinks check,
// the ticket, the launch tracker and the one-launch rule (app/VrHeadsetApp.h),
// and VrAppLauncher's whole chain against a fake adb and a fake backend.

#include <QObject>

class TestVrHeadsetApp : public QObject
{
    Q_OBJECT

private slots:
    // VrHeadsetApp
    void appInfoIsReadFromTheJson();
    void apkIsFoundInOrder();
    void installedVersionNeedsTheExactPackage();
    void installStateComparesVersions();
    void installOutputIsUnderstood();
    void assetlinksMustListPackageAndCertificate();
    void releaseAppIsListedByTheShippedConfig();
    void assetlinksAreAskedAtTheOrigin();
    void wakefulnessIsRead();
    void launchBodyCarriesTheCubeButNoToken();
    void ticketAnswerIsChecked();
    void amStartIsTheContractsCommand();
    void ticketsAreRedacted();
    void launchStatusIsParsed();
    void trackerReachesInVr();
    void trackerTimesOutWithoutExchange();
    void trackerTimesOutWithoutHello();
    void trackerIgnoresAnswersAfterTheEnd();
    void gateSendsOneLaunchPerTicket();
    void aConnectedPageIsShownNotLaunchedAgain();
    void presenceCarriesTheLaunch();

    // VrAppLauncher, against a fake adb and a fake backend
    void launcherInstallsThenLaunchesOnce();
    void launcherShowsInsteadOfASecondLaunch();
    void launcherAsksBeforeUninstalling();
    void aCheckedReceiverMayStartTheNextRun();
    void launcherWaitsForAnAsleepHeadset();
    void launcherSaysPutTheHeadsetOnWhenTheLaunchIsDropped();
    void launcherSaysWhenThePageNeverConnects();
    void launcherRefusesABackendThatDoesNotListTheApp();
    void launchBodyMatchesTheBackendsRequestModel();
};

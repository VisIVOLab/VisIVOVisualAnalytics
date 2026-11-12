#pragma once
// TestVrLink.h
// QTest class for "Open in VR Headset" (issue #172): the link to the
// backend's WebXR page (app/VrLink.h) and what is run through adb to open it
// (app/AdbHeadset.h) — the fragment's encoding, the port taken from the
// client's backend URL, the token kept out of what is shown and logged, the
// `adb devices` states, and the quoting for the headset's shell.

#include <QObject>

class TestVrLink : public QObject
{
    Q_OBJECT

private slots:
    // VrLink
    void linkCarriesEverythingInTheFragment();
    void valuesAreFullyEncoded();
    void portComesFromTheBackendUrl_data();
    void portComesFromTheBackendUrl();
    void aPathPrefixIsKeptInThePageAndInfoUrls_data();
    void aPathPrefixIsKeptInThePageAndInfoUrls();
    void aTunnelledRemoteBackendIsLabelledRemote();
    void backendCheckReadsTheAnswer();
    void aBackendNotOnLoopbackHasNoLink_data();
    void aBackendNotOnLoopbackHasNoLink();
    void noDatasetNoLink();
    void emptyTokenAndSessionAreLeftOut();
    void kindAndBoxOpenOtherDataTypes();
    void pageUrlHoldsNoSecret();
    void redactHidesRawAndEncodedSecrets();

    // AdbHeadset
    void parsesDevicesList_data();
    void parsesDevicesList();
    void statusOfDevices();
    void shellQuoteSurvivesTheShell_data();
    void shellQuoteSurvivesTheShell();
    void commandArgumentLists();
    void packageListedNeedsTheExactName();
    void viewStartedReadsAmOutput();
    void findsAdbOnPathThenInSdk();

    // Next-step guidance (epic #166)
    void installCommandPerPlatform();
    void readsQuestOnUsbFromSystemProfiler();
    void guidanceNamesTheNextStep();
    void prefersTheLastHeadset();
};

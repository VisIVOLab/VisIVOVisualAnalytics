#pragma once
// TestBackendWebrtcEnv.h
// When the launcher adds the WebRTC packages (headset pairing, stereo video)
// to a user-data venv made without them (app/BackendWebrtcEnv.h, issue #305).

#include <QObject>

class TestBackendWebrtcEnv : public QObject
{
    Q_OBJECT

private slots:
    void onlyTheUserDataVenvWithoutAiortcIsUpdated();
    void aFailedUpdateWaitsForANewVersionOrAWeek();
    void theRequirementsSitNextToTheBackends();
    void aVenvSeenWithAiortcIsNotCheckedAgain();
    void theUpdatePinsWhatIsInstalled();
    void pipNeverPromptsAndOnlyTheBootstrapUpgrades();
};

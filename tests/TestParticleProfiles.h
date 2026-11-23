#ifndef TestParticleProfiles_h
#define TestParticleProfiles_h

// TestParticleProfiles.h
// The radial profile and phase diagram requests and answers (issue #426):
// app/ParticleProfiles.h, and BackendClient following their tasks against a
// mock backend.

#include <QObject>

class TestParticleProfiles : public QObject
{
    Q_OBJECT

private slots:
    void theProfileBodyCarriesTheCentreAndOnlyTheRangesGiven();
    void thePhaseBodySendsACentreOnlyForRadialAxes();
    void aProfileAnswerIsParsedWithEmptyShellsAsNaN();
    void aProfileWithShellsOfDifferentLengthsIsRefused();
    void aPhaseAnswerIsParsedAndItsTotalsClose();
    void theCsvFilesCarryTheirProvenance();
    void theProfileRunsAsATask();
    void thePhaseDiagramRunsAsATaskAndCancels();
    void anOlderBackendIsToldToUpdate();
    void theGeometryGoesInTheBodyWithItsSlab();
    void cylindricalAndAxialAnswersAreParsed();
    void theTotalsRunAsATaskWithReasonsForMissingValues();
};

#endif

#pragma once
// TestSimulationVolume.h
// QTest class for app/SimulationVolume.h: slice axes, coordinate -> plane,
// labels, and the physical units of a simulation run (issue #524).

#include <QObject>

class TestSimulationVolume : public QObject
{
    Q_OBJECT

private slots:
    void planeCount_followsTheAxis();
    void planeAxes_areTheTwoAxesLeft();
    void nearestPlane_picksTheClosestAndFlagsTheOffGrid();
    void nearestPlane_handlesADecreasingAxisAndNoSpacing();
    void labels_nameTheAxisAndTheUnit();
    void physicalUnits_parseWhatTheBackendSends();
    void physicalUnits_scaleOnlyWhenOnAndKnown();
    void timeLabel_isCodeOrPhysical();
    void unitsRequest_sendsOnlyWhatIsSet();
};

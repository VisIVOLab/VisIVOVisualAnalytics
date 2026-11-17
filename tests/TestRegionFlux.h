#pragma once
// TestRegionFlux.h
// QTest class for the beam-aware region flux (issue #401): the unit of a
// moment-0 map, the beam area, the flux and its error with a background, the
// HI mass and the typed distance, the background region in the list.
// Header-only logic; no window.

#include <QObject>

class TestRegionFlux : public QObject
{
    Q_OBJECT

private slots:
    void unitsOfBrightnessAndMoments();
    void beamAreaIsTheGaussianSolidAngle();
    void fluxOfAGaussianSourceIsItsKnownFlux();
    void backgroundIsSubtractedAndGivesTheError();
    void hcg44NumbersOfTheUseCase();
    void reducedMapsCountBlocks();
    void madSigmaIsRobust();
    void nearestBackgroundIsChosen();
    void backgroundFlagRoundTripsAndUndoes();
    void distancesAndMasses();
    void fluxAndMassText();
};

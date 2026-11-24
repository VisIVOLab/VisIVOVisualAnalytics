#pragma once
// TestIsoLevels.h
// QTest class for the isosurface levels (IsoLevels.h, issue #526) and the
// decoding of the backend's answer with one surface per level. No window.

#include <QObject>

class TestIsoLevels : public QObject
{
    Q_OBJECT

private slots:
    void linearLevelsIncludeBothEnds();
    void logLevelsAreEvenInTheirLogarithm();
    void oneLevelIsTheMiddleOfTheRange();
    void aReversedRangeIsReadTheRightWayRound();
    void logNeedsARangeAboveZero();
    void moreThanTheMaximumIsRefused();
    void aTypedListIsSortedWithoutDuplicates();
    void aTypedListRefusesWhatItCannotRead();
    void opacityFallsOutwardFromAnOpaqueInnerSurface();
    void colourComesFromTheMapAtEachValue();
    void regeneratingKeepsTheColoursNotTheOpacities();
    void aSimulationRangeStartsLog();
    void theAnswerKeepsOneMeshPerLevel();
    void anOlderAnswerHasOnlyTheFirstSurface();
};

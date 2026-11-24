#pragma once
// TestContourSettings.h
// QTest class for the contour settings shared by the image, external-file and
// cube-slice contours (issues #519, #49): level rules, noise, smoothing and the
// ds9 regions written from the lines. Header-only logic; no window, no VTK.

#include <QObject>

class TestContourSettings : public QObject
{
    Q_OBJECT

private slots:
    void countLevelsHaveBothEnds();
    void aSingleLevelIsTheLowerBound();
    void sigmaLevelsAreMultiplesOfTheNoise();
    void negativeSigmaLevelsAreMirrored();
    void sigmaNeedsTheNoise();
    void logarithmicLevelsAreAGeometricProgression();
    void logarithmicLevelsNeedAPositiveLowerBound();
    void aListIsSortedAndWithoutRepeats();
    void aBadListIsAProblem();
    void noiseIsTheMadAndIgnoresBrightSources();
    void nearestLevelPicksTheClosest();
    void smoothingBlursAndKeepsTheFlux();
    void smoothingOfAConstantIsTheConstant();
    void smoothingKeepsBlanksAndIgnoresThemInNeighbours();
    void noSmoothingIsACopy();
    void closedLinesArePolygonsAndOpenOnesSegments();
    void regionsCarryColourWidthDashAndLevel();
    void verticesTableFollowsTheRegions();
    void verticesThatAreNotNumbersCutTheLines();
    void linesWithTooFewFinitePointsAreSkipped();
    void sigmaIsRecordedInTheRegions();
    void segmentsAreJoinedIntoLines();
    void segmentsWithABadEndSplitTheLine();
    void joinedLinesCarryLevelColourAndDash();
    void dashesFollowTheLengthAndRoundCorners();
    void largeImagesAreThinned();
    void signedMultiplesGiveNegativeLevels();
    void defaultLevelsAreThreeFiveTenTwentySigma();
    void radioImagesAlsoGetMinusThreeSigmaDashed();
    void aMaskedMomentMapFallsBackToLogLevels();
    void aSimulationFieldFallsBackToLogLevels();
    void theLevelsInUseAreDescribed();
};

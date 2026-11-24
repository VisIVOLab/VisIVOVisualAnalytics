#pragma once
// TestRegionMeasure.h
// QTest class for the region measurements of a continuum image (issue #503):
// the statistics rows (sum, RMS and standard deviation named apart, the peak
// and its position), point and line regions, the line profile's CSV and rows,
// and the rows of the image noise. Header-only logic (RegionMeasure.h).

#include <QObject>

class TestRegionMeasure : public QObject
{
    Q_OBJECT

private slots:
    void rmsAndStdAreNamedApart();
    void unweightedRmsFromTheSumOfSquares();
    void statisticsRowsCarryTheSumInTheImageUnit();
    void peakRowsGivePixelAndWorld();
    void pointRowsSayBlank();
    void pointAndLineRegionsAreRegionsOfTheList();
    void aLineThatWentNowhereIsNotARegion();
    void profileRowsSummariseTheLine();
    void noiseRowsOfAClippedImage();
    void noiseRowsOfBackgroundRegions();
    void noiseWithoutABeamHasNoConversion();
};

#pragma once
// TestSkyWcs.h
// QTest class for the sky-matching rules of issue #522: the celestial WCS read
// from header cards (checked against astropy's numbers), the matching of a
// view between two images of different projection, pixel scale, rotation and
// frame, and the conversion of regions between them. Header-only logic; no
// FITS, no VTK.

#include <QObject>

class TestSkyWcs : public QObject
{
    Q_OBJECT

private slots:
    void projectionsAgreeWithAstropy_data();
    void projectionsAgreeWithAstropy();
    void skyToPixelInvertsPixelToSky_data();
    void skyToPixelInvertsPixelToSky();
    void headersWithoutAUsableSolutionAreRefused();
    void aPixelBeyondTheSinHorizonHasNoSky();
    void matchViewKeepsCentreAndAngularHeight();
    void matchViewAcrossFramesAndProjections();
    void matchViewThereAndBackIsTheIdentity();
    void pixelScaleIsArcsecondsPerPixel();
    void regionsKeepTheirSizeOnTheSky();
    void aBoxTurnsWithTheOtherGrid();
    void polygonAndLineRoundTrip();
    void aRegionOffTheProjectionIsNotPlaced();
    void pathConversionFailsAsAWhole();
};

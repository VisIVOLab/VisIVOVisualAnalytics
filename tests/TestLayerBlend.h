#pragma once
// TestLayerBlend.h
// QTest class for the Additive blend arithmetic (LayerBlend.h): the sum of
// layer colours, clipping, weights, the hue given to each layer of a composite,
// and the resampling of a layer onto the base image's grid.

#include <QObject>

class TestLayerBlend : public QObject
{
    Q_OBJECT

private slots:
    void threeBlackToHueLayersSumToTheirMix();
    void equalFullLayersReachWhite();
    void aSumPastOneClips();
    void opacityIsTheWeightOfTheLayer();
    void aZeroWeightLayerAddsNothing();
    void aLayerWithoutDataLeavesThePixelTransparent();
    void aBlackPixelWithDataIsOpaque();
    void threeLayersAreRedGreenBlue();
    void fourAndFiveLayersRunRedToBlue();
    void moreThanFiveLayersCycle();

    void sameGridResamplesUnchanged();
    void aShiftedLayerLandsOnTheBaseGrid();
    void aCoarserLayerIsScaledToTheBaseGrid();
    void aRotatedLayerIsSampledWhereItLies();
    void outsideTheLayerIsBlankAndBlanksDoNotBleed();
    void aSingularMapGivesBlank();
    void reducedDisplayAveragesTheBlock();
    void displayStepFitsTheBudget();
    void affineInverseRoundTrips();
};

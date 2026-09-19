#pragma once
// TestSkyOverlap.h
// QTest class for the sky-overlap rule that decides whether a FITS file can be
// added as a layer to the image on screen. Header-only logic; no FITS, no VTK.

#include <QObject>

class TestSkyOverlap : public QObject
{
    Q_OBJECT

private slots:
    void twoImagesOfTheSameFieldOverlap();
    void adjacentFieldsDoNotOverlap();
    void touchingEdgesAreNotAnOverlap();
    void aContainedFieldOverlaps();
    void declinationAloneIsNotEnough();
    void aFieldStraddlingRaZeroOverlapsItsNeighbourOnEitherSide();
    void twoFieldsStraddlingRaZeroOverlapEachOther();
    void aWrappedFieldStillMissesAFarAwayOne();
    void invertedDeclinationBoundsAreReadAsARange();
    void boundsWithoutAWcsSolutionRefuse();
};

#pragma once
// TestVlkbPosString.h
// QTest class for the VLKB POS string: galactic longitude wrapping, latitude
// clamping, and the RANGE box that crosses l = 0. Header-only logic; no GUI,
// no network.

#include <QObject>

class TestVlkbPosString : public QObject
{
    Q_OBJECT

private slots:
    void aNegativeLongitudeWrapsByAFullTurnNotAHalf();
    void aLongitudePastThreeSixtyComesBackRound();
    void latitudeIsClampedNotWrapped();
    void aCircleCarriesItsRadiusAsAPositiveNumber();
    void aBoxAwayFromZeroIsAnOrdinaryInterval();
    void aBoxAcrossZeroIsEmittedWrappedAndFlagged();
    void nonFiniteInputDoesNotProduceGarbage();
    void coordinatesKeepEnoughPrecisionForASmallBox();
    void aFullCircleBoxCoversEveryLongitude();
};

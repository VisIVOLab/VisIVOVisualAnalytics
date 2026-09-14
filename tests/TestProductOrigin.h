#pragma once
// TestProductOrigin.h
// QTest class for ProductOrigin — the geometry a product was generated from.
// Header-only pure logic; no GUI link.

#include <QObject>

class TestProductOrigin : public QObject
{
    Q_OBJECT

private slots:
    void boxAndCircle_roundTripTheirTwoCorners();
    void annulus_roundTripsItsInnerRadius();
    void polygon_keepsEveryVertexAsAPair();
    void probe_roundTripsItsVoxel();
    void pvPath_roundTripsItsPointsAndWidth();
    void pvPath_needsTwoPointsToBeAPath();
    void unknownMode_encodesToNothing();
    void malformedRecord_refusesRatherThanRestoringHalfAGeometry();
    void describeOrigin_readsAsASentence();
};

#pragma once
// TestCubeRegionGeom.h
// QTest class for the CubeRegionGeom pure hit-testers (E-04: made inline +
// tested). Header-only pure geometry; no GUI/VTK link.

#include <QObject>

class TestCubeRegionGeom : public QObject
{
    Q_OBJECT

private slots:
    void bounds_circleCoversTheWholeDisc();
    void bounds_annulusUsesTheOuterRadius();
    void bounds_boxAndPolygonSpanTheirCorners();

    void box_insideEdgesInclusive();
    void box_outside();
    void box_anchorCurrentOrderIndependent();

    void circle_insideAndBoundary();
    void circle_outside();
    void circle_degenerateRadiusOnlyCentre();

    void polygon_insideSquare();
    void polygon_outsideSquare();
    void polygon_fewerThan3VerticesIsFalse();
    void polygon_pixelCentreSampling();
    void polygon_horizontalEdgeNoCrash();

    void annulus_insideRing();
    void annulus_inHoleIsFalse();
    void annulus_outsideOuterIsFalse();
    void annulus_degenerateOuterIsFalse();
};

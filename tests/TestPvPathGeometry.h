#ifndef TestPvPathGeometry_h
#define TestPvPathGeometry_h

#include <QObject>

class TestPvPathGeometry : public QObject
{
    Q_OBJECT

private slots:
    void hitVertexPicksTheNearestWithinTolerance();
    void hitSegmentReportsSegmentAndPosition();
    void translateAndRotate();
    void insertAndRemoveVertices();
    void smoothCurvePassesThroughControlVertices();
    void smoothCurveOfTwoVerticesIsTheLine();
    void descendingSpectralAxisRowsAreReversed();
    void nonLinearSpectralAxisIsPlacedByValue();
};

#endif

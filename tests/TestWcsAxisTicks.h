#pragma once
// TestWcsAxisTicks.h
// QTest class for where a 2-D view's WCS axis ticks are read (WcsAxisTicks.h):
// each axis along its own edge, not on the image diagonal.

#include <QObject>

class TestWcsAxisTicks : public QObject
{
    Q_OBJECT

private slots:
    void theXAxisIsReadAlongTheBottomEdge();
    void theYAxisIsReadAlongTheLeftEdge();
    void aRotatedFrameLabelsTheLeftEdgeNotTheDiagonal();
};

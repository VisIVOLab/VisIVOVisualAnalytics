#pragma once
// TestRegionFullRes.h
// QTest class for handing a region drawn on a reduced display to the backend in
// full-resolution pixels (issue #95). Header-only logic; no window.

#include <QObject>

class TestRegionFullRes : public QObject
{
    Q_OBJECT

private slots:
    void atFullResolutionTheRegionIsUnchanged();
    void aBoxCoversWholeBlocks();
    void circlesAndAnnuliScaleTheirRadii();
    void polygonVerticesGoToBlockCentres();
};

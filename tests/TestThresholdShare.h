#pragma once
// TestThresholdShare.h
// QTest class for the share of voxels above the 3-D threshold, the caption
// under the Threshold slider (issue #83). Header-only logic; no window.

#include <QObject>

class TestThresholdShare : public QObject
{
    Q_OBJECT

private slots:
    void theShareCountsOnlyRealVoxels();
    void aLargeCubeIsSampled();
    void theTextShowsSmallShares();
};

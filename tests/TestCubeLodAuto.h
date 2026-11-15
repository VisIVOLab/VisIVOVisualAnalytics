#pragma once
// TestCubeLodAuto.h
// QTest class for CubeLodAuto and CubeLodPick, the camera-driven level of
// detail of the 3-D cube view (issue #321, phase 4): the camera in the
// planner's terms, the fetch decision, the block cache, and the pick through
// a refined view. Pure logic; no GUI/VTK link.

#include <QObject>

class TestCubeLodAuto : public QObject
{
    Q_OBJECT

private slots:
    void visibleBox_coversEveryTouchedVoxel();
    void visibleBox_blockMeanPreview();
    void visibleBox_offTheCubeOrNotFinite();
    void voxelsPerPixel_perspectiveAndParallel();
    void voxelsPerPixel_unusableCamera();
    void visibleBoxAndZoom_driveThePlan();
    void signature_ignoresNodeOrder();
    void decide_table();
    void lru_evictsLeastRecentlyUsed();
    void lru_capacityAndOversize();
    void lru_blockKeysSeparateDatasetsAndLevels();
    void pick_fineInsideTheBoxCoarseElsewhere();
    void pick_firstAboveThresholdStopsAtTheFront();
    void pick_maximumAcrossPieces();
};

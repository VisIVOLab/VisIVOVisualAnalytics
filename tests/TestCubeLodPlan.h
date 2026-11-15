#pragma once
// TestCubeLodPlan.h
// QTest class for CubeLodPlan: which finer level, over which box, the 3-D view
// shows on top of the coarse preview (issue #321). Pure logic; no GUI/VTK link.

#include <QObject>

class TestCubeLodPlan : public QObject
{
    Q_OBJECT

private slots:
    void pixelLevel_isFloorLog2();
    void level_followsTheZoom();
    void level_coarsensToFitTheBudget();
    void noRefine_whenTheLevelIsAsCoarseAsThePreview();
    void pyramidWithFewLevels_usesItsLast();
    void shrink_aroundTheFocalPoint();
    void snapping_isOutwardToTheNodeGrid();
    void textureSide_capsTheNodes();
    void maxBlocks_isRespected();
    void cameraOffTheCube();
    void budgetTooSmall_forOneNode();
    void badInputs();
    void planBox_refinesTheSelectedBox();
};

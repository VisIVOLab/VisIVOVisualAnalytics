#pragma once
// TestCubeLodGeometry.h
// QTest class for CubeLodGeometry: the coordinates of the 3-D view's level of
// detail (issue #321). Header-only pure logic; no GUI/VTK link.

#include <QObject>

class TestCubeLodGeometry : public QObject
{
    Q_OBJECT

private slots:
    void levelShapes_matchTheBackendPyramid();
    void levelShape_halvesRoundingUp();
    void sampling_previewOffsets();
    void placement_landsOnTheAveragedVoxels();
    void nodes_gridBoxesAndSnapping();
    void ghosted_clampsTheReadAtTheCube();
    void bracketingSamples_isTheGhostedLevelRange();
    void cropPlanes_matchTheBackend();
    void cropPlaneFor_drawsTheFaceAskedFor();
    void kdSplit_coversTheCubeExactlyOnce();
    void kdOrder_isBackToFrontFromEveryOctant();
    void nodeOrder_isBackToFrontInsideTheBox();
    void nodeOrder_mixedLevelsFollowTheOctree();
};

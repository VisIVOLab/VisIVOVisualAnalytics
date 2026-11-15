#pragma once
// TestCubeLodAssembly.h
// QTest class for CubeLodAssembly (the volumes of a refined 3-D view, issue
// #321), the status sentences of CubeLodStatus (#320, #321) and the parsing
// of the pyramid_status answer. Pure logic; no GUI/VTK link.

#include <QObject>

class TestCubeLodAssembly : public QObject
{
    Q_OBJECT

private slots:
    void blocks_ownTheCubeExactlyOnce();
    void blocks_reachTheFacesTheyDraw();
    void cutSamples_repeatsTheEdge();
    void placement_putsSamplesWhereThePreviewShowsThem();
    void neighbours_meetOnOneWorldFace();
    void userCrop_hidesBlocksOutsideIt();
    void drawOrder_isBackToFrontForEveryEye();
    void statusText_namesThePreviewAndTheDetail();
    void pyramidStatus_isParsedAsXyz();
};

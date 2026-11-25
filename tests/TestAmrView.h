#pragma once
// TestAmrView.h
// QTest class for app/AmrView.h: the plan the backend sends for the uniform
// cube of an AMR output, the requests, the level map and how it lands on a
// slice, the box of voxels as a region, and the AMR* header cards (#550).

#include <QObject>

class TestAmrView : public QObject
{
    Q_OBJECT

private slots:
    void parsePlan_readsTheBackendReply();
    void parsePlan_refusesAReplyWithoutAGrid();
    void parsePlan_unknownFinestLevelIsMinusOne();
    void summary_saysLevelMemoryAndWhyItDoesNotFit();
    void requestBody_sendsALevelOnlyWhenChosen();
    void requestBody_carriesTheRegionAndItsUnit();
    void parseCubes_listsTheOutput();
    void regionFromVoxelBox_isTheOuterFacesOfTheEndVoxels();
    void regionFromVoxelBox_handlesADescendingAxis();
    void regionFromVoxelBox_refusesAnAxisWithoutAStep();
    void parseLevelMap_groupsRectanglesByLevel();
    void rectToVoxels_putsCellEdgesOnHalfPixels();
    void planeAxes_dropTheNormal();
    void levelColour_runsCoarseToFine();
    void cardValue_readsStringsAndNumbers();
    void cardValue_joinsContinuedStrings();
    void provenanceFromCards_needsTheSourceCard();
};

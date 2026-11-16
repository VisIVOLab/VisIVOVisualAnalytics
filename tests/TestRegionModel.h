#pragma once
// TestRegionModel.h
// QTest class for the per-window region list (issue #341): geometry, hit tests,
// handle drags, undo, the canonical JSON and the frame text of the properties
// editor. Header-only logic; no window.

#include <QObject>

class TestRegionModel : public QObject
{
    Q_OBJECT

private slots:
    void containsFollowsTheBackendRules();
    void aBoxFromCornersHoldsTheSamePixelsAsBefore();
    void displayedBoundsAreTheVoxelsInside();
    void boundsCoverRotatedShapes();
    void hitTestPrefersSelectedHandlesThenTopmostBody();
    void movingShiftsEveryPosition();
    void boxCornerDragKeepsTheOppositeCorner();
    void radiiAxesAndRotationFollowTheCursor();
    void polygonVertexDrag();
    void sizesNeverCollapse();
    void listAddsNamesAndSelects();
    void undoAndRedoRestoreTheList();
    void aDragIsOneUndoStep();
    void canonicalRoundTripEveryShape();
    void canonicalRejectsSkyAndDegenerateRegions();
    void sexagesimalFormatting();
    void coordinateParsing();
    void framesOfferedFollowTheWcs();
    void fieldsRoundTripThroughCanonical();
    void fieldsReportWhatIsWrong();
    void cubeSamplesFollowTheDisplaySampling();
    void addAllIsOneUndoStep();
    void newShapesRoundTripAndGeometry();
    void importKeepsEveryRegionAndSaysWhy();
    void planeAndRegionsShareOnePlacement();
    void momentPaneMatchesTheSlice();
    void croppedAndForeignMaps();
    void editsOnOnePaneShowOnTheOthers();
    void remotePlaneDrawingIsTheLocalOne();
};

#pragma once
// TestFieldOverlay.h
// QTest class for the arithmetic of several fields in one view (FieldOverlay.h):
// the log rule, the quantile that starts a 3-D ramp, the Layered and Additive
// composites, and the split / mirrored layouts.

#include <QObject>

class TestFieldOverlay : public QObject
{
    Q_OBJECT

private slots:
    void aPositiveFieldOverThreeDecadesPrefersLog();
    void aSignedOrNarrowFieldStaysLinear();
    void quantileIgnoresBlanksAndClamps();

    void layeredOpaqueTopHidesTheBottom();
    void layeredOpacityMixesTheLayers();
    void layeredBlankTopLeavesTheBottom();
    void layeredNothingIsTransparent();
    void additiveSumsAndClips();
    void additiveBlankLayerAddsNothing();
    void additiveMatchesTheImageViewerBlend();

    void splitTakesLeftFromAAndRightFromB();
    void splitSeamIsClampedToTheSlice();
    void mirroredReflectsAAndKeepsB();
    void mirroredAxisAtTheEdgeDoublesTheWidth();
    void mirroredRightHalfKeepsItsPlace();
    void composeSplitPaintsEachHalfWithItsOwnColours();
    void seamColumnFollowsStrideAndOrigin();

    void opacityNodesScale();
};

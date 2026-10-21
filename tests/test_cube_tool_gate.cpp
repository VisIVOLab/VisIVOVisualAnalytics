// test_cube_tool_gate.cpp
// Unit tests for CubeToolGate, the decision table behind "which cube tools may
// act right now". The rules used to live inline in vtkWindowCube, where the only
// way to check them was to open the app, put a pane in front and look at thirty
// buttons — which is how the Kinematic Lasso came to sit enabled with no 3-D
// view on screen, arming an observer on an interactor nobody was looking at.
//
// A tool binds to a VIEW, not to the focused pane: the user must be able to
// select the spectrum pane (to reach its controls) without the slice tools
// greying out under them.

#include "TestCubeToolGate.h"
#include "CubeToolGate.h"

#include <QTest>

using namespace CubeToolGate;

namespace {
Inputs layout(bool slice, bool view3d)
{
    Inputs in;
    in.slice2dVisible = slice;
    in.view3dVisible = view3d;
    return in;
}
}   // namespace

void TestCubeToolGate::slice2dTools_followTheSliceViewBeingOnScreen()
{
    QVERIFY(evaluate(layout(/*slice=*/true, /*view3d=*/false)).slice2dTools);
    QVERIFY(evaluate(layout(true, true)).slice2dTools);
    QVERIFY(!evaluate(layout(false, true)).slice2dTools);
}

void TestCubeToolGate::view3dTools_followThe3dViewBeingOnScreen()
{
    QVERIFY(evaluate(layout(false, true)).view3dTools);
    QVERIFY(evaluate(layout(true, true)).view3dTools);
    QVERIFY(!evaluate(layout(true, false)).view3dTools);
}

void TestCubeToolGate::families_areIndependentNotExclusive()
{
    // With both views on screen — the common 2-pane layout — both families are
    // available at once. Each acts on its own view, and the armed pane's amber
    // border says which one is listening.
    const Decision d = evaluate(layout(true, true));
    QVERIFY(d.slice2dTools);
    QVERIFY(d.view3dTools);
}

void TestCubeToolGate::slice2dTools_alsoOnAMomentPane()
{
    // A moment map on screen and no slice: the slice tools stay available —
    // arming one makes that pane the slice with the map as its background.
    Inputs in = layout(false, true);
    in.slicePaneAvailable = true;
    QVERIFY(evaluate(in).slice2dTools);
}

void TestCubeToolGate::noViews_disablesBothFamilies()
{
    // e.g. two panes both showing derived products.
    const Decision d = evaluate(layout(false, false));
    QVERIFY(!d.slice2dTools);
    QVERIFY(!d.view3dTools);
}

// ── State gates ──────────────────────────────────────────────────────────────
void TestCubeToolGate::pinSpectrum_followsTheProbedSpectrumOnly()
{
    Inputs in = layout(true, true);
    QVERIFY(!evaluate(in).pinSpectrum);
    in.haveProbeSpectrum = true;
    QVERIFY(evaluate(in).pinSpectrum);
}

void TestCubeToolGate::exportRegion_followsRegionExistence()
{
    Inputs in = layout(true, false);
    QVERIFY(!evaluate(in).exportRegion);
    in.haveRegion = true;
    QVERIFY(evaluate(in).exportRegion);
}

void TestCubeToolGate::sendSlice_followsAnOpenImageViewer()
{
    Inputs in = layout(true, false);
    QVERIFY(!evaluate(in).sendSliceToImageViewer);
    in.imageViewerOpen = true;
    QVERIFY(evaluate(in).sendSliceToImageViewer);
}

void TestCubeToolGate::linkViews_followsLinkTargets()
{
    Inputs in = layout(true, false);
    QVERIFY(!evaluate(in).linkViews);
    in.haveLinkTargets = true;
    QVERIFY(evaluate(in).linkViews);
}

void TestCubeToolGate::stateGates_ignoreTheLayout()
{
    // A probed spectrum stays pinnable, a region stays exportable and contours
    // stay sendable whatever is on screen — including a layout with neither the
    // slice nor the 3-D view in it (the region geometry, the probed curve and the
    // other window all outlive the pane the tool was used in).
    Inputs in = layout(false, false);
    in.haveProbeSpectrum = true;
    in.haveRegion = true;
    in.imageViewerOpen = true;
    in.haveLinkTargets = true;
    const Decision d = evaluate(in);
    QVERIFY(d.pinSpectrum);
    QVERIFY(d.exportRegion);
    QVERIFY(d.sendSliceToImageViewer);
    QVERIFY(d.linkViews);
    QVERIFY(!d.slice2dTools);
    QVERIFY(!d.view3dTools);
}

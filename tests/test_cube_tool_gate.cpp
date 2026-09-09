// test_cube_tool_gate.cpp
// Unit tests for CubeToolGate, the decision table behind "which cube tools may
// act on what is in front of the user". The rules used to live inline in
// vtkWindowCube, where the only way to check them was to open the app, put a
// pane in front and look at thirty buttons — which is how the Kinematic Lasso
// came to sit enabled on the 2-D slice, arming an observer on a 3-D interactor
// nobody was looking at.

#include "TestCubeToolGate.h"
#include "CubeToolGate.h"

#include <QTest>

using namespace CubeToolGate;

namespace {
Inputs on(Pane p)
{
    Inputs in;
    in.pane = p;
    return in;
}
}   // namespace

// ── The six (now seven) 2-D slice tools ──────────────────────────────────────
void TestCubeToolGate::slice2dTools_availableOnTheSlicePane()
{
    const Decision d = evaluate(on(Pane::Slice2D));
    QVERIFY(d.slice2dTools);
    QCOMPARE(d.sliceReason, SliceReason::None);
}

void TestCubeToolGate::slice2dTools_gatedOn3dWithReason()
{
    const Decision d = evaluate(on(Pane::View3D));
    QVERIFY(!d.slice2dTools);
    QCOMPARE(d.sliceReason, SliceReason::View3D);
}

void TestCubeToolGate::slice2dTools_gatedOnMomentWithItsOwnReason()
{
    // A moment map keeps its own wording — it is not "pick the 3-D view", it is
    // "this map is collapsed in velocity".
    const Decision d = evaluate(on(Pane::Moment));
    QVERIFY(!d.slice2dTools);
    QCOMPARE(d.sliceReason, SliceReason::Moment);
}

void TestCubeToolGate::slice2dTools_gatedOnSpectrumAndPvPanes()
{
    for (const Pane p : { Pane::Spectrum, Pane::Pv }) {
        const Decision d = evaluate(on(p));
        QVERIFY(!d.slice2dTools);
        QCOMPARE(d.sliceReason, SliceReason::DerivedProduct);
    }
}

void TestCubeToolGate::slice2dTools_availableOnOtherProductAndNoPane()
{
    // Anything else renderable (channel maps, a masked cube …) and the
    // no-pane-selected state keep the baseline behaviour: not gated.
    for (const Pane p : { Pane::OtherProduct, Pane::None }) {
        const Decision d = evaluate(on(p));
        QVERIFY(d.slice2dTools);
        QCOMPARE(d.sliceReason, SliceReason::None);
    }
}

void TestCubeToolGate::extractSpectrum_staysArmedOnItsOwnLivePreview()
{
    // The live preview pane IS a spectrum product, so the slice tools gate on
    // it — but un-arming Extract Spectrum there would stop the live spectrum and
    // delete the pane the user is watching.
    Inputs in = on(Pane::Spectrum);
    in.liveSpectrumPane = true;
    const Decision d = evaluate(in);
    QVERIFY(!d.slice2dTools);
    QVERIFY(d.keepExtractSpectrumArmed);

    // A finished (pinned / generated) spectrum is not that.
    QVERIFY(!evaluate(on(Pane::Spectrum)).keepExtractSpectrumArmed);
}

// ── The 3-D-only tools (lasso + plane picker) ───────────────────────────────
void TestCubeToolGate::view3dTools_onlyOnThe3dPane()
{
    QVERIFY(evaluate(on(Pane::View3D)).view3dTools);
    for (const Pane p : { Pane::Slice2D, Pane::Moment, Pane::Spectrum, Pane::Pv,
                          Pane::OtherProduct, Pane::None })
        QVERIFY(!evaluate(on(p)).view3dTools);
}

void TestCubeToolGate::view3dTools_mirrorTheSliceGate()
{
    // The two families are mutually exclusive by construction: whichever pane is
    // in front, at most one of them may act. This is the property the screenshots
    // disagreed with (lasso enabled next to greyed-out region tools).
    for (const Pane p : { Pane::View3D, Pane::Slice2D, Pane::Moment, Pane::Spectrum,
                          Pane::Pv, Pane::OtherProduct, Pane::None }) {
        const Decision d = evaluate(on(p));
        QVERIFY(!(d.slice2dTools && d.view3dTools));
    }
}

// ── State gates ──────────────────────────────────────────────────────────────
void TestCubeToolGate::pinSpectrum_followsTheProbedSpectrumNotThePane()
{
    // Pin Spectrum promotes the last probed spectrum, and both the 2-D probe and
    // the 3-D plane pick feed that — so the pane must not decide it.
    for (const Pane p : { Pane::Slice2D, Pane::View3D, Pane::Spectrum, Pane::Moment }) {
        Inputs in = on(p);
        QVERIFY(!evaluate(in).pinSpectrum);
        in.haveProbeSpectrum = true;
        QVERIFY(evaluate(in).pinSpectrum);
    }
}

void TestCubeToolGate::exportRegion_followsRegionExistence()
{
    Inputs in = on(Pane::Slice2D);
    QVERIFY(!evaluate(in).exportRegion);
    in.haveRegion = true;
    QVERIFY(evaluate(in).exportRegion);
    // Region geometry outlives the pane it was drawn in.
    in.pane = Pane::View3D;
    QVERIFY(evaluate(in).exportRegion);
}

void TestCubeToolGate::sendSlice_followsAnOpenImageViewer()
{
    Inputs in = on(Pane::Slice2D);
    QVERIFY(!evaluate(in).sendSliceToImageViewer);
    in.imageViewerOpen = true;
    QVERIFY(evaluate(in).sendSliceToImageViewer);
}

void TestCubeToolGate::linkViews_followsLinkTargets()
{
    Inputs in = on(Pane::Slice2D);
    QVERIFY(!evaluate(in).linkViews);
    in.haveLinkTargets = true;
    QVERIFY(evaluate(in).linkViews);
}

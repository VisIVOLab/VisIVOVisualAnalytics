#ifndef CUBE_TOOL_GATE_H
#define CUBE_TOOL_GATE_H
// CubeToolGate.h
// The cube viewer's tool-availability decision table, as pure inline logic.
//
// Which of the thirty-odd cube tools may act right now is not a property of the
// tool but of what is in front of the user: the six slice tools draw on the 2-D
// slice, the lasso and the 3-D picker listen to the 3-D interactor, and a
// handful more need a piece of state to exist first (a probed spectrum to pin, a
// drawn region to export, an image viewer to send contours to, a second view to
// link against). That reasoning used to live inline in vtkWindowCube where it
// could only be checked by opening the app and looking; it is a table, so it is
// written as one here — no Qt widgets, no VTK, unit-tested in
// tests/test_cube_tool_gate.cpp.
//
// The caller maps a decision onto QActions (enabled state + a tooltip that says
// WHY, never a silent no-op) and keeps every surface — Tools menu, left dock,
// Inspector ▸ Analysis — in step, because all three are built from the same
// actions.

namespace CubeToolGate {

/// What the active pane is showing. Slice2D is the fixed 2-D slice view; the
/// Moment / Spectrum / Pv entries are derived products mounted in a pane, and
/// OtherProduct is any further renderable product (channel maps, masked cube …).
enum class Pane { None, View3D, Slice2D, Moment, Spectrum, Pv, OtherProduct };

/// Why the 2-D slice tools are unavailable (None = they are available).
enum class SliceReason { None, Moment, View3D, DerivedProduct };

struct Inputs
{
    Pane pane{ Pane::None };
    /// The active pane is the live probe preview the Extract Spectrum tool is
    /// itself driving — disarming the tool there would delete what is on screen.
    bool liveSpectrumPane{ false };
    /// A spectrum has been probed (2-D click) or picked (3-D plane click), so
    /// Pin Spectrum has something to promote.
    bool haveProbeSpectrum{ false };
    /// A region has been drawn or imported and is still current.
    bool haveRegion{ false };
    /// At least one image viewer window is open to receive slice contours.
    bool imageViewerOpen{ false };
    /// More than one pane in this window, or more than one cube window open.
    bool haveLinkTargets{ false };
};

struct Decision
{
    SliceReason sliceReason{ SliceReason::None };
    /// Extract Spectrum / PV / Box / Circle / Polygon / Annulus / Import Region.
    bool slice2dTools{ false };
    /// Leave Extract Spectrum alone even though the slice tools are gated.
    bool keepExtractSpectrumArmed{ false };
    /// Kinematic Lasso / Pick Spectrum on Plane Click.
    bool view3dTools{ false };
    bool pinSpectrum{ false };
    bool exportRegion{ false };
    bool sendSliceToImageViewer{ false };
    bool linkViews{ false };
};

inline Decision evaluate(const Inputs &in)
{
    Decision d;
    // A moment map is collapsed in velocity, and a spectrum / PV diagram is
    // itself a derived 1-D or 2-D result: none of the three carries the (x, y, v)
    // grid the slice tools measure. The 3-D view has the grid but not the 2-D
    // interactor they install their observers on.
    d.sliceReason = (in.pane == Pane::Moment)   ? SliceReason::Moment
                  : (in.pane == Pane::View3D)   ? SliceReason::View3D
                  : (in.pane == Pane::Spectrum
                     || in.pane == Pane::Pv)    ? SliceReason::DerivedProduct
                                                : SliceReason::None;
    d.slice2dTools = (d.sliceReason == SliceReason::None);
    d.keepExtractSpectrumArmed = in.liveSpectrumPane;
    d.view3dTools = (in.pane == Pane::View3D);
    // These four gate on state, not on the pane: a spectrum can be probed on the
    // slice or picked on the 3-D cutting plane and stays pinnable from either;
    // region geometry outlives the pane it was drawn in; contours go to another
    // window entirely; linking is about how many views exist, not which is up.
    d.pinSpectrum = in.haveProbeSpectrum;
    d.exportRegion = in.haveRegion;
    d.sendSliceToImageViewer = in.imageViewerOpen;
    d.linkViews = in.haveLinkTargets;
    return d;
}

}   // namespace CubeToolGate

#endif   // CUBE_TOOL_GATE_H

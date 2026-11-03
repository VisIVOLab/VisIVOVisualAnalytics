#ifndef CUBE_TOOL_GATE_H
#define CUBE_TOOL_GATE_H
// CubeToolGate.h
// The cube viewer's tool-availability decision table, as pure inline logic.
//
// Which of the thirty-odd cube tools may act right now is not a property of the
// tool but of what is ON SCREEN: the seven slice tools draw on the 2-D slice
// view, the lasso and the 3-D picker listen to the 3-D view's interactor, and a
// handful more need a piece of state to exist first (a probed spectrum to pin, a
// drawn region to export, an image viewer to send contours to, a second view to
// link against). That reasoning used to live inline in vtkWindowCube where it
// could only be checked by opening the app and looking; it is a table, so it is
// written as one here — no Qt widgets, no VTK, unit-tested in
// tests/test_cube_tool_gate.cpp.
//
// A tool binds to a VIEW, not to the focused pane. Gating on the active pane
// instead (the first cut of this table) forced the user to keep switching:
// select the spectrum pane to reach its controls and the slice tools greyed out;
// select the slice to extract and the spectrum controls disappeared. So the
// question each family asks is "is my view on screen?", the armed pane wears the
// amber border that says where the tool is listening, and that pane keeps taking
// mouse input even while another pane holds the focus (vtkWindowCube's
// paneIsArmedTarget).
//
// The caller maps a decision onto QActions (enabled state + a tooltip that says
// WHY, never a silent no-op) and keeps both surfaces — the Tools menu and
// Inspector ▸ Analysis — in step, because they are built from the same actions.

#include <QString>
#include <QStringView>

namespace CubeToolGate {

/// True when a CTYPE names a SPATIAL axis — a celestial one or a plain
/// coordinate (a simulation box's X/Y/Z, LINEAR) — so a cube whose third axis
/// is one has no spectrum along it. An empty or unknown type is not: a FITS
/// cube with an incomplete header keeps every tool, as it always has.
inline bool isSpatialAxisType(QStringView ctype)
{
    const QString t = ctype.trimmed().toString().toUpper();
    if (t.isEmpty())
        return false;
    if (t == u"X" || t == u"Y" || t == u"Z" || t == u"LINEAR" || t.startsWith(u"OFFSET"))
        return true;
    for (const char16_t *prefix : { u"RA", u"DEC", u"GLON", u"GLAT", u"ELON", u"ELAT" })
        if (t == QStringView(prefix) || t.startsWith(QString::fromUtf16(prefix) + u'-'))
            return true;
    return false;
}

struct Inputs
{
    /// The fixed 2-D slice view is mounted in a visible pane.
    bool slice2dVisible{ false };
    /// The 3-D view is mounted in a visible pane.
    bool view3dVisible{ false };
    /// A visible pane shows a full-field map (a moment) that the 2-D Slice can
    /// take as its background: arming a slice tool turns that pane into the
    /// slice with the map underneath, so the tool draws on the map.
    bool slicePaneAvailable{ false };
    /// A spectrum has been probed (2-D click) or picked (3-D plane click), so
    /// Pin Spectrum has something to promote.
    bool haveProbeSpectrum{ false };
    /// A region has been drawn or imported and is still current.
    bool haveRegion{ false };
    /// At least one image viewer window is open to receive slice contours.
    bool imageViewerOpen{ false };
    /// More than one pane in this window, or more than one cube window open.
    bool haveLinkTargets{ false };
    /// The third axis is a spectrum (frequency, velocity, wavelength, channel,
    /// or unknown); false for a spatial one (a VBT volume's Z).
    bool spectralAxis{ true };
    /// The two first axes carry a celestial WCS.
    bool celestialWcs{ true };
};

struct Decision
{
    /// Extract Spectrum / PV / Box / Circle / Polygon / Annulus / Import Region.
    bool slice2dTools{ false };
    /// Kinematic Lasso / Pick Spectrum on Plane Click.
    bool view3dTools{ false };
    bool pinSpectrum{ false };
    bool exportRegion{ false };
    bool sendSliceToImageViewer{ false };
    bool linkViews{ false };
    /// Line width, baseline, stacking, polarimetry, source finding, kinematic
    /// model: they read the third axis as a spectrum. Hidden, not greyed, on a
    /// cube that has none — no state would ever make them apply.
    bool spectralTools{ true };
    /// Catalogue overlays place RA/Dec on the image.
    bool celestialTools{ true };
};

inline Decision evaluate(const Inputs &in)
{
    Decision d;
    // Each family needs its own view on screen — that is where it draws its
    // overlay and where its interactor observer lives. Nothing else about the
    // layout matters, the focused pane included.
    d.slice2dTools = in.slice2dVisible || in.slicePaneAvailable;
    d.view3dTools = in.view3dVisible;
    // These four gate on state, not on the layout: a spectrum can be probed on
    // the slice or picked on the 3-D cutting plane and stays pinnable from
    // either; region geometry outlives the pane it was drawn in; contours go to
    // another window entirely; linking is about how many views exist.
    d.pinSpectrum = in.haveProbeSpectrum;
    d.exportRegion = in.haveRegion;
    d.sendSliceToImageViewer = in.imageViewerOpen;
    d.linkViews = in.haveLinkTargets;
    // What the data IS, not what is on screen: fixed for a dataset.
    d.spectralTools = in.spectralAxis;
    d.celestialTools = in.celestialWcs;
    return d;
}

}   // namespace CubeToolGate

#endif   // CUBE_TOOL_GATE_H

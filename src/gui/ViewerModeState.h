#ifndef ViewerModeState_h
#define ViewerModeState_h

// ViewerModeState.h
// Which of the image viewer's mutually exclusive modes is the one on screen.
//
// Pan, Probe, Region, Ruler and Annotate cannot be on at once: arming one
// disarms the others. But the "off" is done by four different code paths — an
// action group, a setter that unchecks its neighbours, a placement flag cleared
// at the end of a click — and any of them can be reached from the menu, the
// command palette, or another mode's cleanup.
//
// Rather than keep a fifth copy of the state and hope it agrees with the other
// four, the strip DERIVES what to show from them. The derivation is this
// function, in a header with no widgets in it, so the precedence — the answer
// to "what if two are somehow on at once" — can be tested directly.

namespace visivo {

enum class ViewerModeKind
{
    Pan = 0,
    Probe,
    Region,
    Ruler,
    Annotate,
};

/// The mode to display, given what is armed.
///
/// The order is not a preference: it is the order `toggleProbeFreeze()`
/// dispatches a click in — annotation placement first, then a measurement, then
/// a region, then the probe. If two flags ever say "on", the strip has to name
/// the one that will actually consume the next click; naming the other is worse
/// than naming nothing, because the user acts on it. Pan is what is left when
/// nothing is armed.
///
/// Keep this in step with that handler: it is the definition, not a copy of it.
inline ViewerModeKind viewerModeFor(bool regionArmed, bool measurementArmed,
                                    bool annotationPlacing, bool probeActive)
{
    if (annotationPlacing)
        return ViewerModeKind::Annotate;
    if (measurementArmed)
        return ViewerModeKind::Ruler;
    if (regionArmed)
        return ViewerModeKind::Region;
    if (probeActive)
        return ViewerModeKind::Probe;
    return ViewerModeKind::Pan;
}

}   // namespace visivo

#endif   // ViewerModeState_h

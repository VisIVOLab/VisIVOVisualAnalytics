#ifndef SkyLinkEndpoint_h
#define SkyLinkEndpoint_h

// SkyLinkEndpoint.h
// What an image window and a cube window both offer so that SkyLinkHub can link
// them in sky coordinates (issue #522).
//
// The hub owns no view of its own: it asks a window for what it shows (its view
// in the window's own pixels) and tells another window to show the same sky.
// Everything about pixels, projections and regions that does not need a window
// is in SkyWcs.h and RegionSkyConvert.h; this is the thin seam between them and
// the two viewers, so the viewers do not need to know about each other.

#include "RegionModel.h"
#include "SkyWcs.h"

#include <QString>

#include <array>
#include <vector>

class QWidget;

class SkyLinkEndpoint
{
public:
    virtual ~SkyLinkEndpoint() = default;

    /// The window, for the list of windows to link to and to bring forward.
    virtual QWidget *skyLinkWidget() = 0;
    /// A cube window or an image window: a link joins one of each.
    virtual bool skyLinkIsCube() const = 0;
    /// How the window is named in the list ("SDP9_cont.I.pbcor.fits").
    virtual QString skyLinkLabel() const = 0;
    /// The celestial WCS of the plane the window shows, read from its dataset's
    /// header on first use; null when it has none the link can use.
    virtual const visivo::skylink::SkyWcs *skyWcs() = 0;

    /// The view on screen, in the window's pixels. False when the window shows
    /// no 2-D view of its dataset right now.
    virtual bool skyViewRead(visivo::skylink::View &view) = 0;
    /// Show @p view (a pan and a zoom) without telling the peers it moved.
    virtual void skyViewApply(const visivo::skylink::View &view) = 0;
    /// A peer's cursor is at pixel (px, py) of this window's dataset.
    virtual void skyCursorShow(double px, double py) = 0;
    virtual void skyCursorHide() = 0;

    /// The regions of the window's list, in its pixels: the selected one (with
    /// the rings it is grouped with) or all of them.
    virtual std::vector<visivo::regions::Region> skyRegions(bool selectedOnly) const = 0;
    /// Add regions already in this window's pixels; @p label names where they
    /// came from. Returns how many joined the list.
    virtual int skyAddRegions(const std::vector<visivo::regions::Region> &regions,
                              const QString &label) = 0;

    /// A path drawn in this window that a cube can cut a PV along: the vertices
    /// of the selected line region, empty when there is none.
    virtual std::vector<visivo::regions::Vec2> skyPvPath() const { return {}; }
    /// Cut a PV along @p path (this window's pixels) and show it. False when it
    /// cannot start; the window says why in its status bar.
    virtual bool skyRunPv(const std::vector<std::array<double, 2>> &path, const QString &label)
    {
        Q_UNUSED(path);
        Q_UNUSED(label);
        return false;
    }

    /// The set of windows this one is linked to changed (actions to refresh).
    virtual void skyLinkStateChanged() = 0;
    /// A word for the status bar.
    virtual void skyMessage(const QString &text) = 0;
};

#endif   // SkyLinkEndpoint_h

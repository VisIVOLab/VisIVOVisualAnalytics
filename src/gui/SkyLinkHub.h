#ifndef SkyLinkHub_h
#define SkyLinkHub_h

// SkyLinkHub.h
// Links between image windows and cube windows in sky coordinates (issue #522),
// as CARTA's "XY matching" does: pan and zoom follow by sky centre and angular
// scale (not pixels), the cursor of one window is a crosshair in the other,
// regions drawn on a radio map land on the cube's slice, and a path drawn on
// the image becomes the PV cut of the cube.
//
// The hub holds the pairs and does the matching with SkyWcs.h; the windows
// only say what they show (SkyLinkEndpoint.h). It is separate from the cube
// viewers' own "Link Views" (camera, channel and colour across cubes): the two
// can be on together, and a cube that follows an image passes the view on to the
// cubes it is Link-Viewed with.

#include "SkyLinkEndpoint.h"

#include <QList>
#include <QPointer>
#include <QSet>

class QWidget;

class SkyLinkHub
{
public:
    static SkyLinkHub &instance();

    /// Ask which open window of the other kind to link @p self to, and link
    /// them (the window the user acted in leads: the other one is put on its
    /// sky straight away).
    void chooseAndLink(SkyLinkEndpoint *self);
    bool link(SkyLinkEndpoint *a, SkyLinkEndpoint *b);
    /// Break every link of @p self.
    void unlink(SkyLinkEndpoint *self);
    /// A window is closing: drop its links without a word.
    void forget(SkyLinkEndpoint *self);

    bool isLinked(const SkyLinkEndpoint *self) const;
    QList<SkyLinkEndpoint *> peers(const SkyLinkEndpoint *self) const;

    /// @p from's view moved: bring its peers to the same sky. Coalesced to one
    /// run per turn of the event loop, whatever frames asked for it.
    void notifyViewMoved(SkyLinkEndpoint *from);
    /// @p from's cursor is at pixel (px, py) of its dataset.
    void cursorMoved(SkyLinkEndpoint *from, double px, double py);

    /// Copy @p from's regions (the selected one or all) to a window of the
    /// other kind: its linked peer, or one chosen from the open windows.
    void sendRegions(SkyLinkEndpoint *from, bool selectedOnly);
    /// Cut a PV in a cube along the line region selected in image @p from.
    void extractPv(SkyLinkEndpoint *from);

    /// Open windows of the other kind than @p self.
    QList<SkyLinkEndpoint *> candidates(const SkyLinkEndpoint *self) const;

private:
    SkyLinkHub() = default;

    struct Pair
    {
        QPointer<QWidget> a;
        QPointer<QWidget> b;
        SkyLinkEndpoint *ea{ nullptr };
        SkyLinkEndpoint *eb{ nullptr };
    };

    void prune();
    void syncFrom(SkyLinkEndpoint *from);
    /// The window to act on: @p from's single peer, else the one the user picks
    /// among the open windows of the other kind. Null when there is none (the
    /// reason went to @p from's status bar) or the user cancelled.
    SkyLinkEndpoint *chooseTarget(SkyLinkEndpoint *from, const QString &title,
                                  const QString &prompt);

    QList<Pair> m_pairs;
    QSet<QWidget *> m_pending;   ///< windows with a coalesced view update waiting
    qint64 m_lastCursorMs{ 0 };
};

#endif   // SkyLinkHub_h

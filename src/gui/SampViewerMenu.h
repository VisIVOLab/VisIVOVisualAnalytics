// SampViewerMenu.h – the SAMP menu of a viewer window (issue #130)
//
// The image, cube and 3-D catalogue windows each have a SAMP menu acting on
// THEIR data, with a "Send … To" choice of recipient: every tool that takes
// it, or one of them by name. The hub connection itself is the Data Hub's
// (MainWindow): these helpers find it and build the menu items from it. The
// menu is rebuilt each time it opens, so it always shows the hub's current
// tools and the connection's state.

#ifndef SAMPVIEWERMENU_H
#define SAMPVIEWERMENU_H

#include "app/SampSkyPosition.h"

#include <QString>

#include <functional>
#include <optional>

class MainWindow;
class QMainWindow;
class QMenu;
class QObject;
class QWidget;

namespace visivo {

/// The window that holds the SAMP connection, or null.
MainWindow *sampHost(const QWidget *viewer);

/// "@p title ▸ All Tools / <tool> …" in @p menu, the tools being those that
/// take @p mtype. @p send gets the chosen tool's hub id (empty: all of them).
/// With @p unavailable set, or no hub, a disabled entry says why instead.
void addSampSendTo(QMenu *menu, MainWindow *host, const QString &title, const QString &mtype,
                   const QString &unavailable, std::function<void(const QString &)> send);

/// The connection's state at the top of a viewer's SAMP menu, with Connect
/// To Hub when not connected. False when there is no hub to send to.
bool addSampConnectionLine(QMenu *menu, MainWindow *host);

/// A "SAMP" menu at the end of @p window's menu bar, filled by @p fill each
/// time it opens (after the connection line).
QMenu *installSampMenu(QMainWindow *window, std::function<void(QMenu *, MainWindow *)> fill);

/// Where a pixel is on the sky, for coord.pointAt.sky.
struct PixelSky
{
    std::optional<samp::SkyPosition> position;
    QString problem;      ///< why there is none
    bool local{ false };  ///< computed here: the backend has no pixel_to_sky route
};

/// The ICRS position of 0-based pixel (@p x, @p y, channel @p z) of an open
/// dataset, from the file's full WCS on the backend (POST
/// /v1/datasets/pixel_to_sky), off the GUI thread. A backend older than that
/// route gets @p fallback, the client's own conversion. @p done runs on the
/// GUI thread, and not at all once @p context is gone.
struct PixelSkyRequest
{
    QString backendUrl;
    QString token;
    QString sessionId;
    QString datasetId;
    double x{ 0.0 };
    double y{ 0.0 };
    double z{ 0.0 };
};
void resolvePixelSky(QObject *context, const PixelSkyRequest &request,
                     std::function<std::optional<samp::SkyPosition>()> fallback,
                     std::function<void(const PixelSky &)> done);

}  // namespace visivo

#endif  // SAMPVIEWERMENU_H

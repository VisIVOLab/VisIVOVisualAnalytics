#pragma once

// BackendLocality.h
// Whether a viewer's backend is the local one this run started, which decides
// where its 3-D view is drawn: on the desktop for the local backend, and on
// the backend (server-side rendering) only for a remote one, never mixed.

#include <QByteArray>
#include <QString>

class QNetworkReply;

namespace visivo {

/// True for the local backend, false for a remote one (--backend, a node of
/// the backends registry reached through a tunnel, another host).
bool backendIsLocal(const QString &backendUrl);

/// How the user knows this backend: "Local", this run's remote backend by the
/// name the status bar shows, a node of the backends registry by its name,
/// else the URL's host:port.
QString backendDisplayName(const QString &backendUrl);

/// For a reply of a backend that has no route for `feature` (an older one:
/// visivo::isMissingRouteReply), the line that says so and names the backend
/// ("The backend on PLEIADI does not have ray-traced views (it is older than
/// this client); update it."). Empty for any other reply, a route's own 404
/// included.
/// `body` is the reply's body, read by the caller (a reply reads once).
QString missingRouteMessage(const QString &backendUrl, const QNetworkReply *reply,
                            const QByteArray &body, const QString &feature);

} // namespace visivo

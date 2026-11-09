#pragma once

// BackendLocality.h
// Whether a viewer's backend is the local one this run started, which decides
// where its 3-D view is drawn: on the desktop for the local backend, and on
// the backend (server-side rendering) only for a remote one, never mixed.

#include <QString>

namespace visivo {

/// True for the local backend, false for a remote one (--backend, a node of
/// the backends registry reached through a tunnel, another host).
bool backendIsLocal(const QString &backendUrl);

/// How the user knows this backend: "Local", this run's remote backend by the
/// name the status bar shows, a node of the backends registry by its name,
/// else the URL's host:port.
QString backendDisplayName(const QString &backendUrl);

} // namespace visivo

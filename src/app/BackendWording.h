#pragma once

// BackendWording.h
// The words the interface uses for where a dataset lives. A dataset is opened
// by a backend process either way, so "remote" is only true when that backend
// is not the one this run started; a local run that says "remote · full-res"
// sends the user looking for a server that does not exist (issue #502).
// Header-only, so the wording is tested without a window.

#include <QString>

namespace visivo {

/// "local" or "remote", for a tag such as "local · full-res".
inline QString backendKindWord(bool local)
{
    return local ? QStringLiteral("local") : QStringLiteral("remote");
}

/// The pane's secondary tag: what is shown and where it was read from.
inline QString fullResolutionTag(bool local)
{
    return QStringLiteral("%1 · full-res").arg(backendKindWord(local));
}

/// What a moment map was computed from, for its summary and provenance.
inline QString momentScopeLabel(bool local)
{
    return local ? QStringLiteral("Full dataset (local backend)")
                 : QStringLiteral("Remote full dataset");
}

/// How a Recent Datasets entry says where it was opened. "remote" is the
/// stored source of every file a backend opened by path; it names the local
/// backend as long as the entry recorded no other backend (@p backend empty).
/// Any other source (a SKAVA discovery, a SODA cutout, a node product) is
/// shown as stored.
inline QString recentSourceLabel(const QString &source, const QString &backend)
{
    if (source.isEmpty() || source == QLatin1String("remote"))
        return backend.isEmpty() ? QStringLiteral("local") : QStringLiteral("remote");
    return source;
}

}   // namespace visivo

#pragma once

// WorkspaceExportsView.h
// What the Data Hub's Workspace Exports list shows of a listing (issue #312).
// The list is refreshed on every backend health tick (every 4 s). Rebuilding
// one styled row per export each time stalled the app with thousands of
// exports, so the rows are rebuilt only when the listing changed, and only
// the most recent ones get a row. Widget-free, so it is unit-tested.

#include "app/BackendClient.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace visivo {

/// Rows the list builds at most: the most recent exports (the backend sends
/// them newest first). Older ones are counted on a line of their own.
inline constexpr int kWorkspaceExportRows = 100;

/// A fingerprint of everything the list shows of a listing: equal
/// fingerprints, equal rows. An export's name, size and time are its row.
inline QByteArray workspaceExportsSignature(const BackendWorkspaceExportsList &listing)
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(listing.valid ? QByteArrayView("ok") : QByteArrayView("failed"));
    h.addData(QByteArrayView("\x1f", 1));
    h.addData(listing.error.toUtf8());
    for (const auto &e : listing.entries) {
        h.addData(QByteArrayView("\x1e", 1));
        h.addData(e.filename.toUtf8());
        h.addData(QByteArrayView("\x1f", 1));
        h.addData(QByteArray::number(e.size));
        h.addData(QByteArrayView("\x1f", 1));
        h.addData(e.modifiedTime.toUtf8());
        h.addData(QByteArrayView("\x1f", 1));
        h.addData(e.absolutePath.toUtf8());
    }
    return h.result();
}

/// How many of `total` exports get a row.
inline int workspaceExportRowsShown(qsizetype total)
{
    return int(qMin<qsizetype>(total, kWorkspaceExportRows));
}

/// The line under the rows when some exports have none; empty when all do.
inline QString workspaceExportsOverflowText(qsizetype total)
{
    const qsizetype hidden = total - workspaceExportRowsShown(total);
    if (hidden <= 0)
        return {};
    return QStringLiteral("%1 older export%2 not listed. Use “Clean up older than…” to remove old exports.")
            .arg(hidden)
            .arg(hidden == 1 ? QString() : QStringLiteral("s"));
}

}  // namespace visivo

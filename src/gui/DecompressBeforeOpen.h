#ifndef DECOMPRESS_BEFORE_OPEN_H
#define DECOMPRESS_BEFORE_OPEN_H
// DecompressBeforeOpen.h
// The progress sheet in front of opening a compressed FITS (issue #57).
//
// The backend opens a .fits.gz or .fz from a decompressed copy in its
// workspace cache. Making that copy of a 2 GB cube takes seconds on a warm
// disk and minutes on a cold one or a remote node, and BackendClient::
// openDataset() would wait for it with nothing on screen. So the windows that
// open a dataset by path decompress first, here: a task followed behind the
// standard task sheet ("Decompressing cube.fits.gz… 42 %", Cancel), after
// which openDataset() finds the copy in the cache and opens at once.
//
// openDataset() without this (a caller with no window) still decompresses on
// its own, without a sheet.

#include <QElapsedTimer>
#include <QString>

#include <functional>

class BackendClient;
class QWidget;

namespace visivo {

struct DecompressBeforeOpenResult
{
    /// Go on and open the file: decompressed, cached already, not compressed,
    /// or a backend without the decompression route (its open says the rest).
    bool proceed{ true };
    /// The user pressed Cancel; the backend was asked to stop. Say it quietly.
    bool cancelled{ false };
    /// Another file is being decompressed (one at a time); error says so.
    /// Its progress window has been brought to the front. Say this without a
    /// modal box: a box's event loop would hold the other file's open until
    /// it is dismissed.
    bool busy{ false };
    /// Why the file cannot be opened, when neither of the above.
    QString error;
};

/// Decompress @p path on @p client's backend behind a progress window over
/// @p parent, waiting in a local event loop until it is done. Returns at once
/// for a name that is not a compressed FITS and for a copy already cached.
///
/// @p requestedAt is when the user asked for the open: the window's clock
/// counts from it (invalid: from the start of the task). @p onTaskStarted is
/// called once the backend has the task running, so a panel that lists the
/// backend's work can look again at once instead of at its next poll.
DecompressBeforeOpenResult decompressBeforeOpen(QWidget *parent, const BackendClient &client,
                                                const QString &path,
                                                const QElapsedTimer &requestedAt = QElapsedTimer(),
                                                const std::function<void()> &onTaskStarted = {});

}   // namespace visivo

#endif   // DECOMPRESS_BEFORE_OPEN_H

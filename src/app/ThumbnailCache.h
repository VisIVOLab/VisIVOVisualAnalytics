#ifndef ThumbnailCache_h
#define ThumbnailCache_h

#include <QPixmap>
#include <QString>

class QWidget;

// ---------------------------------------------------------------------------
// ThumbnailCache — tiny on-disk cache of dataset preview images for the Data
// Hub "Recent datasets" panel.
//
// Keys are RecentDataset.id strings (typically the FITS file path on the
// backend). The cache files live at:
//    <QStandardPaths::AppLocalDataLocation>/thumbnails/<sha1>.png
//
// Captures are intentionally cheap and asynchronous-friendly: we just call
// QWidget::grab() on the viewer's central widget, scale to the recents-panel
// thumbnail size, and save. No backend round-trip, no separate "render
// preview" pipeline. The thumbnail represents the *last view* of a dataset —
// if the user re-opens with different parameters the old thumbnail stays
// until the next close.
// ---------------------------------------------------------------------------
namespace ThumbnailCache {

// Recents panel thumbnails are 96×64 (Mission Control design). All capture
// helpers below scale to this aspect at full size; the loader honors caller-
// supplied dimensions when the row needs a different size.
constexpr int kDefaultWidth  = 96;
constexpr int kDefaultHeight = 64;

// Returns the absolute path where the PNG for `id` lives on disk. Creates
// the parent directory on first call. Returns an empty string if the cache
// directory could not be created.
QString pathFor(const QString &id);

// Persists `pixmap` (already at the correct size) for `id`. Atomic via a
// tmp file + rename. Returns true on success.
bool save(const QString &id, const QPixmap &pixmap);

// Loads the cached thumbnail for `id` at the given size. Returns a null
// QPixmap if no cache file exists (or cannot be read).
QPixmap load(const QString &id,
             int width = kDefaultWidth,
             int height = kDefaultHeight);

// Captures `widget`'s current contents and saves it as the thumbnail for
// `id`. Scales to (kDefaultWidth × kDefaultHeight) preserving aspect ratio
// (cropped to fill — the thumbnail is a tile, not a letterbox). Returns the
// path written, or an empty string on failure.
//
// Safe to call from a viewer's closeEvent; the grab is synchronous and runs
// against the widget's current backbuffer.
QString captureFromWidget(QWidget *widget, const QString &id);

// Returns a small typed placeholder pixmap — a stylised gradient/glyph the
// Recent panel shows while no real thumbnail has been captured yet. `type`
// matches RecentDataset::type ("cube" | "image" | "catalogue" | "vbt" |
// "vbt-volume" | "hips").
QPixmap placeholder(const QString &type,
                    int width = kDefaultWidth,
                    int height = kDefaultHeight);

} // namespace ThumbnailCache

#endif // ThumbnailCache_h

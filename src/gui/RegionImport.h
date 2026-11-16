#ifndef RegionImport_h
#define RegionImport_h

// RegionImport.h
// Tools ▸ Regions ▸ Import Region in both viewers (issue #342).
//
// The file is read by the backend with the dataset (`POST /v1/regions/parse`,
// `to_frame: image`): every region of every shape, in any frame and
// coordinate format, placed with the dataset's full WCS at sub-pixel
// precision. The viewer gets the regions for its list, and a non-modal report
// lists every line that did not become a region with the reason.

#include "RegionImportResult.h"
#include "RegionListExport.h"

#include <QString>

#include <functional>

class QMainWindow;
class QWidget;

namespace visivo::regions {

/// Ask for a region file (or read @p path when given), have the backend read
/// it, and hand the result to @p apply on the GUI thread, unless @p window has
/// gone meanwhile. Then the report opens when some lines were not read.
/// @p note is added to the status line that reports the import.
void importRegionFile(QMainWindow *window, const ExportContext &ctx,
                      std::function<void(const ImportResult &)> apply,
                      const QString &path = QString(), const QString &note = QString());

/// The non-modal report of an import: what was read, and each line not read
/// with its reason. Owned by @p parent, deleted on close.
void showImportReport(QWidget *parent, const QString &fileName, const ImportResult &result);

}   // namespace visivo::regions

#endif   // RegionImport_h

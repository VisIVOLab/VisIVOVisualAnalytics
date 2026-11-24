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
#include <vector>

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

/// The regions a window sends to a linked one: the selected region with the
/// rings it is grouped with (an annulus drawn as one moves as one), or all.
inline std::vector<Region> regionsToSend(const RegionList &list, bool selectedOnly)
{
    if (!selectedOnly)
        return list.regions();
    std::vector<Region> out;
    for (int id : list.ringGroup(list.selectedId()))
        if (const Region *r = list.find(id))
            out.push_back(*r);
    return out;
}

/// Regions of the window's dataset (@p from, in its 0-based pixels) put on
/// another dataset's pixels through the sky (issue #522): the backend places
/// them on the sky with the first dataset's full WCS and back on the pixels
/// with the second's (`POST /v1/regions/convert` twice, each dataset on its own
/// backend), so a circle stays a circle and a box turns with the other grid.
/// Regions off the other projection come back as diagnostics of the result.
/// @p apply gets the regions on the GUI thread unless @p window has gone; the
/// report opens when some were not placed. @p label names where they came from
/// (their group in Session Data).
void sendRegionsToDataset(QMainWindow *window, const std::vector<Region> &regions,
                          const ExportContext &from, const ExportContext &to, const QString &label,
                          std::function<void(const ImportResult &)> apply);

/// The non-modal report of an import: what was read, and each line not read
/// with its reason. Owned by @p parent, deleted on close.
void showImportReport(QWidget *parent, const QString &fileName, const ImportResult &result);

}   // namespace visivo::regions

#endif   // RegionImport_h

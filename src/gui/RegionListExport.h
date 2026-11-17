#ifndef RegionListExport_h
#define RegionListExport_h

// RegionListExport.h
// Tools ▸ Regions ▸ Export Region for a window's region list (issue #341),
// shared by the image viewer and the cube viewer.
//
// Asks which regions (all, or the selected one), the format (ds9 or CASA CRTF)
// and the frame (FK5, pixels, or each region in the frame it was typed or read
// in), then writes the file through the backend (`/v1/regions/export`, and
// `/v1/regions/convert` for the regions' own frames) off the GUI thread.

#include "RegionModel.h"

#include <QString>

#include <vector>

class QMainWindow;

namespace visivo::regions {

struct ExportContext
{
    QString backendUrl;
    QString token;
    QString sessionId;
    QString datasetId;   ///< the dataset whose WCS places sky regions
    QString filePath;    ///< the dataset's file, for the default name
};

/// Run the export dialogs over @p list and write the file. Messages go to
/// @p window's status bar; nothing happens when the list is empty.
///
/// With @p only empty (Tools ▸ Regions ▸ Export Region) the user picks the
/// visible regions, all of them hidden ones included, or the selected one;
/// the visible ones come first, so a region hidden in Session Data is left
/// out unless asked (#388). With @p only given (a row or a group of Session
/// Data) those regions are written; for a group with hidden regions it asks,
/// offering all of them first.
void exportRegionList(QMainWindow *window, const RegionList &list, const ExportContext &ctx,
                      const std::vector<int> &only = {});

}   // namespace visivo::regions

#endif   // RegionListExport_h

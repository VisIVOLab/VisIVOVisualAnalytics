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
void exportRegionList(QMainWindow *window, const RegionList &list, const ExportContext &ctx);

}   // namespace visivo::regions

#endif   // RegionListExport_h

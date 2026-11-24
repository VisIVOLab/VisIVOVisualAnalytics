#ifndef ContourExport_h
#define ContourExport_h

// ContourExport.h
// Save the contours a window shows (issue #49): as a ds9 region file, one polygon
// per contour line (a line that leaves the image is a segment) with its colour,
// width, dash and level, or as a table of vertices. The vertices are in the sky
// frame the user picks (the window's own frame is offered first) or in image
// pixels. The sky positions come from the backend with the dataset's WCS
// (`/v1/regions/export`, `/v1/regions/convert`), off the GUI thread, as for
// region files.

#include "ContourSettings.h"
#include "RegionListExport.h"

#include <QString>

#include <cstddef>
#include <vector>

class QMainWindow;

namespace visivo::contours {

/// More segments than this are not written: the file would be of no use to ds9
/// (a contour of noise, mostly) and the request too big for the backend.
constexpr std::size_t kMaxExportSegments = 2'000'000;

/// Ask for the file kind, the frame and the path, then write the file: the
/// segments of @p source are joined into lines and sent off the GUI thread, with
/// a progress dialog that can stop it. Messages go to @p window's status bar. @p defaultFrame is "galactic", "fk5" or
/// "ecliptic". With no dataset on the backend only image pixels are offered.
void exportContours(QMainWindow *window, LineSource source,
                    const visivo::regions::ExportContext &ctx, const QString &defaultFrame);

}   // namespace visivo::contours

#endif   // ContourExport_h

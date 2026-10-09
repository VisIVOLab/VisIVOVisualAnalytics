#ifndef CubeStatsUtil_h
#define CubeStatsUtil_h

// Shared pixel-statistics / formatting helpers extracted from vtkWindowCube.cpp.
// Used across the probe, histogram, data-state panel and remote-slice paths.

#include <QString>

class vtkImageData;

namespace CubeStats {

void computeImageStats(vtkImageData *img, double &meanOut, double &rmsOut);
QString fmtStat(double v);

}  // namespace CubeStats

#endif  // CubeStatsUtil_h

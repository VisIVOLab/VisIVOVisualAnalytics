#include "CubeStatsUtil.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkPointData.h>

#include <QString>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace CubeStats {

void computeImageStats(vtkImageData *img, double &meanOut, double &rmsOut)
{
    constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
    meanOut = qnan;
    rmsOut  = qnan;
    if (!img) return;
    vtkDataArray *scalars = img->GetPointData()->GetScalars();
    if (!scalars) return;
    const vtkIdType n = scalars->GetNumberOfTuples();
    double sum = 0.0, sumSq = 0.0;
    vtkIdType count = 0;
    for (vtkIdType i = 0; i < n; ++i) {
        const double v = scalars->GetComponent(i, 0);
        if (!std::isfinite(v)) continue;
        sum   += v;
        sumSq += v * v;
        ++count;
    }
    if (count == 0) return;
    meanOut = sum / static_cast<double>(count);
    rmsOut  = std::sqrt(sumSq / static_cast<double>(count));
}

QString fmtStat(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 6) : u"-"_s;
}

}  // namespace CubeStats

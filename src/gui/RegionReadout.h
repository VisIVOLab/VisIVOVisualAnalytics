#ifndef RegionReadout_h
#define RegionReadout_h

// RegionReadout.h
// The live numbers shown while a region is being dragged.
//
// Until now the statistics of a region appeared only when the drag ended, in a
// dialog: you chose the region blind, read the numbers, closed the dialog and
// dragged again. The readout puts the sum and the mean under the cursor while
// the region is still being drawn — which is what makes the *choice* of region
// something you can aim.
//
// That has a cost the final dialog does not have: it runs on every mouse move.
// The full statistics sort the pixels (for the median and the MAD), which on a
// megapixel region is a tenth of a second per move — so the readout computes
// only what one pass can give, over at most a fixed budget of pixels, and says
// when it sampled rather than pretending the number is exact.

#include <QString>

#include <algorithm>
#include <cmath>

namespace visivo {

/// How many pixels the live readout is willing to touch per mouse move.
///
/// Measured on this budget (8192² float image, -O2, Apple M-series): a circular
/// region costs 0.16–0.26 ms and a 12-vertex polygon 0.75–1.2 ms, and — this is
/// the point of the stride — those figures are flat from 1 Mpx to 64 Mpx. A
/// mouse move at 60 Hz has 16 ms, so the readout spends at most ~7% of it.
/// The final statistics, computed once on release, use every pixel.
inline constexpr long long kRegionReadoutBudget = 200000;

/// Step between sampled pixels along each axis, so that a region of
/// ``pixels`` costs at most ``budget``. Always ≥ 1.
inline int regionSampleStride(long long pixels, long long budget = kRegionReadoutBudget)
{
    if (pixels <= 0 || budget <= 0)
        return 1;
    if (pixels <= budget)
        return 1;
    // The stride applies to both axes, so the cost falls as its square.
    const double ratio = static_cast<double>(pixels) / static_cast<double>(budget);
    return std::max(1, static_cast<int>(std::ceil(std::sqrt(ratio))));
}

/// The text of the readout: what the region is, then Σ and μ.
///
/// `≈` marks a sampled figure. It is not decoration: a sampled sum is an
/// estimate scaled from a fraction of the pixels, and a reader who takes it for
/// the real integrated flux has been misled by a number that looked exact.
inline QString regionReadoutText(const QString &shape, double sum, double mean,
                                 long long validPixels, bool sampled)
{
    if (validPixels <= 0)
        return shape.isEmpty() ? QStringLiteral("no valid pixels")
                               : shape + QStringLiteral("  ·  no valid pixels");
    const QString mark = sampled ? QStringLiteral("≈ ") : QString();
    const QString numbers = QStringLiteral("Σ %1%2  ·  μ %3%4")
                                    .arg(mark)
                                    .arg(sum, 0, 'g', 4)
                                    .arg(mark)
                                    .arg(mean, 0, 'g', 4);
    return shape.isEmpty() ? numbers : shape + QStringLiteral("  ·  ") + numbers;
}

}   // namespace visivo

#endif   // RegionReadout_h

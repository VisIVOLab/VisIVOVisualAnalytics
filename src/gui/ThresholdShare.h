#pragma once

// ThresholdShare.h
// What the 3-D threshold does, said in the voxels it keeps (issue #83): the
// share of the cube's voxels above it, read from the viewer's own copy of the
// cube. On a field spanning decades (a simulation's density) the value alone
// says little; "1 % of voxels drawn" says what moving it does.
//
// No widgets, so it can be tested.

#include <QString>

#include <cmath>
#include <cstddef>
#include <optional>

namespace visivo {

/// The share (0..1) of the values above `threshold`, among those that are
/// finite and above `sentinel` (the value masked voxels carry). A large cube is
/// read with a stride, about `maxSamples` values. None when no value counts.
inline std::optional<double> shareAbove(const float *values, std::size_t n, double threshold,
                                        double sentinel, std::size_t maxSamples = 1u << 21)
{
    if (!values || n == 0)
        return std::nullopt;
    // Odd, so that a step that is a multiple of a row does not read one column.
    const std::size_t step = maxSamples > 0 && n > maxSamples ? (n / maxSamples) | 1u : 1;
    std::size_t counted = 0, above = 0;
    for (std::size_t i = 0; i < n; i += step) {
        const float v = values[i];
        if (!std::isfinite(v) || !(v > sentinel))
            continue;
        ++counted;
        if (v > threshold)
            ++above;
    }
    if (counted == 0)
        return std::nullopt;
    return static_cast<double>(above) / static_cast<double>(counted);
}

/// "1.0 % of voxels above", with as many digits as a small share needs.
/// `someAbove`: the data goes above the threshold although the sample found
/// nothing there (a threshold that keeps a hundred voxels of a large cube).
inline QString shareAboveText(double share, bool someAbove = false)
{
    const double percent = 100.0 * share;
    QString number;
    if (percent <= 0.0)
        number = someAbove ? QStringLiteral("< 0.01") : QStringLiteral("0");
    else if (percent < 100.0 && percent > 99.9)
        number = QStringLiteral("> 99.9");
    else if (percent < 0.01)
        number = QStringLiteral("< 0.01");
    else if (percent < 1.0)
        number = QString::number(percent, 'f', percent < 0.1 ? 3 : 2);
    else if (percent < 10.0)
        number = QString::number(percent, 'f', 1);
    else
        number = QString::number(percent, 'f', 0);
    return QStringLiteral("%1 % of voxels above").arg(number);
}

} // namespace visivo

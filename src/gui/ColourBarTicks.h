#pragma once

// ColourBarTicks.h
// The labels down a colour bar, the 3-D view's and the ray-traced views'
// alike (VolumeColorbar): one notation for every tick.
//
// No widgets, so it can be tested.

#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace visivo {

/// The labels of `values`: three significant digits; written out when they
/// reach a thousand (a velocity in km/s), with the decimals their spacing
/// needs; else in exponents when any needs one ("0.000183" over "5.31e-06"
/// reads as two scales).
inline QStringList tickLabels(const QVector<double> &values)
{
    QStringList labels;
    bool exponent = false;
    double largest = 0.0, step = std::numeric_limits<double>::infinity();
    for (int i = 0; i < values.size(); ++i) {
        labels << QString::number(values[i], 'g', 3);
        exponent = exponent || labels.back().contains(u'e');
        largest = std::max(largest, std::abs(values[i]));
        if (i > 0 && values[i] != values[i - 1])
            step = std::min(step, std::abs(values[i] - values[i - 1]));
    }
    if (!exponent)
        return labels;
    if (largest >= 1e3 && largest < 1e5 && std::isfinite(step)) {
        const int decimals = std::clamp(1 - static_cast<int>(std::floor(std::log10(step))), 0, 3);
        QStringList fixed;
        bool readable = true;
        for (int i = 0; i < values.size(); ++i) {
            fixed << QString::number(values[i], 'f', decimals);
            // Not when two ticks read alike, or a value reads as nothing (a
            // log bar over many decades): exponents then.
            readable = readable && (values[i] == 0.0 || fixed.back().toDouble() != 0.0)
                    && (i == 0 || values[i] == values[i - 1] || fixed[i] != fixed[i - 1]);
        }
        if (readable)
            return fixed;
    }
    labels.clear();
    for (const double v : values)
        labels << QString::number(v, 'e', 2);
    return labels;
}

} // namespace visivo

#pragma once

// RemotePointStyle.h
// The ``set_point_style`` command a 3-D catalogue window sends to the
// backend's point session (Remote view), from the size slider. Kept apart from
// the window so the size the Remote view receives while the slider is dragged
// can be tested (issue #456). Header-only, no widgets.

#include "ParticleSplatDefaults.h"

#include <QString>
#include <QVariantMap>

#include <cmath>

namespace visivo::remote {

/// ``intensity`` of the point session: its standard brightness, as the
/// desktop has no control for it.
inline constexpr int kPointStyleIntensity = 35;

/// The ``set_point_style`` command for the size slider's @p sliderValue.
/// Splats take the same continuous size step as the local view; points are
/// sized in physical pixels, since that is what the backend draws in.
inline QVariantMap pointStyleCommand(bool gaussian, int sliderValue, double devicePixelRatio)
{
    const QVariant size = gaussian
            ? QVariant(ParticleSplatDefaults::splatSizeForSlider(sliderValue))
            : QVariant(static_cast<int>(std::lround(
                      ParticleSplatDefaults::pointSizeForSlider(sliderValue)
                      * devicePixelRatio)));
    return QVariantMap{ { QStringLiteral("type"), QStringLiteral("set_point_style") },
                        { QStringLiteral("mode"),
                          gaussian ? QStringLiteral("gaussian") : QStringLiteral("points") },
                        { QStringLiteral("size"), size },
                        { QStringLiteral("intensity"), kPointStyleIntensity } };
}

} // namespace visivo::remote

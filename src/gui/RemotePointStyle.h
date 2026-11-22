#pragma once

// RemotePointStyle.h
// The ``set_point_style`` command a 3-D catalogue window sends to the
// backend's point session (Remote view), from the size slider. Kept apart from
// the window so the size the Remote view receives while the slider is dragged
// can be tested (issue #456). Header-only, no widgets.

#include "ParticleSplatDefaults.h"

#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <functional>

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
            : QVariant(std::max(1, static_cast<int>(std::lround(
                      ParticleSplatDefaults::pointSizeForSlider(sliderValue)
                      * devicePixelRatio))));
    return QVariantMap{ { QStringLiteral("type"), QStringLiteral("set_point_style") },
                        { QStringLiteral("mode"),
                          gaussian ? QStringLiteral("gaussian") : QStringLiteral("points") },
                        { QStringLiteral("size"), size },
                        { QStringLiteral("intensity"), kPointStyleIntensity } };
}

/// What a move of the size slider updates. The local view is restyled only
/// when it has rows; the backend's views are told in any case, as a table
/// shown only there has no local rows and used to keep its old size
/// (issue #456).
struct SizeSliderTargets
{
    std::function<void()> restyleLocal;
    std::function<void()> updateServerViews;
};

inline void applySizeSlider(bool hasLocalRows, const SizeSliderTargets &targets)
{
    if (hasLocalRows && targets.restyleLocal)
        targets.restyleLocal();
    if (targets.updateServerViews)
        targets.updateServerViews();
}

} // namespace visivo::remote

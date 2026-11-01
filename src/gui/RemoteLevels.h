#ifndef RemoteLevels_h
#define RemoteLevels_h

// RemoteLevels.h
// The starting values of a remote 3-D view (issue #82).
//
// A remote session renders with a threshold and over a data range the backend
// computed from data the desktop does not have: the full-resolution cube, or a
// sample of its bricks. The cube viewer shows those values in its Threshold
// field and slider instead of its own preview-based default, and sends a
// threshold only once the user has chosen one.
//
// This header reads the backend's answer (create_session or /levels) and does
// the arithmetic of the slider's bounds, with no widgets, so it can be tested.

#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <optional>

namespace visivo {

struct RemoteLevels
{
    std::optional<double> threshold;
    std::optional<double> dataMin;
    std::optional<double> dataMax;
    /// The cube is still streaming into the session: ask /levels again.
    bool pending = false;
};

/// The values in a create_session answer (``levels_pending``) or a /levels
/// answer (``pending``). A null or missing value is not known yet; a range
/// that is empty or reversed is dropped.
inline RemoteLevels parseRemoteLevels(const QJsonObject &o)
{
    RemoteLevels l;
    const auto number = [&o](const char *key) -> std::optional<double> {
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
    };
    l.threshold = number("threshold");
    l.dataMin = number("data_min");
    l.dataMax = number("data_max");
    if (!l.dataMin || !l.dataMax || !(*l.dataMax > *l.dataMin)) {
        l.dataMin.reset();
        l.dataMax.reset();
    }
    l.pending = o.value(QLatin1String("pending")).toBool()
            || o.value(QLatin1String("levels_pending")).toBool();
    return l;
}

/// The bounds of the threshold slider.
struct ThresholdBounds
{
    float lower = 0.f;
    float upper = 1.f;
};

/// The slider's bounds over a data range: from 3 % of the span above the
/// minimum (below it is the constant background) to the maximum, as for a
/// preview's range.
inline ThresholdBounds boundsFromRange(double min, double max)
{
    return { static_cast<float>(min + 0.03 * (max - min)), static_cast<float>(max) };
}

/// Bounds that hold both: the backend's full-resolution range is wider than
/// a preview's (averaging narrows it), and the local view keeps its own.
inline ThresholdBounds unite(ThresholdBounds a, ThresholdBounds b)
{
    return { std::min(a.lower, b.lower), std::max(a.upper, b.upper) };
}

/// Bounds widened to hold ``value``: the backend's threshold is shown as it
/// is, never clamped to a range the desktop took from its preview.
inline ThresholdBounds including(ThresholdBounds b, float value)
{
    return { std::min(b.lower, value), std::max(b.upper, value) };
}

/// The slider's position, 0..100, for ``value``.
inline int thresholdSliderPosition(ThresholdBounds b, float value)
{
    const float span = b.upper - b.lower;
    if (!(span > 0.f))
        return 0;
    return std::clamp(static_cast<int>(100.f * (value - b.lower) / span), 0, 100);
}

} // namespace visivo

#endif // RemoteLevels_h

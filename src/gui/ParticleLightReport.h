#ifndef PARTICLE_LIGHT_REPORT_H
#define PARTICLE_LIGHT_REPORT_H
// ParticleLightReport.h
// What a ray-traced particle render left out of the light (issue #414, items
// 4 and 5): the rows with no usable light (not finite, not positive, or 0 in
// float32) and the rows capped because their column mixes two populations
// (gas and stars), from the backend's X-Visivo-Particle-Dropped/-Clipped
// headers or a render task's result. Before, such rows went missing without a
// word. Also the request's ``period`` and the particle-type column a
// "show gas only" suggestion filters on. No widgets: tests/test_particle_light_report.cpp.

#include <QByteArray>
#include <QJsonObject>
#include <QLocale>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace visivo {

struct ParticleLightReport
{
    /// Rows left out of the light, and rows whose light was capped; -1: the
    /// backend did not say (an older one).
    qint64 dropped{ -1 };
    qint64 clipped{ -1 };
    /// The span of the light column's finite values, in decades; NaN: unknown
    /// (the preview and particle routes do not send it).
    double spanDecades{ std::numeric_limits<double>::quiet_NaN() };
    /// The particles drawn (to judge how much was clipped).
    qint64 particles{ 0 };

    bool any() const { return dropped > 0 || clipped > 0; }
};

namespace detail {

inline qint64 headerCount(const QByteArray &value)
{
    bool ok = false;
    const qint64 n = value.trimmed().toLongLong(&ok);
    return ok && n >= 0 ? n : -1;
}

} // namespace detail

inline ParticleLightReport lightReportFromHeaders(const QByteArray &dropped,
                                                  const QByteArray &clipped, qint64 particles)
{
    ParticleLightReport r;
    r.dropped = detail::headerCount(dropped);
    r.clipped = detail::headerCount(clipped);
    r.particles = std::max<qint64>(0, particles);
    return r;
}

inline ParticleLightReport lightReportFromResult(const QJsonObject &result)
{
    ParticleLightReport r;
    const auto count = [&result](const QString &key) -> qint64 {
        const QJsonValue v = result.value(key);
        return v.isDouble() && v.toDouble() >= 0.0 ? static_cast<qint64>(v.toDouble()) : -1;
    };
    r.dropped = count(QStringLiteral("dropped_rows"));
    r.clipped = count(QStringLiteral("clipped_rows"));
    const QJsonValue span = result.value(QStringLiteral("light_span_decades"));
    if (span.isDouble() && std::isfinite(span.toDouble()))
        r.spanDecades = span.toDouble();
    r.particles = std::max<qint64>(0, count(QStringLiteral("particles")));
    return r;
}

/// The status line's words, empty when nothing was left out:
/// "1,204 rows dropped (no light) · 539,519 rows clipped".
inline QString lightReportText(const ParticleLightReport &r, const QLocale &locale = QLocale())
{
    QStringList parts;
    if (r.dropped > 0)
        parts << QStringLiteral("%1 rows dropped (no light)").arg(locale.toString(r.dropped));
    if (r.clipped > 0)
        parts << QStringLiteral("%1 rows clipped").arg(locale.toString(r.clipped));
    return parts.join(QStringLiteral(" · "));
}

inline QString lightReportTooltip(const ParticleLightReport &r)
{
    QStringList lines;
    if (r.dropped > 0)
        lines << QStringLiteral(
                "Dropped: rows whose light value is not a number, is zero or negative, or is "
                "too small for the GPU's float. They are not drawn; with sizes from "
                "neighbours, they still count as neighbours.");
    if (r.clipped > 0)
        lines << QStringLiteral(
                "Clipped: the light column mixes two populations many decades apart (for "
                "example gas and stars). The fainter, larger one sets the brightness, and the "
                "other is capped at its brightest row, so it does not swamp everything else.");
    if (std::isfinite(r.spanDecades))
        lines << QStringLiteral("The light column spans %1 decades.")
                         .arg(r.spanDecades, 0, 'f', 1);
    return lines.join(QStringLiteral("\n\n"));
}

/// A column that tells particle types apart (Gadget's 0 = gas), or empty.
inline QString particleTypeColumn(const QStringList &columns)
{
    for (const QString &column : columns) {
        QString key;
        for (const QChar c : column)
            if (c.isLetterOrNumber())
                key += c.toLower();
        if (key == QLatin1String("type") || key == QLatin1String("ptype")
            || key == QLatin1String("parttype") || key == QLatin1String("particletype"))
            return column;
    }
    return {};
}

/// The light spans so many decades that the backend counts two populations
/// (backend/app/particle_raytrace.py, normalised_light: more than 20).
inline constexpr double kTwoPopulationDecades = 20.0;

/// Whether to suggest showing one particle type: the table can be filtered by
/// type, and the render capped many rows because two populations share the
/// light column. Without the span (the preview and particle routes), at least
/// 1 % of the particles drawn, or 1,000 rows when their number is not known
/// (a backend frame).
inline bool suggestTypeFilter(const ParticleLightReport &r, const QString &typeColumn)
{
    if (typeColumn.isEmpty() || r.clipped <= 0)
        return false;
    if (std::isfinite(r.spanDecades))
        return r.spanDecades > kTwoPopulationDecades;
    if (r.particles <= 0)
        return r.clipped >= 1000;
    return r.clipped * 100 >= r.particles;
}

/// The value of the gas type in a type column (Gadget's convention).
inline constexpr int kGasType = 0;

/// Whether a snapshot's metadata (the open's ``snapshot``) describes a
/// periodic box, as the backend decides it (gadget_snapshot.Component.periods):
/// a box size, and the run stated periodic or, when nothing states it,
/// comoving.
inline bool periodicBoxKnown(const QJsonObject &snapshot)
{
    if (!(snapshot.value(QStringLiteral("box_size")).toDouble() > 0.0))
        return false;
    const QJsonObject units = snapshot.value(QStringLiteral("units")).toObject();
    const QJsonValue periodic = units.value(QStringLiteral("periodic"));
    if (periodic.isBool())
        return periodic.toBool();
    return units.value(QStringLiteral("comoving")).toBool(false);
}

/// The request's ``period`` for the "Periodic box" choice: nothing (the
/// backend's null, the table's own box) while it is on or the box is not
/// known, 0 (no wrapping) when the user turned it off.
inline void insertPeriod(QJsonObject &request, bool boxKnown, bool periodic)
{
    if (boxKnown && !periodic)
        request.insert(QStringLiteral("period"), 0);
}

} // namespace visivo

#endif // PARTICLE_LIGHT_REPORT_H

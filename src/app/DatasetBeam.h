#ifndef DatasetBeam_h
#define DatasetBeam_h

// DatasetBeam.h
// A dataset's beam and intensity unit as the user sees them, for headers that
// lack BMAJ or BUNIT (issue #416: CHIMPS line cubes carry neither, only
// TEMPSCAL = 'TA*').
//
// The backend can keep a beam typed by the user (POST /v1/datasets/beam) and
// infers the intensity unit from TEMPSCAL/BTYPE/LABEL. These are the request,
// the answer and the labels, header-only so the unit tests reach them.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cmath>
#include <limits>

namespace visivo::beam {

struct BeamRequest
{
    QString datasetId;
    double majorArcsec{ 0.0 };
    double minorArcsec{ 0.0 };   ///< 0: the backend takes the major axis
    double paDeg{ 0.0 };
    bool clear{ false };
};

struct BeamResult
{
    bool valid{ false };
    QString error;
    /// Degrees, as in the open response; NaN when there is no beam.
    double majorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double minorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double paDeg{ 0.0 };
    /// "header", "user" or "none".
    QString source;
    QString note;
};

inline QJsonObject requestBody(const BeamRequest &request)
{
    QJsonObject body{ { QStringLiteral("dataset_id"), request.datasetId } };
    if (request.clear) {
        body.insert(QStringLiteral("clear"), true);
        return body;
    }
    body.insert(QStringLiteral("major_arcsec"), request.majorArcsec);
    if (request.minorArcsec > 0.0)
        body.insert(QStringLiteral("minor_arcsec"), request.minorArcsec);
    body.insert(QStringLiteral("pa_deg"), request.paDeg);
    return body;
}

inline BeamResult parseResponse(const QJsonObject &obj)
{
    BeamResult result;
    result.valid = obj.value(QStringLiteral("valid")).toBool();
    result.error = obj.value(QStringLiteral("error")).toString();
    const auto degrees = [&obj](const char *key) {
        const QJsonValue v = obj.value(QLatin1String(key));
        return v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN();
    };
    result.majorDeg = degrees("beam_major");
    result.minorDeg = degrees("beam_minor");
    const double pa = degrees("beam_pa");
    result.paDeg = std::isfinite(pa) ? pa : 0.0;
    result.source = obj.value(QStringLiteral("beam_source")).toString();
    result.note = obj.value(QStringLiteral("note")).toString();
    if (!result.valid && result.error.isEmpty())
        result.error = QStringLiteral("The backend did not accept the beam.");
    return result;
}

inline bool hasBeam(double majorDeg, double minorDeg)
{
    return std::isfinite(majorDeg) && std::isfinite(minorDeg) && majorDeg > 0.0
            && minorDeg > 0.0;
}

/// A beam can be typed when the header has none. Only a FITS dataset on a
/// backend that keeps typed beams reports beam_source at all; an empty source
/// (an older backend, a VBT field, a dynamic spectrum) cannot take one.
inline bool canSetBeam(const QString &source)
{
    return source == QLatin1String("user") || source == QLatin1String("none");
}

/// "15.0″ × 15.0″, PA 0° (set by user)", "none in the header", …
inline QString summary(double majorDeg, double minorDeg, double paDeg, const QString &source)
{
    if (!hasBeam(majorDeg, minorDeg))
        return QStringLiteral("none in the header");
    QString text = QStringLiteral("%1″ × %2″, PA %3°")
                           .arg(majorDeg * 3600.0, 0, 'f', 1)
                           .arg(minorDeg * 3600.0, 0, 'f', 1)
                           .arg(paDeg, 0, 'f', 0);
    if (source == QLatin1String("user"))
        text += QStringLiteral(" (set by user)");
    else if (source == QLatin1String("header"))
        text += QStringLiteral(" (header)");
    return text;
}

/// The unit the values are in: the backend's inferred unit, else the raw
/// BUNIT (a backend older than intensity_unit), else empty.
inline QString intensityUnit(const QString &bunit, const QString &inferredUnit)
{
    const QString inferred = inferredUnit.trimmed();
    return inferred.isEmpty() ? bunit.trimmed() : inferred;
}

/// The y-axis title of a spectrum. A unit that did not come from BUNIT says
/// so on the axis itself, where the number is read; the note explains how in
/// the Inspector's tooltip.
inline QString intensityAxisLabel(const QString &unit, const QString &note)
{
    const QString u = unit.trimmed();
    const bool inferred = !note.trimmed().isEmpty();
    if (u.isEmpty())
        return inferred ? QStringLiteral("Intensity (unit unknown)") : QStringLiteral("Intensity");
    return inferred ? QStringLiteral("Intensity (%1, no BUNIT)").arg(u)
                    : QStringLiteral("Intensity (%1)").arg(u);
}

/// The Inspector's "Unit" value.
inline QString unitDisplay(const QString &unit, const QString &note)
{
    const QString u = unit.trimmed();
    if (u.isEmpty())
        return note.trimmed().isEmpty() ? QStringLiteral("-") : QStringLiteral("unknown (no BUNIT)");
    return note.trimmed().isEmpty() ? u : QStringLiteral("%1 (no BUNIT)").arg(u);
}

}   // namespace visivo::beam

#endif   // DatasetBeam_h

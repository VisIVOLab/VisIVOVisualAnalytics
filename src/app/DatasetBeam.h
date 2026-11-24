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

#include <algorithm>
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

/// A number with three significant digits, written in fixed notation: 22.8,
/// 0.0228, 1.23, 15.0, 120.
inline QString threeSignificant(double value)
{
    if (!(value > 0.0) || !std::isfinite(value))
        return QStringLiteral("0");
    const int magnitude = int(std::floor(std::log10(value)));
    const int decimals = std::max(0, 2 - magnitude);
    QString text = QString::number(value, 'f', decimals);
    // Rounding can carry into the next decade (9.996 → "10.00"): one digit too
    // many then, so write it with one decimal fewer.
    if (decimals > 0 && text.toDouble() >= std::pow(10.0, magnitude + 1))
        text = QString::number(value, 'f', decimals - 1);
    return text;
}

/// The unit an angle of @p deg degrees is written in: milliarcseconds below
/// 1″, arcseconds up to 60″, arcminutes up to 1°, degrees above. @p perDegree is
/// how many of that unit make a degree. The edges are read with a hair of
/// tolerance, so 1″ computed as 1/3600 × 3600 is not 999.99 mas.
struct AngularUnit
{
    double perDegree;
    const char *suffix;
};

inline AngularUnit angularUnitFor(double deg)
{
    const double arcsec = deg * 3600.0 * (1.0 + 1e-9);
    if (arcsec < 1.0)
        return { 3600.0 * 1000.0, " mas" };
    if (arcsec < 60.0)
        return { 3600.0, "\xE2\x80\xB3" };   // ″
    if (arcsec < 3600.0)
        return { 60.0, "\xE2\x80\xB2" };      // ′
    return { 1.0, "\xC2\xB0" };                // °
}

/// An angular size given in degrees, in the unit that fits it. A beam of
/// 22.8 × 18.4 mas must not read 0.02″ × 0.02″ (issue #502), so the digits
/// follow the size, not a fixed number of decimals.
inline QString formatAngularSize(double deg)
{
    if (!std::isfinite(deg) || deg <= 0.0)
        return QStringLiteral("0″");
    const AngularUnit unit = angularUnitFor(deg);
    return threeSignificant(deg * unit.perDegree) + QString::fromUtf8(unit.suffix);
}

/// "22.8 mas × 18.4 mas": both axes in the unit of the larger one, so the pair
/// reads as one measurement ("1.20″ × 0.800″" never mixes mas with ″).
inline QString formatBeamSize(double majorDeg, double minorDeg)
{
    const double larger = std::max(majorDeg, minorDeg);
    if (!std::isfinite(larger) || larger <= 0.0)
        return QStringLiteral("0″ × 0″");
    const AngularUnit unit = angularUnitFor(larger);
    const QString suffix = QString::fromUtf8(unit.suffix);
    return threeSignificant(majorDeg * unit.perDegree) + suffix + QStringLiteral(" × ")
            + threeSignificant(minorDeg * unit.perDegree) + suffix;
}

/// "15.0″ × 15.0″, PA 0° (set by user)", "none in the header", …
inline QString summary(double majorDeg, double minorDeg, double paDeg, const QString &source)
{
    if (!hasBeam(majorDeg, minorDeg))
        return QStringLiteral("none in the header");
    QString text = QStringLiteral("%1, PA %2°")
                           .arg(formatBeamSize(majorDeg, minorDeg))
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

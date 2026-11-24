#pragma once

// SimulationVolume.h
// The rules of a simulation volume in the cube viewer (issue #524), as pure
// logic: which plane is shown when the slice is taken along x, y or z, how a
// coordinate typed in the grid's units picks a plane, how a plane is labelled,
// and the physical units (UNIT_DENSITY, UNIT_LENGTH, UNIT_VELOCITY) the backend
// reports for a run. No widgets, no VTK: tests/test_simulation_volume.cpp.
//
// A spectral cube steps through channels along its third axis only. A volume of
// a simulation (a PLUTO variable, a VBT field) has no spectrum: any of its
// three axes can be the one the slice is normal to, and every axis is a
// coordinate in the grid's units. Data stay in the file's code units; a viewer
// shows value * factor in the unit the backend names.

#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace visivo::simvol {

/// The cube axis the slice plane is normal to.
enum class SliceAxis { X = 0, Y = 1, Z = 2 };

inline QString axisLetter(SliceAxis axis)
{
    return axis == SliceAxis::X ? QStringLiteral("x")
            : (axis == SliceAxis::Y ? QStringLiteral("y") : QStringLiteral("z"));
}

inline SliceAxis axisFromIndex(int index)
{
    return index == 0 ? SliceAxis::X : (index == 1 ? SliceAxis::Y : SliceAxis::Z);
}

/// The axis a letter names ("x", "y", "z", any case); Z for anything else.
inline SliceAxis axisFromLetter(const QString &letter)
{
    const QString l = letter.trimmed().toLower();
    return l == QLatin1String("x") ? SliceAxis::X : (l == QLatin1String("y") ? SliceAxis::Y
                                                                              : SliceAxis::Z);
}

/// How many planes there are along @p axis of a @p width x @p height x @p depth cube.
inline int planeCount(SliceAxis axis, int width, int height, int depth)
{
    const int n = axis == SliceAxis::X ? width : (axis == SliceAxis::Y ? height : depth);
    return std::max(1, n);
}

/// The cube axes a plane normal to @p axis shows horizontally and vertically:
/// z planes are x by y, y planes x by z, x planes y by z (the order the
/// backend returns them in).
inline std::array<int, 2> planeAxes(SliceAxis axis)
{
    return axis == SliceAxis::Z ? std::array<int, 2>{ 0, 1 }
            : (axis == SliceAxis::Y ? std::array<int, 2>{ 0, 2 } : std::array<int, 2>{ 1, 2 });
}

/// World coordinate of 0-based plane @p index of an axis (@p crval at the
/// 1-based pixel @p crpix, @p cdelt per plane).
inline double axisWorld(double crval, double crpix, double cdelt, int index)
{
    return crval + (static_cast<double>(index) + 1.0 - crpix) * cdelt;
}

/// The 0-based plane of an @p n-plane axis nearest to @p coordinate. A
/// coordinate beyond the first or last plane by more than half a plane is
/// clamped to it and @p inside (when given) says false, so the caller can tell
/// the user the typed value is off the grid. -1 when the axis has no spacing.
inline int nearestPlane(double coordinate, double crval, double crpix, double cdelt, int n,
                        bool *inside = nullptr)
{
    if (inside)
        *inside = false;
    if (!std::isfinite(coordinate) || !std::isfinite(cdelt) || cdelt == 0.0 || n < 1)
        return -1;
    const double pixel = (coordinate - crval) / cdelt + crpix - 1.0;
    if (inside)
        *inside = pixel >= -0.5 && pixel <= n - 0.5;
    return std::clamp(static_cast<int>(std::lround(pixel)), 0, n - 1);
}

/// A number the way a coordinate or value is read on screen: five significant
/// digits, scientific when the magnitude needs it ("1.5e+18", "0.15").
inline QString formatNumber(double value)
{
    if (!std::isfinite(value))
        return QStringLiteral("-");
    if (value == 0.0)
        return QStringLiteral("0");
    return QString::number(value, 'g', 5);
}

/// "x = 0.15 pc", or "x = 0.15" without a unit.
inline QString coordinateLabel(SliceAxis axis, double world, const QString &unit)
{
    const QString value = formatNumber(world);
    return unit.trimmed().isEmpty()
            ? QStringLiteral("%1 = %2").arg(axisLetter(axis), value)
            : QStringLiteral("%1 = %2 %3").arg(axisLetter(axis), value, unit.trimmed());
}

/// The physical units of one simulation variable, as /v1/datasets/open and
/// /v1/datasets/units report them: value * valueFactor is in valueUnit, a
/// coordinate * lengthFactor in lengthUnit, a time * timeFactor in timeUnit.
/// Each *Source is "definitions" (the run's definitions.h), "user" or "none".
struct PhysicalUnits
{
    bool active{ false };
    QString source{ QStringLiteral("none") };
    QString origin;          ///< the definitions file the factors came from
    QString variable;
    double unitDensity{ std::numeric_limits<double>::quiet_NaN() };
    double unitLength{ std::numeric_limits<double>::quiet_NaN() };
    double unitVelocity{ std::numeric_limits<double>::quiet_NaN() };
    double valueFactor{ 1.0 };
    QString valueUnit;
    QString valueQuantity;
    QString valueSource{ QStringLiteral("none") };
    double lengthFactor{ 1.0 };
    QString lengthUnit;
    QString lengthSource{ QStringLiteral("none") };
    double timeFactor{ 1.0 };
    QString timeUnit;
    double timeCode{ std::numeric_limits<double>::quiet_NaN() };

    bool hasValue() const { return valueSource != QLatin1String("none") && valueFactor > 0.0; }
    bool hasLength() const { return lengthSource != QLatin1String("none") && lengthFactor > 0.0; }
    bool hasTime() const { return timeFactor > 0.0 && !timeUnit.isEmpty() && std::isfinite(timeCode); }
};

inline PhysicalUnits parsePhysicalUnits(const QJsonObject &o)
{
    PhysicalUnits u;
    if (o.isEmpty())
        return u;
    const auto number = [&o](const char *key, double fallback) {
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isDouble() ? v.toDouble() : fallback;
    };
    const auto text = [&o](const char *key, const QString &fallback = QString()) {
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isString() ? v.toString() : fallback;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    u.active = o.value(QLatin1String("active")).toBool(false);
    u.source = text("source", u.source);
    u.origin = text("origin");
    u.variable = text("variable");
    u.unitDensity = number("unit_density", nan);
    u.unitLength = number("unit_length", nan);
    u.unitVelocity = number("unit_velocity", nan);
    u.valueFactor = number("value_factor", 1.0);
    u.valueUnit = text("value_unit");
    u.valueQuantity = text("value_quantity");
    u.valueSource = text("value_source", u.valueSource);
    u.lengthFactor = number("length_factor", 1.0);
    u.lengthUnit = text("length_unit");
    u.lengthSource = text("length_source", u.lengthSource);
    u.timeFactor = number("time_factor", 1.0);
    u.timeUnit = text("time_unit");
    u.timeCode = number("time_code", nan);
    return u;
}

/// What the viewer multiplies the file's values by: the factor when physical
/// units are on and known, else 1 (the file's code units).
inline double valueScale(const PhysicalUnits &u, bool physical)
{
    return physical && u.hasValue() ? u.valueFactor : 1.0;
}

inline double lengthScale(const PhysicalUnits &u, bool physical)
{
    return physical && u.hasLength() ? u.lengthFactor : 1.0;
}

/// The unit the values are shown in; @p codeUnit (the header's BUNIT, usually
/// empty for a simulation) when physical units are off or unknown.
inline QString valueUnitLabel(const PhysicalUnits &u, bool physical, const QString &codeUnit)
{
    return physical && u.hasValue() ? u.valueUnit : codeUnit;
}

inline QString lengthUnitLabel(const PhysicalUnits &u, bool physical, const QString &codeUnit)
{
    return physical && u.hasLength() ? u.lengthUnit : codeUnit;
}

/// The output's time, "t = 3 (code units)" or, with physical units on and the
/// run's length and velocity known, "t = 9.5e+12 s". Empty when there is none.
inline QString timeLabel(const PhysicalUnits &u, bool physical)
{
    if (!std::isfinite(u.timeCode))
        return QString();
    if (physical && u.hasTime())
        return QStringLiteral("t = %1 %2").arg(formatNumber(u.timeCode * u.timeFactor), u.timeUnit);
    return QStringLiteral("t = %1 (code units)").arg(formatNumber(u.timeCode));
}

/// The body of POST /v1/datasets/units. Fields left at NaN (or empty) are not
/// sent; @p clear forgets everything typed first.
struct UnitsRequest
{
    double unitDensity{ std::numeric_limits<double>::quiet_NaN() };
    double unitLength{ std::numeric_limits<double>::quiet_NaN() };
    double unitVelocity{ std::numeric_limits<double>::quiet_NaN() };
    double valueFactor{ std::numeric_limits<double>::quiet_NaN() };
    QString valueUnit;
    double lengthFactor{ std::numeric_limits<double>::quiet_NaN() };
    QString lengthUnit;
    bool clear{ false };
};

inline QJsonObject unitsRequestBody(const QString &datasetId, const UnitsRequest &r)
{
    QJsonObject body{ { QStringLiteral("dataset_id"), datasetId },
                      { QStringLiteral("clear"), r.clear } };
    const auto put = [&body](const char *key, double v) {
        if (std::isfinite(v))
            body.insert(QLatin1String(key), v);
    };
    put("unit_density", r.unitDensity);
    put("unit_length", r.unitLength);
    put("unit_velocity", r.unitVelocity);
    if (std::isfinite(r.valueFactor)) {
        body.insert(QStringLiteral("value_factor"), r.valueFactor);
        body.insert(QStringLiteral("value_unit"), r.valueUnit);
    }
    if (std::isfinite(r.lengthFactor)) {
        body.insert(QStringLiteral("length_factor"), r.lengthFactor);
        body.insert(QStringLiteral("length_unit"), r.lengthUnit);
    }
    return body;
}

/// The answer of POST /v1/datasets/units.
struct UnitsResult
{
    bool valid{ false };
    QString error;
    PhysicalUnits units;
};

inline UnitsResult parseUnitsResult(const QJsonObject &o)
{
    UnitsResult r;
    r.valid = o.value(QStringLiteral("valid")).toBool(false);
    r.error = o.value(QStringLiteral("error")).toString();
    r.units = parsePhysicalUnits(o.value(QStringLiteral("physical_units")).toObject());
    return r;
}

}   // namespace visivo::simvol

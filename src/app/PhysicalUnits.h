#pragma once

// PhysicalUnits.h
// The physical units every viewer shows a dataset in, as pure logic: what
// /v1/datasets/open and /v1/datasets/units report (a factor and a unit per
// column; for a simulation volume its values, coordinates and time), how a
// value, a coordinate or a time is labelled with them, and the body of the
// request that types them. The same for a PLUTO run, a VBT field, a FITS cube
// and a Gadget snapshot's columns: the backend's one table
// (backend/app/physical_units.py). Data stay in the file's code units; a viewer
// shows value * factor in the unit the backend names. No widgets, no VTK:
// tests/test_simulation_volume.cpp.

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <cmath>
#include <limits>

namespace visivo::units {

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

/// One column's conversion: value * factor is in unit; source is "definitions",
/// "user" or empty (the reader's own unit system, e.g. a Gadget snapshot's).
struct ColumnScale
{
    double factor{ 1.0 };
    QString unit;
    QString source;
};

/// The column names of a volume (the backend's app/physical_units.py): its
/// values, its three coordinates and its time. A Gadget snapshot's columns are
/// its own ("mass", "hsml", ...).
inline const QString &valueColumn() { static const QString n = QStringLiteral("value"); return n; }
inline const QString &timeColumn() { static const QString n = QStringLiteral("time"); return n; }
inline QString axisColumn(int cubeAxis)
{
    return cubeAxis == 0 ? QStringLiteral("x") : (cubeAxis == 1 ? QStringLiteral("y")
                                                                : QStringLiteral("z"));
}

/// The physical units of a dataset, as /v1/datasets/open and
/// /v1/datasets/units report them: one table for every reader (the backend's
/// app/physical_units.py), a factor and a unit per column. A column that is
/// absent has no known conversion.
struct PhysicalUnits
{
    bool active{ false };
    QString source{ QStringLiteral("none") };
    QString origin;          ///< the definitions file the factors came from
    QString variable;
    QString valueQuantity;   ///< density, velocity, pressure, magnetic or empty
    double unitDensity{ std::numeric_limits<double>::quiet_NaN() };
    double unitLength{ std::numeric_limits<double>::quiet_NaN() };
    double unitVelocity{ std::numeric_limits<double>::quiet_NaN() };
    double timeCode{ std::numeric_limits<double>::quiet_NaN() };   ///< the output's time, code units
    QHash<QString, ColumnScale> columns;

    bool has(const QString &column) const { return columns.contains(column); }
    bool hasValue() const { return has(valueColumn()); }
    bool hasLength() const { return has(axisColumn(0)) || has(axisColumn(1)) || has(axisColumn(2)); }
    bool hasTime() const { return has(timeColumn()) && std::isfinite(timeCode); }
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
    u.valueQuantity = text("value_quantity");
    u.unitDensity = number("unit_density", nan);
    u.unitLength = number("unit_length", nan);
    u.unitVelocity = number("unit_velocity", nan);
    u.timeCode = number("time_code", nan);
    const QJsonObject columns = o.value(QLatin1String("columns")).toObject();
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        const double factor = c.value(QLatin1String("factor")).toDouble(0.0);
        if (factor > 0.0)
            u.columns.insert(it.key(), { factor, c.value(QLatin1String("unit")).toString(),
                                         c.value(QLatin1String("source")).toString() });
    }
    return u;
}

/// What a viewer multiplies the column @p name by (a catalogue or particle
/// column, a volume's "value", one of its "x", "y", "z"): its factor when
/// physical units are on and the column has one, else 1 (the file's code units).
inline double columnScale(const PhysicalUnits &u, const QString &name, bool physical)
{
    const auto it = u.columns.constFind(name);
    return physical && it != u.columns.constEnd() ? it->factor : 1.0;
}

/// The unit column @p name is shown in; @p codeUnit (the header's BUNIT or
/// CUNIT, often empty for a simulation) when physical units are off or the
/// column has none.
inline QString columnUnitLabel(const PhysicalUnits &u, const QString &name, bool physical,
                               const QString &codeUnit)
{
    const auto it = u.columns.constFind(name);
    return physical && it != u.columns.constEnd() ? it->unit : codeUnit;
}

/// A colour bar's title for a field: "density (g/cm^3)", or whichever of the two
/// there is. Every field of a view is titled the same way, the window's own
/// included, so a bar never carries just the unit while its neighbours carry
/// names.
inline QString quantityLabel(const QString &name, const QString &unit)
{
    if (!name.isEmpty() && !unit.isEmpty())
        return QStringLiteral("%1 (%2)").arg(name, unit);
    return name.isEmpty() ? unit : name;
}

/// The output's time, "t = 3 (code units)" or, with physical units on and the
/// run's length and velocity known, "t = 9.5e+12 s". Empty when there is none.
inline QString timeLabel(const PhysicalUnits &u, bool physical)
{
    if (!std::isfinite(u.timeCode))
        return QString();
    if (physical && u.hasTime()) {
        const ColumnScale &t = u.columns[timeColumn()];
        return QStringLiteral("t = %1 %2").arg(formatNumber(u.timeCode * t.factor), t.unit);
    }
    return QStringLiteral("t = %1 (code units)").arg(formatNumber(u.timeCode));
}

/// The body of POST /v1/datasets/units: the run's code units (PLUTO) and a
/// factor and unit per column. NaN or non-positive entries are not sent;
/// @p clear forgets everything typed first.
struct UnitsRequest
{
    double unitDensity{ std::numeric_limits<double>::quiet_NaN() };
    double unitLength{ std::numeric_limits<double>::quiet_NaN() };
    double unitVelocity{ std::numeric_limits<double>::quiet_NaN() };
    QHash<QString, ColumnScale> columns;
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
    QJsonObject columns;
    for (auto it = r.columns.begin(); it != r.columns.end(); ++it)
        if (std::isfinite(it->factor) && it->factor > 0.0)
            columns.insert(it.key(), QJsonObject{ { QStringLiteral("factor"), it->factor },
                                                  { QStringLiteral("unit"), it->unit } });
    if (!columns.isEmpty())
        body.insert(QStringLiteral("columns"), columns);
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

}   // namespace visivo::units

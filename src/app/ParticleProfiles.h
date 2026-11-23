#ifndef ParticleProfiles_h
#define ParticleProfiles_h

// ParticleProfiles.h
// Profiles (spherical, cylindrical, along an axis), phase diagrams and
// physical totals of a particle selection (issue #426), as POST
// /v1/catalogue/profile, /v1/catalogue/phase_diagram and /v1/catalogue/totals
// (and their task routes) take and answer them: the request fields, the parsed answers,
// and the text the dialog shows and saves. The filters and dataset come from
// the tabular query body BackendClient builds; these functions add the rest.
// Header-only and free of widgets so the unit tests reach it.

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace visivo::profiles {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Where the profile, or a phase diagram's radius, is measured from.
enum class CentreMode { ShrinkingSphere, Point, Target };

struct CentreChoice
{
    CentreMode mode{ CentreMode::ShrinkingSphere };
    std::array<double, 3> point{ kNaN, kNaN, kNaN };
    /// A row of another open catalogue (a SubFind group or subhalo).
    QString targetDatasetId;
    qint64 targetRow{ -1 };
    /// How the dialog names it: "subhalo 12 of fof_subhalo_tab_099".
    QString label;

    bool usable() const
    {
        switch (mode) {
        case CentreMode::ShrinkingSphere:
            return true;
        case CentreMode::Point:
            return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]);
        case CentreMode::Target:
            return !targetDatasetId.isEmpty() && targetRow >= 0;
        }
        return false;
    }
};

inline QJsonObject centreJson(const CentreChoice &centre)
{
    switch (centre.mode) {
    case CentreMode::Point:
        return QJsonObject{ { QStringLiteral("mode"), QStringLiteral("point") },
                            { QStringLiteral("point"),
                              QJsonArray{ centre.point[0], centre.point[1], centre.point[2] } } };
    case CentreMode::Target:
        return QJsonObject{ { QStringLiteral("mode"), QStringLiteral("target") },
                            { QStringLiteral("target_dataset_id"), centre.targetDatasetId },
                            { QStringLiteral("target_row"), centre.targetRow } };
    case CentreMode::ShrinkingSphere:
        break;
    }
    return QJsonObject{ { QStringLiteral("mode"), QStringLiteral("shrinking_sphere") } };
}

// ── Profile ──────────────────────────────────────────────────────────────────

/// Spherical shells in r, cylindrical annuli in R (the distance from the
/// axis) or slabs in z (the signed height along it).
enum class Geometry { Spherical, Cylindrical, Axial };

inline QString geometryName(Geometry g)
{
    switch (g) {
    case Geometry::Cylindrical:
        return QStringLiteral("cylindrical");
    case Geometry::Axial:
        return QStringLiteral("axial");
    case Geometry::Spherical:
        break;
    }
    return QStringLiteral("spherical");
}

inline Geometry geometryFrom(const QString &name)
{
    if (name == QLatin1String("cylindrical"))
        return Geometry::Cylindrical;
    if (name == QLatin1String("axial"))
        return Geometry::Axial;
    return Geometry::Spherical;
}

struct ProfileRequest
{
    QString datasetId;
    CentreChoice centre;
    double rMin{ kNaN };   ///< NaN: the backend's default
    double rMax{ kNaN };
    int bins{ 32 };
    /// Profiled per shell besides the density: mass-weighted mean, σ and
    /// 16/50/84 percentiles of each.
    QStringList columns;
    /// The polar axis of the polar and azimuthal velocity components; none:
    /// the angular momentum of the central particles, as the backend picks.
    std::optional<std::array<double, 3>> axis;
    Geometry geometry{ Geometry::Spherical };
    /// Cylindrical: the slab z_min <= z < z_max (both or neither); axial: the
    /// range of the slabs (NaN: the backend's default).
    double zMin{ kNaN };
    double zMax{ kNaN };
};

/// Adds the profile's own fields to @p body, which already carries the
/// dataset and the filters.
inline void addProfileFields(QJsonObject &body, const ProfileRequest &req)
{
    body.remove(QStringLiteral("limit"));
    body.remove(QStringLiteral("offset"));
    body.insert(QStringLiteral("centre"), centreJson(req.centre));
    body.insert(QStringLiteral("bins"), std::clamp(req.bins, 4, 256));
    body.insert(QStringLiteral("columns"), QJsonArray::fromStringList(req.columns));
    // An axial profile has no inner radius: the backend refuses one.
    if (std::isfinite(req.rMin) && req.rMin > 0.0 && req.geometry != Geometry::Axial)
        body.insert(QStringLiteral("r_min"), req.rMin);
    if (std::isfinite(req.rMax) && req.rMax > 0.0)
        body.insert(QStringLiteral("r_max"), req.rMax);
    if (req.axis)
        body.insert(QStringLiteral("axis"),
                    QJsonArray{ (*req.axis)[0], (*req.axis)[1], (*req.axis)[2] });
    if (req.geometry != Geometry::Spherical) {
        body.insert(QStringLiteral("geometry"), geometryName(req.geometry));
        // The slab is both bounds or none.
        if (std::isfinite(req.zMin) && std::isfinite(req.zMax) && req.zMin < req.zMax) {
            body.insert(QStringLiteral("z_min"), req.zMin);
            body.insert(QStringLiteral("z_max"), req.zMax);
        }
    }
}

struct ColumnProfile
{
    QString name;
    QVector<double> mean, std, p16, median, p84;
};

struct VelocityProfile
{
    bool present{ false };
    std::array<double, 3> centreVelocity{ kNaN, kNaN, kNaN };
    /// The polar axis the components were taken about.
    std::array<double, 3> axis{ kNaN, kNaN, kNaN };
    QString centreVelocityFrom;
    QVector<double> meanRadial, meanAzimuthal, sigmaR, sigmaTheta, sigmaPhi, sigma1d, beta;
    /// Cylindrical and axial profiles: the components about the axis (v_R,
    /// v_φ, v_z) instead of the spherical ones.
    bool cylindricalComponents{ false };
    QVector<double> meanCylindricalRadial, meanVertical, sigmaCylindricalR, sigmaZ;
};

struct ProfileResult
{
    bool valid{ false };
    bool cancelled{ false };
    QString error;
    Geometry geometry{ Geometry::Spherical };
    /// The bins: r (spherical), R (cylindrical) or z (axial).
    QVector<double> rLo, rHi, rMid, rMean;
    QVector<bool> complete;
    /// `density` is empty for a cylinder without a slab and an axial profile
    /// without a radius limit (no volume to divide by).
    QVector<double> count, mass, density, enclosedMass;
    QVector<double> surfaceDensity;   ///< cylindrical
    QVector<double> linearDensity;    ///< axial
    /// The range of the bins (r, R or z).
    double rMin{ kNaN }, rMax{ kNaN }, completeRadius{ kNaN };
    qint64 selectedRows{ 0 };
    /// Below the first bin and past the last (inside r_min / below z_min,
    /// outside r_max / above z_max), in no bin.
    qint64 insideRMinCount{ 0 }, outsideRMaxCount{ 0 };
    /// Outside the slab (cylindrical) or the radius limit (axial), in no bin.
    qint64 outsideRegionCount{ 0 };
    /// Cylindrical: the slab, NaN without one; axial: the radius limit in
    /// zMin's place, NaN without one.
    double zMin{ kNaN }, zMax{ kNaN }, rLimit{ kNaN };
    /// Cylindrical and axial: the axis and where it comes from.
    std::array<double, 3> axisVector{ kNaN, kNaN, kNaN };
    QString axisFrom;
    QVector<ColumnProfile> columns;
    VelocityProfile velocity;
    std::array<double, 3> centre{ kNaN, kNaN, kNaN };
    QString centreMode;
    QJsonObject centreObject, units, provenance;
};

namespace detail {

/// A JSON array of numbers with nulls (empty shells) as NaN.
inline QVector<double> numbers(const QJsonValue &value)
{
    QVector<double> out;
    const QJsonArray array = value.toArray();
    out.reserve(array.size());
    for (const QJsonValue &v : array)
        out.append(v.isDouble() ? v.toDouble() : kNaN);
    return out;
}

inline double number(const QJsonValue &value)
{
    return value.isDouble() ? value.toDouble() : kNaN;
}

inline std::array<double, 3> triple(const QJsonValue &value)
{
    const QVector<double> v = numbers(value);
    if (v.size() != 3)
        return { kNaN, kNaN, kNaN };
    return { v[0], v[1], v[2] };
}

inline QString text(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isDouble())
        return QString::number(value.toDouble(), 'g', 17);
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return QString();
}

}   // namespace detail

inline ProfileResult parseProfile(const QJsonObject &object)
{
    ProfileResult out;
    out.valid = object.value(QStringLiteral("valid")).toBool(false);
    out.error = object.value(QStringLiteral("error")).toString();
    const QJsonObject p = object.value(QStringLiteral("profile")).toObject();
    out.geometry = geometryFrom(p.value(QStringLiteral("geometry")).toString());
    const bool axial = out.geometry == Geometry::Axial;
    const auto key = [axial](const char *suffix) {
        return QLatin1String(axial ? "z_" : "r_") + QLatin1String(suffix);
    };
    out.rLo = detail::numbers(p.value(key("lo")));
    out.rHi = detail::numbers(p.value(key("hi")));
    out.rMid = detail::numbers(p.value(key("mid")));
    out.rMean = detail::numbers(p.value(key("mean")));
    out.surfaceDensity = detail::numbers(p.value(QStringLiteral("surface_density")));
    out.linearDensity = detail::numbers(p.value(QStringLiteral("linear_density")));
    for (const QJsonValue &v : p.value(QStringLiteral("complete")).toArray())
        out.complete.append(v.toBool(false));
    out.count = detail::numbers(p.value(QStringLiteral("count")));
    out.mass = detail::numbers(p.value(QStringLiteral("mass")));
    out.density = detail::numbers(p.value(QStringLiteral("density")));
    out.enclosedMass = detail::numbers(p.value(QStringLiteral("enclosed_mass")));
    out.rMin = detail::number(p.value(key("min")));
    out.rMax = detail::number(p.value(key("max")));
    out.completeRadius = detail::number(p.value(QStringLiteral("complete_radius")));
    out.selectedRows = static_cast<qint64>(p.value(QStringLiteral("selected_rows")).toDouble(0));
    const auto countOf = [&p](const char *name) {
        return static_cast<qint64>(p.value(QLatin1String(name)).toObject()
                                           .value(QStringLiteral("count")).toDouble(0));
    };
    out.insideRMinCount = countOf(axial ? "below_z_min" : "inside_r_min");
    out.outsideRMaxCount = countOf(axial ? "above_z_max" : "outside_r_max");
    out.outsideRegionCount = countOf(axial ? "outside_r_limit" : "outside_z_range");
    if (axial) {
        out.rLimit = detail::number(p.value(QStringLiteral("r_limit")));
    } else {
        const QVector<double> slab = detail::numbers(p.value(QStringLiteral("z_range")));
        if (slab.size() == 2) {
            out.zMin = slab[0];
            out.zMax = slab[1];
        }
    }
    const QJsonObject axis = p.value(QStringLiteral("axis")).toObject();
    out.axisVector = detail::triple(axis.value(QStringLiteral("vector")));
    out.axisFrom = detail::text(axis.value(QStringLiteral("from")));
    const QJsonObject columns = p.value(QStringLiteral("columns")).toObject();
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const QJsonObject c = it.value().toObject();
        ColumnProfile column;
        column.name = it.key();
        column.mean = detail::numbers(c.value(QStringLiteral("mean")));
        column.std = detail::numbers(c.value(QStringLiteral("std")));
        column.p16 = detail::numbers(c.value(QStringLiteral("p16")));
        column.median = detail::numbers(c.value(QStringLiteral("median")));
        column.p84 = detail::numbers(c.value(QStringLiteral("p84")));
        out.columns.append(column);
    }
    const QJsonValue velocity = p.value(QStringLiteral("velocity"));
    if (velocity.isObject()) {
        const QJsonObject v = velocity.toObject();
        VelocityProfile &vp = out.velocity;
        vp.present = true;
        vp.centreVelocity = detail::triple(v.value(QStringLiteral("centre_velocity")));
        vp.axis = detail::triple(v.value(QStringLiteral("axis")));
        vp.centreVelocityFrom = detail::text(v.value(QStringLiteral("centre_velocity_from")));
        vp.meanRadial = detail::numbers(v.value(QStringLiteral("mean_radial")));
        vp.meanAzimuthal = detail::numbers(v.value(QStringLiteral("mean_azimuthal")));
        vp.sigmaR = detail::numbers(v.value(QStringLiteral("sigma_r")));
        vp.sigmaTheta = detail::numbers(v.value(QStringLiteral("sigma_theta")));
        vp.sigmaPhi = detail::numbers(v.value(QStringLiteral("sigma_phi")));
        vp.sigma1d = detail::numbers(v.value(QStringLiteral("sigma_1d")));
        vp.beta = detail::numbers(v.value(QStringLiteral("beta")));
        vp.cylindricalComponents = v.contains(QStringLiteral("sigma_R"));
        vp.meanCylindricalRadial = detail::numbers(v.value(QStringLiteral("mean_cylindrical_radial")));
        vp.meanVertical = detail::numbers(v.value(QStringLiteral("mean_vertical")));
        vp.sigmaCylindricalR = detail::numbers(v.value(QStringLiteral("sigma_R")));
        vp.sigmaZ = detail::numbers(v.value(QStringLiteral("sigma_z")));
    }
    out.centreObject = object.value(QStringLiteral("centre")).toObject();
    out.centre = detail::triple(out.centreObject.value(QStringLiteral("centre")));
    out.centreMode = out.centreObject.value(QStringLiteral("mode")).toString();
    out.units = object.value(QStringLiteral("units")).toObject();
    out.provenance = object.value(QStringLiteral("provenance")).toObject();
    // A valid answer whose arrays disagree would be plotted shell against
    // the wrong shell: refused here instead.
    if (out.valid) {
        const qsizetype n = out.rMid.size();
        // A density without a volume to divide by is null (empty) for the
        // cylinder and the axis; a spherical profile always has one.
        const auto fits = [n](const QVector<double> &v, bool required) {
            return v.size() == n || (!required && v.isEmpty());
        };
        const bool consistent = n > 0 && out.rLo.size() == n && out.rHi.size() == n
                && fits(out.density, out.geometry == Geometry::Spherical)
                && out.count.size() == n && out.enclosedMass.size() == n
                && fits(out.surfaceDensity, out.geometry == Geometry::Cylindrical)
                && fits(out.linearDensity, out.geometry == Geometry::Axial);
        if (!consistent) {
            out.valid = false;
            out.error = QStringLiteral("The profile answer has shells of different lengths.");
        }
    }
    return out;
}

/// The unit of @p key ("radius", "mass", "density", "velocity") or of column
/// @p key under units.columns, as the table states it; empty when none.
inline QString unitOf(const QJsonObject &units, const QString &key)
{
    const QJsonValue direct = units.value(key);
    if (direct.isString())
        return direct.toString();
    return units.value(QStringLiteral("columns")).toObject().value(key).toString();
}

/// "radius [kpc/h]", or the name alone without a unit.
inline QString axisTitle(const QString &name, const QString &unit)
{
    const QString u = unit.trimmed();
    return u.isEmpty() ? name : QStringLiteral("%1 [%2]").arg(name, u);
}

inline QString csvNumber(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 17) : QString();
}

/// Provenance as comment lines, the way the histogram's CSV starts.
inline QString provenanceComments(const QJsonObject &provenance, const QJsonObject &centre,
                                  const QJsonObject &units)
{
    QString out;
    const auto line = [&out](const QString &key, const QString &value) {
        QString v = value;
        v.replace(QLatin1Char('\n'), QLatin1Char(' '));
        out += QStringLiteral("# %1,%2\n").arg(key, v);
    };
    for (auto it = provenance.constBegin(); it != provenance.constEnd(); ++it)
        line(it.key(), detail::text(it.value()));
    line(QStringLiteral("centre"), detail::text(QJsonValue(centre)));
    line(QStringLiteral("units"), detail::text(QJsonValue(units)));
    return out;
}

/// "(0.12, -0.05, 0.99)"; empty for an axis with no finite component.
inline QString vectorText(const std::array<double, 3> &v)
{
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
        return QString();
    return QStringLiteral("(%1, %2, %3)")
            .arg(v[0], 0, 'g', 4)
            .arg(v[1], 0, 'g', 4)
            .arg(v[2], 0, 'g', 4);
}

/// The length unit of the bins: the height along the axis for an axial
/// profile, the radius otherwise.
inline QString binUnit(const ProfileResult &r)
{
    if (r.geometry == Geometry::Axial) {
        const QString height = unitOf(r.units, QStringLiteral("height"));
        if (!height.isEmpty())
            return height;
    }
    return unitOf(r.units, QStringLiteral("radius"));
}

inline QString profileCsv(const ProfileResult &r)
{
    QString out = provenanceComments(r.provenance, r.centreObject, r.units);
    out += QStringLiteral("# geometry,%1\n").arg(geometryName(r.geometry));
    if (r.geometry != Geometry::Spherical) {
        out += QStringLiteral("# axis,%1\n").arg(vectorText(r.axisVector).replace(',', ';'));
        out += QStringLiteral("# axis_from,%1\n").arg(QString(r.axisFrom).replace(',', ';'));
        if (r.geometry == Geometry::Cylindrical && std::isfinite(r.zMin))
            out += QStringLiteral("# slab_z,%1;%2\n").arg(csvNumber(r.zMin), csvNumber(r.zMax));
        if (r.geometry == Geometry::Axial && std::isfinite(r.rLimit))
            out += QStringLiteral("# r_limit,%1\n").arg(csvNumber(r.rLimit));
    }
    const VelocityProfile &v = r.velocity;
    if (v.present) {
        out += QStringLiteral("# velocity_axis,%1\n").arg(vectorText(v.axis).replace(',', ';'));
        out += QStringLiteral("# centre_velocity,%1\n")
                       .arg(vectorText(v.centreVelocity).replace(',', ';'));
        out += QStringLiteral("# velocities,peculiar (without the Hubble flow) relative to "
                              "the centre velocity\n");
    }
    const bool axial = r.geometry == Geometry::Axial;
    const QString b = axial ? QStringLiteral("z") : QStringLiteral("r");
    QStringList header{ b + QStringLiteral("_lo"), b + QStringLiteral("_hi"),
                        b + QStringLiteral("_mid"), b + QStringLiteral("_mean"),
                        QStringLiteral("complete"), QStringLiteral("count"),
                        QStringLiteral("mass"), QStringLiteral("density"),
                        QStringLiteral("enclosed_mass") };
    if (r.geometry == Geometry::Cylindrical)
        header << QStringLiteral("surface_density");
    if (axial)
        header << QStringLiteral("linear_density");
    for (const ColumnProfile &c : r.columns)
        for (const char *stat : { "mean", "std", "p16", "median", "p84" })
            header << QStringLiteral("%1_%2").arg(c.name, QLatin1String(stat));
    if (v.present && v.cylindricalComponents)
        header << QStringLiteral("mean_cylindrical_radial") << QStringLiteral("mean_azimuthal")
               << QStringLiteral("mean_vertical") << QStringLiteral("sigma_R")
               << QStringLiteral("sigma_phi") << QStringLiteral("sigma_z")
               << QStringLiteral("sigma_1d");
    else if (v.present)
        header << QStringLiteral("mean_radial") << QStringLiteral("mean_azimuthal")
               << QStringLiteral("sigma_r") << QStringLiteral("sigma_theta")
               << QStringLiteral("sigma_phi") << QStringLiteral("sigma_1d")
               << QStringLiteral("beta");
    out += header.join(QLatin1Char(',')) + QLatin1Char('\n');
    const auto at = [](const QVector<double> &values, qsizetype i) {
        return i < values.size() ? csvNumber(values[i]) : QString();
    };
    for (qsizetype i = 0; i < r.rMid.size(); ++i) {
        QStringList row{ at(r.rLo, i), at(r.rHi, i), at(r.rMid, i), at(r.rMean, i),
                         i < r.complete.size() && r.complete[i] ? QStringLiteral("1")
                                                                 : QStringLiteral("0"),
                         at(r.count, i), at(r.mass, i), at(r.density, i),
                         at(r.enclosedMass, i) };
        if (r.geometry == Geometry::Cylindrical)
            row << at(r.surfaceDensity, i);
        if (axial)
            row << at(r.linearDensity, i);
        for (const ColumnProfile &c : r.columns)
            row << at(c.mean, i) << at(c.std, i) << at(c.p16, i) << at(c.median, i)
                << at(c.p84, i);
        if (v.present && v.cylindricalComponents)
            row << at(v.meanCylindricalRadial, i) << at(v.meanAzimuthal, i)
                << at(v.meanVertical, i) << at(v.sigmaCylindricalR, i) << at(v.sigmaPhi, i)
                << at(v.sigmaZ, i) << at(v.sigma1d, i);
        else if (v.present)
            row << at(v.meanRadial, i) << at(v.meanAzimuthal, i) << at(v.sigmaR, i)
                << at(v.sigmaTheta, i) << at(v.sigmaPhi, i) << at(v.sigma1d, i)
                << at(v.beta, i);
        out += row.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    return out;
}

// ── Phase diagram ────────────────────────────────────────────────────────────

struct PhaseAxis
{
    QString field;
    bool log{ false };
    int bins{ 64 };
    double min{ kNaN };   ///< both or neither: the backend refuses one alone
    double max{ kNaN };
};

struct PhaseRequest
{
    QString datasetId;
    PhaseAxis x, y;
    QString weightField;   ///< empty: counts
    std::optional<CentreChoice> centre;
};

/// "radius" and "radial_velocity" are measured from a centre.
inline bool needsCentre(const QString &field)
{
    return field == QLatin1String("radius") || field == QLatin1String("radial_velocity");
}

inline QJsonObject axisJson(const PhaseAxis &axis)
{
    QJsonObject o{ { QStringLiteral("field"), axis.field },
                   { QStringLiteral("scale"),
                     axis.log ? QStringLiteral("log") : QStringLiteral("linear") },
                   { QStringLiteral("bins"), std::clamp(axis.bins, 4, 512) } };
    if (std::isfinite(axis.min) && std::isfinite(axis.max) && axis.min < axis.max) {
        o.insert(QStringLiteral("min"), axis.min);
        o.insert(QStringLiteral("max"), axis.max);
    }
    return o;
}

inline void addPhaseFields(QJsonObject &body, const PhaseRequest &req)
{
    body.remove(QStringLiteral("limit"));
    body.remove(QStringLiteral("offset"));
    body.insert(QStringLiteral("x"), axisJson(req.x));
    body.insert(QStringLiteral("y"), axisJson(req.y));
    body.insert(QStringLiteral("weight_field"), req.weightField);
    if (req.centre && (needsCentre(req.x.field) || needsCentre(req.y.field)))
        body.insert(QStringLiteral("centre"), centreJson(*req.centre));
}

struct PhaseResult
{
    bool valid{ false };
    bool cancelled{ false };
    QString error;
    QVector<double> xEdges, yEdges;
    /// [x][y], as the backend indexes them.
    QVector<QVector<double>> counts, weights;
    qint64 selectedRows{ 0 }, binnedRows{ 0 }, outsideRows{ 0 }, droppedRows{ 0 };
    double selectedWeight{ 0.0 }, binnedWeight{ 0.0 }, outsideWeight{ 0.0 },
            droppedWeight{ 0.0 };
    qint64 invalidWeightRows{ 0 };
    QString xUnit, yUnit, weightUnit;
    QJsonObject centreObject, units, provenance;
};

inline PhaseResult parsePhase(const QJsonObject &object)
{
    PhaseResult out;
    out.valid = object.value(QStringLiteral("valid")).toBool(false);
    out.error = object.value(QStringLiteral("error")).toString();
    const QJsonObject d = object.value(QStringLiteral("diagram")).toObject();
    out.xEdges = detail::numbers(d.value(QStringLiteral("x_edges")));
    out.yEdges = detail::numbers(d.value(QStringLiteral("y_edges")));
    for (const QJsonValue &row : d.value(QStringLiteral("counts")).toArray())
        out.counts.append(detail::numbers(row));
    for (const QJsonValue &row : d.value(QStringLiteral("weights")).toArray())
        out.weights.append(detail::numbers(row));
    const auto rows = [&d](const char *key) {
        return static_cast<qint64>(d.value(QLatin1String(key)).toDouble(0));
    };
    const auto weight = [&d](const char *key) { return d.value(QLatin1String(key)).toDouble(0); };
    out.selectedRows = rows("selected_rows");
    out.binnedRows = rows("binned_rows");
    out.outsideRows = rows("outside_rows");
    out.droppedRows = rows("dropped_rows");
    out.invalidWeightRows = rows("invalid_weight_rows");
    out.selectedWeight = weight("selected_weight");
    out.binnedWeight = weight("binned_weight");
    out.outsideWeight = weight("outside_weight");
    out.droppedWeight = weight("dropped_weight");
    out.centreObject = object.value(QStringLiteral("centre")).toObject();
    out.units = object.value(QStringLiteral("units")).toObject();
    out.xUnit = out.units.value(QStringLiteral("x")).toString();
    out.yUnit = out.units.value(QStringLiteral("y")).toString();
    out.weightUnit = out.units.value(QStringLiteral("weight")).toString();
    out.provenance = object.value(QStringLiteral("provenance")).toObject();
    if (out.valid) {
        const qsizetype nx = out.xEdges.size() - 1;
        const qsizetype ny = out.yEdges.size() - 1;
        bool consistent = nx > 0 && ny > 0 && out.counts.size() == nx
                && out.weights.size() == nx;
        for (qsizetype i = 0; consistent && i < nx; ++i)
            consistent = out.counts[i].size() == ny && out.weights[i].size() == ny;
        if (!consistent) {
            out.valid = false;
            out.error = QStringLiteral("The phase diagram's bins do not match its edges.");
        }
    }
    return out;
}

/// "1,234,567 selected: 1,200,000 binned, 30,000 outside the range,
/// 4,567 dropped (non-finite or ≤ 0 on a log axis)", plus the weights and
/// the rows with an invalid weight when weighted.
inline QString closureText(const PhaseResult &r, bool weighted)
{
    QString text = QStringLiteral("%L1 selected rows: %L2 binned, %L3 outside the range, "
                                  "%L4 dropped (non-finite, or ≤ 0 on a log axis)")
                           .arg(r.selectedRows)
                           .arg(r.binnedRows)
                           .arg(r.outsideRows)
                           .arg(r.droppedRows);
    if (weighted) {
        text += QStringLiteral(". Weight %1: %2 binned, %3 outside, %4 dropped")
                        .arg(r.selectedWeight, 0, 'g', 6)
                        .arg(r.binnedWeight, 0, 'g', 6)
                        .arg(r.outsideWeight, 0, 'g', 6)
                        .arg(r.droppedWeight, 0, 'g', 6);
        if (!r.weightUnit.isEmpty())
            text += QStringLiteral(" [%1]").arg(r.weightUnit);
    }
    if (r.invalidWeightRows > 0)
        text += QStringLiteral(". %L1 rows have an invalid weight and carry none")
                        .arg(r.invalidWeightRows);
    return text + QLatin1Char('.');
}

/// The rows the totals do not account for: 0 when the answer closes. Rows
/// with an invalid weight are counted apart from the dropped ones.
inline qint64 closureGap(const PhaseResult &r)
{
    return r.selectedRows - r.binnedRows - r.outsideRows - r.droppedRows
            - r.invalidWeightRows;
}

inline QString phaseCsv(const PhaseResult &r, bool weighted)
{
    QString out = provenanceComments(r.provenance, r.centreObject, r.units);
    out += QStringLiteral("# closure,%1\n").arg(closureText(r, weighted).replace(',', ';'));
    out += QStringLiteral("x_lo,x_hi,y_lo,y_hi,count,weight\n");
    for (qsizetype i = 0; i + 1 < r.xEdges.size() && i < r.counts.size(); ++i)
        for (qsizetype j = 0; j + 1 < r.yEdges.size() && j < r.counts[i].size(); ++j)
            out += QStringList{ csvNumber(r.xEdges[i]), csvNumber(r.xEdges[i + 1]),
                                csvNumber(r.yEdges[j]), csvNumber(r.yEdges[j + 1]),
                                csvNumber(r.counts[i][j]),
                                csvNumber(j < r.weights.value(i).size() ? r.weights[i][j]
                                                                        : kNaN) }
                           .join(QLatin1Char(','))
                    + QLatin1Char('\n');
    return out;
}

// ── Physical totals ──────────────────────────────────────────────────────────

/// One total: `value` in physical units (empty when it cannot be given, with
/// `reason`), and as stored in the table's own units when the field exists.
/// A scalar has one component, a vector (velocity, momentum) three.
struct TotalsQuantity
{
    QString name;
    QVector<double> value;
    QString unit;
    bool hasStored{ false };
    QVector<double> storedValue;
    QString storedUnit;
    QString reason;
};

struct TotalsResult
{
    bool valid{ false };
    bool cancelled{ false };
    QString error;
    qint64 count{ 0 };
    qint64 selectedRows{ 0 }, withMass{ 0 }, withVelocity{ 0 }, withEnergy{ 0 };
    double massWithVelocity{ kNaN };
    QVector<TotalsQuantity> quantities;
    QJsonObject provenance;
};

/// The order the dialog lists them in (the backend's names).
inline const QStringList &totalsOrder()
{
    static const QStringList order{
        QStringLiteral("mass"), QStringLiteral("centre_of_mass_velocity"),
        QStringLiteral("momentum"), QStringLiteral("momentum_magnitude"),
        QStringLiteral("kinetic_energy"), QStringLiteral("kinetic_energy_com_frame"),
        QStringLiteral("velocity_dispersion_1d"), QStringLiteral("thermal_energy") };
    return order;
}

/// "Kinetic energy (centre-of-mass frame)" for kinetic_energy_com_frame.
inline QString totalsLabel(const QString &name)
{
    static const QHash<QString, QString> labels{
        { QStringLiteral("mass"), QStringLiteral("Mass") },
        { QStringLiteral("centre_of_mass_velocity"), QStringLiteral("Centre-of-mass velocity") },
        { QStringLiteral("momentum"), QStringLiteral("Momentum") },
        { QStringLiteral("momentum_magnitude"), QStringLiteral("|Momentum|") },
        { QStringLiteral("kinetic_energy"), QStringLiteral("Kinetic energy") },
        { QStringLiteral("kinetic_energy_com_frame"),
          QStringLiteral("Kinetic energy (centre-of-mass frame)") },
        { QStringLiteral("velocity_dispersion_1d"), QStringLiteral("Velocity dispersion (1-D)") },
        { QStringLiteral("thermal_energy"), QStringLiteral("Thermal energy") } };
    return labels.value(name, name);
}

namespace detail {

/// A number or an array of numbers, as one or three components.
inline QVector<double> components(const QJsonValue &value)
{
    if (value.isArray())
        return numbers(value);
    if (value.isDouble())
        return { value.toDouble() };
    return {};
}

}   // namespace detail

inline TotalsResult parseTotals(const QJsonObject &object)
{
    TotalsResult out;
    out.valid = object.value(QStringLiteral("valid")).toBool(false);
    out.error = object.value(QStringLiteral("error")).toString();
    const QJsonObject t = object.value(QStringLiteral("totals")).toObject();
    out.count = static_cast<qint64>(t.value(QStringLiteral("count")).toDouble(0));
    const QJsonObject rows = t.value(QStringLiteral("rows")).toObject();
    const auto rowsOf = [&rows](const char *name) {
        return static_cast<qint64>(rows.value(QLatin1String(name)).toDouble(0));
    };
    out.selectedRows = rowsOf("selected");
    out.withMass = rowsOf("with_mass");
    out.withVelocity = rowsOf("with_velocity");
    out.withEnergy = rowsOf("with_energy");
    out.massWithVelocity = detail::number(t.value(QStringLiteral("mass_with_velocity")));
    const QJsonObject quantities = t.value(QStringLiteral("quantities")).toObject();
    QStringList names = totalsOrder();
    for (auto it = quantities.constBegin(); it != quantities.constEnd(); ++it)
        if (!names.contains(it.key()))
            names << it.key();   // a total a newer backend adds
    for (const QString &name : names) {
        if (!quantities.contains(name))
            continue;
        const QJsonObject q = quantities.value(name).toObject();
        TotalsQuantity quantity;
        quantity.name = name;
        quantity.value = detail::components(q.value(QStringLiteral("value")));
        quantity.unit = q.value(QStringLiteral("unit")).toString();
        quantity.reason = q.value(QStringLiteral("reason")).toString();
        const QJsonValue stored = q.value(QStringLiteral("stored"));
        if (stored.isObject()) {
            quantity.hasStored = true;
            quantity.storedValue = detail::components(stored.toObject().value(QStringLiteral("value")));
            quantity.storedUnit = stored.toObject().value(QStringLiteral("unit")).toString();
        }
        out.quantities.append(quantity);
    }
    out.provenance = object.value(QStringLiteral("provenance")).toObject();
    return out;
}

/// "1.234e+10" or "(12.3, -4.5, 0.6)"; empty for no value.
inline QString componentsText(const QVector<double> &v, int precision = 6)
{
    QStringList parts;
    for (double x : v)
        parts << (std::isfinite(x) ? QString::number(x, 'g', precision) : QStringLiteral("—"));
    if (parts.size() == 1)
        return parts.front();
    return parts.isEmpty() ? QString()
                           : QStringLiteral("(%1)").arg(parts.join(QStringLiteral(", ")));
}

inline QString totalsCsv(const TotalsResult &r)
{
    QString out = provenanceComments(r.provenance, QJsonObject(), QJsonObject());
    out += QStringLiteral("# rows,selected %1; with_mass %2; with_velocity %3; with_energy %4\n")
                   .arg(r.selectedRows)
                   .arg(r.withMass)
                   .arg(r.withVelocity)
                   .arg(r.withEnergy);
    out += QStringLiteral("quantity,component,value,unit,stored_value,stored_unit,reason\n");
    for (const TotalsQuantity &q : r.quantities) {
        const qsizetype n = std::max(q.value.size(), q.storedValue.size());
        for (qsizetype i = 0; i < std::max<qsizetype>(n, 1); ++i) {
            QString reason = q.reason;
            reason.replace(QLatin1Char(','), QLatin1Char(';')).replace(QLatin1Char('\n'), QLatin1Char(' '));
            out += QStringList{ q.name,
                                n > 1 ? QString::number(i) : QString(),
                                i < q.value.size() ? csvNumber(q.value[i]) : QString(),
                                q.unit,
                                i < q.storedValue.size() ? csvNumber(q.storedValue[i]) : QString(),
                                q.storedUnit, reason }
                           .join(QLatin1Char(','))
                    + QLatin1Char('\n');
        }
    }
    return out;
}

}   // namespace visivo::profiles

#endif   // ParticleProfiles_h

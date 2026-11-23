#ifndef ParticleProfiles_h
#define ParticleProfiles_h

// ParticleProfiles.h
// Radial profiles and phase diagrams of a particle selection (issue #426),
// as POST /v1/catalogue/profile and /v1/catalogue/phase_diagram (and their
// task routes) take and answer them: the request fields, the parsed answers,
// and the text the dialog shows and saves. The filters and dataset come from
// the tabular query body BackendClient builds; these functions add the rest.
// Header-only and free of widgets so the unit tests reach it.

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

// ── Radial profile ───────────────────────────────────────────────────────────

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
    if (std::isfinite(req.rMin) && req.rMin > 0.0)
        body.insert(QStringLiteral("r_min"), req.rMin);
    if (std::isfinite(req.rMax) && req.rMax > 0.0)
        body.insert(QStringLiteral("r_max"), req.rMax);
    if (req.axis)
        body.insert(QStringLiteral("axis"),
                    QJsonArray{ (*req.axis)[0], (*req.axis)[1], (*req.axis)[2] });
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
};

struct ProfileResult
{
    bool valid{ false };
    bool cancelled{ false };
    QString error;
    QVector<double> rLo, rHi, rMid, rMean;
    QVector<bool> complete;
    QVector<double> count, mass, density, enclosedMass;
    double rMin{ kNaN }, rMax{ kNaN }, completeRadius{ kNaN };
    qint64 selectedRows{ 0 };
    qint64 insideRMinCount{ 0 }, outsideRMaxCount{ 0 };
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
    out.rLo = detail::numbers(p.value(QStringLiteral("r_lo")));
    out.rHi = detail::numbers(p.value(QStringLiteral("r_hi")));
    out.rMid = detail::numbers(p.value(QStringLiteral("r_mid")));
    out.rMean = detail::numbers(p.value(QStringLiteral("r_mean")));
    for (const QJsonValue &v : p.value(QStringLiteral("complete")).toArray())
        out.complete.append(v.toBool(false));
    out.count = detail::numbers(p.value(QStringLiteral("count")));
    out.mass = detail::numbers(p.value(QStringLiteral("mass")));
    out.density = detail::numbers(p.value(QStringLiteral("density")));
    out.enclosedMass = detail::numbers(p.value(QStringLiteral("enclosed_mass")));
    out.rMin = detail::number(p.value(QStringLiteral("r_min")));
    out.rMax = detail::number(p.value(QStringLiteral("r_max")));
    out.completeRadius = detail::number(p.value(QStringLiteral("complete_radius")));
    out.selectedRows = static_cast<qint64>(p.value(QStringLiteral("selected_rows")).toDouble(0));
    out.insideRMinCount = static_cast<qint64>(
            p.value(QStringLiteral("inside_r_min")).toObject().value(QStringLiteral("count"))
                    .toDouble(0));
    out.outsideRMaxCount = static_cast<qint64>(
            p.value(QStringLiteral("outside_r_max")).toObject().value(QStringLiteral("count"))
                    .toDouble(0));
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
        const bool consistent = n > 0 && out.rLo.size() == n && out.rHi.size() == n
                && out.density.size() == n && out.count.size() == n
                && out.enclosedMass.size() == n;
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

inline QString profileCsv(const ProfileResult &r)
{
    QString out = provenanceComments(r.provenance, r.centreObject, r.units);
    if (r.velocity.present) {
        out += QStringLiteral("# velocity_axis,%1\n").arg(vectorText(r.velocity.axis).replace(',', ';'));
        out += QStringLiteral("# centre_velocity,%1\n")
                       .arg(vectorText(r.velocity.centreVelocity).replace(',', ';'));
        out += QStringLiteral("# velocities,peculiar (without the Hubble flow) relative to "
                              "the centre velocity\n");
    }
    QStringList header{ QStringLiteral("r_lo"), QStringLiteral("r_hi"), QStringLiteral("r_mid"),
                        QStringLiteral("r_mean"), QStringLiteral("complete"),
                        QStringLiteral("count"), QStringLiteral("mass"),
                        QStringLiteral("density"), QStringLiteral("enclosed_mass") };
    for (const ColumnProfile &c : r.columns)
        for (const char *stat : { "mean", "std", "p16", "median", "p84" })
            header << QStringLiteral("%1_%2").arg(c.name, QLatin1String(stat));
    if (r.velocity.present)
        header << QStringLiteral("mean_radial") << QStringLiteral("mean_azimuthal")
               << QStringLiteral("sigma_r") << QStringLiteral("sigma_theta")
               << QStringLiteral("sigma_phi") << QStringLiteral("sigma_1d")
               << QStringLiteral("beta");
    out += header.join(QLatin1Char(',')) + QLatin1Char('\n');
    const auto at = [](const QVector<double> &v, qsizetype i) {
        return i < v.size() ? csvNumber(v[i]) : QString();
    };
    for (qsizetype i = 0; i < r.rMid.size(); ++i) {
        QStringList row{ at(r.rLo, i), at(r.rHi, i), at(r.rMid, i), at(r.rMean, i),
                         i < r.complete.size() && r.complete[i] ? QStringLiteral("1")
                                                                 : QStringLiteral("0"),
                         at(r.count, i), at(r.mass, i), at(r.density, i),
                         at(r.enclosedMass, i) };
        for (const ColumnProfile &c : r.columns)
            row << at(c.mean, i) << at(c.std, i) << at(c.p16, i) << at(c.median, i)
                << at(c.p84, i);
        if (r.velocity.present)
            row << at(r.velocity.meanRadial, i) << at(r.velocity.meanAzimuthal, i)
                << at(r.velocity.sigmaR, i) << at(r.velocity.sigmaTheta, i)
                << at(r.velocity.sigmaPhi, i) << at(r.velocity.sigma1d, i)
                << at(r.velocity.beta, i);
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

}   // namespace visivo::profiles

#endif   // ParticleProfiles_h

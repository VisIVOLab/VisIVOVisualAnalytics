#ifndef COSMOLOGYMODEL_H
#define COSMOLOGYMODEL_H
// CosmologyModel.h
// The cosmology a user picks in the calculator or the 3-D catalogue viewer,
// and the plain logic around it: what the backend accepts, how the choice is
// named on screen, how a list of redshifts is read, and how a result table is
// written out. QtCore only, so tests/test_cosmology_tools.cpp can pin it.
//
// Two kinds of model, because the backend has two kinds of route:
//   * a PRESET (Planck18, Planck15, Planck13, WMAP9) is astropy's own object,
//     radiation and massive neutrinos included. POST /v1/cosmology/distance and
//     /distance/batch take it by name.
//   * a CUSTOM model is astropy's LambdaCDM(H0, Om0, Ode0) with its default
//     Tcmb0 = 0: matter, Λ and curvature, no radiation. Only GET
//     /v1/cosmology/distances takes one.
// The numbers themselves always come from the backend; nothing here
// integrates a Friedmann equation.

#include <QList>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <limits>

namespace visivo::cosmology {

/// The presets the backend loads (app/dependencies.py, _load_cosmology_models),
/// in the order the selectors list them.
inline QStringList presetNames()
{
    return { QStringLiteral("Planck18"), QStringLiteral("Planck15"),
             QStringLiteral("Planck13"), QStringLiteral("WMAP9") };
}

/// The name a custom model travels under inside the client (a combo entry, a
/// product parameter). Never sent to a preset route.
inline QString customName() { return QStringLiteral("Custom"); }

/// The backend's bounds for a custom model (routers/cosmology.py,
/// cosmology_distances). Checked here first so the form can say what is wrong
/// before a request is made; the backend checks again.
inline constexpr double kH0Min = 40.0;
inline constexpr double kH0Max = 120.0;
inline constexpr double kOmegaMax = 1.5;
inline constexpr double kRedshiftMaxCustom = 20.0;
/// Below this |Ωk| the model is called flat: the spin boxes carry four
/// decimals, so anything smaller is rounding, not curvature.
inline constexpr double kFlatTolerance = 5e-5;

struct Model
{
    /// A preset's name, or empty for a custom model.
    QString preset{ QStringLiteral("Planck18") };
    double h0{ 70.0 };     ///< km/s/Mpc
    double om0{ 0.3 };     ///< Ωm today
    double ode0{ 0.7 };    ///< ΩΛ today

    bool isCustom() const { return preset.isEmpty(); }
    /// Ωk = 1 − Ωm − ΩΛ (radiation is absent from a custom model).
    double omegaK() const { return 1.0 - om0 - ode0; }
    bool isFlat() const { return std::abs(omegaK()) < kFlatTolerance; }

    static Model fromPreset(const QString &name)
    {
        Model m;
        m.preset = name;
        return m;
    }
    static Model custom(double h0, double om0, double ode0)
    {
        Model m;
        m.preset.clear();
        m.h0 = h0;
        m.om0 = om0;
        m.ode0 = ode0;
        return m;
    }
    bool operator==(const Model &o) const
    {
        if (isCustom() != o.isCustom())
            return false;
        if (!isCustom())
            return preset == o.preset;
        return h0 == o.h0 && om0 == o.om0 && ode0 == o.ode0;
    }
    bool operator!=(const Model &o) const { return !(*this == o); }
};

/// "open" when Ωk > 0, "closed" when Ωk < 0, "flat" otherwise.
inline QString curvatureWord(const Model &m)
{
    if (m.isFlat())
        return QStringLiteral("flat");
    return m.omegaK() > 0.0 ? QStringLiteral("open") : QStringLiteral("closed");
}

/// Why the backend would refuse this custom model, or empty when it would not.
/// A preset is always accepted here: the backend knows its own names.
inline QString validate(const Model &m)
{
    if (!m.isCustom())
        return {};
    if (!std::isfinite(m.h0) || m.h0 < kH0Min || m.h0 > kH0Max)
        return QStringLiteral("H₀ must be between %1 and %2 km/s/Mpc.")
                .arg(kH0Min).arg(kH0Max);
    if (!std::isfinite(m.om0) || m.om0 < 0.0 || m.om0 > kOmegaMax)
        return QStringLiteral("Ωm must be between 0 and %1.").arg(kOmegaMax);
    if (!std::isfinite(m.ode0) || m.ode0 < 0.0 || m.ode0 > kOmegaMax)
        return QStringLiteral("ΩΛ must be between 0 and %1.").arg(kOmegaMax);
    // Ωm = 0 with ΩΛ = 1 (de Sitter) is accepted: the server answers its
    // infinite age as null and the distances as numbers (#129).
    // The backend refuses Ωm + ΩΛ > 1: closed models are not supported there.
    if (m.om0 + m.ode0 > 1.0 + 1e-6)
        return QStringLiteral("Ωm + ΩΛ = %1 is a closed universe, which the "
                              "server does not support: keep the sum at or below 1.")
                .arg(m.om0 + m.ode0, 0, 'f', 4);
    return {};
}

/// One line naming the model, as the status lines and CSV headers show it.
/// A custom model always states its curvature: a non-flat model must never
/// pass for the flat one people assume.
inline QString describe(const Model &m)
{
    if (!m.isCustom())
        return m.preset;
    QString s = QStringLiteral("Custom ΛCDM (H₀ = %1 km/s/Mpc, Ωm = %2, "
                               "ΩΛ = %3")
                        .arg(m.h0, 0, 'g', 6)
                        .arg(m.om0, 0, 'g', 6)
                        .arg(m.ode0, 0, 'g', 6);
    if (m.isFlat())
        s += QStringLiteral(", flat)");
    else
        s += QStringLiteral(", %1: Ωk = %2)")
                     .arg(curvatureWord(m))
                     .arg(m.omegaK(), 0, 'f', 4);
    return s;
}

/// The same, in ASCII, for files read by scripts (a CSV comment line).
inline QString describeAscii(const Model &m)
{
    if (!m.isCustom())
        return m.preset;
    QString s = QStringLiteral("Custom LambdaCDM (H0=%1 km/s/Mpc, Om0=%2, Ode0=%3, Tcmb0=0")
                        .arg(m.h0, 0, 'g', 6)
                        .arg(m.om0, 0, 'g', 6)
                        .arg(m.ode0, 0, 'g', 6);
    if (m.isFlat())
        s += QStringLiteral(", flat)");
    else
        s += QStringLiteral(", %1, Ok0=%2)").arg(curvatureWord(m)).arg(m.omegaK(), 0, 'f', 4);
    return s;
}

// ── Redshift input ───────────────────────────────────────────────────────────

struct RedshiftParse
{
    QVector<double> redshifts;   ///< in the order typed
    QStringList rejected;        ///< tokens that are not a redshift ≥ 0
};

/// Read one or many redshifts: separated by commas, semicolons, spaces, tabs
/// or new lines, so a column pasted from a spreadsheet works as well as a
/// typed "0.1, 0.5, 1". A token that is not a finite number ≥ 0 is reported,
/// not dropped silently.
inline RedshiftParse parseRedshifts(const QString &text)
{
    RedshiftParse out;
    static const QRegularExpression sep(QStringLiteral("[\\s,;]+"));
    const QStringList tokens = text.split(sep, Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        bool ok = false;
        const double z = token.toDouble(&ok);
        if (ok && std::isfinite(z) && z >= 0.0)
            out.redshifts.append(z);
        else
            out.rejected.append(token);
    }
    return out;
}

// ── Results ──────────────────────────────────────────────────────────────────

inline double notProvided() { return std::numeric_limits<double>::quiet_NaN(); }

/// One redshift's answer. NaN marks a quantity the backend did not provide for
/// this model (see the calculator's column notes), never a computed zero.
struct Row
{
    double z{ 0.0 };
    double comovingMpc{ notProvided() };
    double luminosityMpc{ notProvided() };
    double angularDiameterMpc{ notProvided() };
    double lookbackGyr{ notProvided() };
    double ageGyr{ notProvided() };
    double kpcPerArcsec{ notProvided() };
    QString error;   ///< the backend's reason, when this redshift was refused
};

/// Proper transverse kpc per arcsecond from the angular-diameter distance:
/// D_A [kpc] × (1″ in radians). A change of units of the backend's D_A, and the
/// same expression astropy's kpc_proper_per_arcmin evaluates. Used only where
/// the backend returns D_A but not the scale (a custom model).
inline double kpcPerArcsecFromAngularDiameter(double dAMpc)
{
    if (!std::isfinite(dAMpc))
        return notProvided();
    constexpr double kArcsecInRadians = 3.14159265358979323846 / (180.0 * 3600.0);
    return dAMpc * 1000.0 * kArcsecInRadians;
}

struct Column
{
    QString header;     ///< with its unit, for the table and the file
    double Row::*field;
    int decimals;
};

inline QList<Column> columns()
{
    return {
        { QStringLiteral("D_C comoving (Mpc)"), &Row::comovingMpc, 3 },
        { QStringLiteral("D_L luminosity (Mpc)"), &Row::luminosityMpc, 3 },
        { QStringLiteral("D_A angular diameter (Mpc)"), &Row::angularDiameterMpc, 3 },
        { QStringLiteral("Lookback time (Gyr)"), &Row::lookbackGyr, 4 },
        { QStringLiteral("Age of universe at z (Gyr)"), &Row::ageGyr, 4 },
        // Proper, not comoving: D_A per arcsecond.
        { QStringLiteral("Proper scale (kpc/arcsec)"), &Row::kpcPerArcsec, 4 },
    };
}

/// A cell as shown and exported: fixed decimals, empty when not provided.
inline QString formatCell(double value, int decimals)
{
    return std::isfinite(value) ? QString::number(value, 'f', decimals) : QString();
}

/// The result table as delimited text: CSV for the file (separator ','), TSV
/// for the clipboard so a paste into a spreadsheet lands in columns. The first
/// line names the model, so a file read later says which universe it is in.
inline QString formatTable(const Model &model, const QList<Row> &rows, QChar separator)
{
    const auto quote = [separator](const QString &s) {
        if (!s.contains(separator) && !s.contains(u'"') && !s.contains(u'\n'))
            return s;
        QString q = s;
        q.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        return QStringLiteral("\"%1\"").arg(q);
    };
    QString out = QStringLiteral("# cosmology: %1\n").arg(describeAscii(model));
    QStringList header{ QStringLiteral("z") };
    for (const Column &c : columns())
        header.append(quote(c.header));
    header.append(QStringLiteral("note"));
    out += header.join(separator) + u'\n';
    for (const Row &r : rows) {
        QStringList cells{ QString::number(r.z, 'g', 10) };
        for (const Column &c : columns())
            cells.append(formatCell(r.*(c.field), c.decimals));
        cells.append(quote(r.error));
        out += cells.join(separator) + u'\n';
    }
    return out;
}

// ── The custom model the user last defined ───────────────────────────────────
// Shared between the calculator and every catalogue viewer through QSettings,
// so "Custom" in a catalogue means the model the user set up in the calculator.

inline QString settingsGroup() { return QStringLiteral("cosmology"); }

inline Model loadCustom(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    // Default: the round flat model people quote (H0 = 70, Ωm = 0.3).
    const Model m = Model::custom(settings.value(QStringLiteral("customH0"), 70.0).toDouble(),
                                  settings.value(QStringLiteral("customOm0"), 0.3).toDouble(),
                                  settings.value(QStringLiteral("customOde0"), 0.7).toDouble());
    settings.endGroup();
    return m;
}

inline void saveCustom(QSettings &settings, const Model &m)
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("customH0"), m.h0);
    settings.setValue(QStringLiteral("customOm0"), m.om0);
    settings.setValue(QStringLiteral("customOde0"), m.ode0);
    settings.endGroup();
}

}   // namespace visivo::cosmology

#endif   // COSMOLOGYMODEL_H

#ifndef LteCoRequest_h
#define LteCoRequest_h

// LteCoRequest.h
// The LTE analysis of CO isotopologues (POST /v1/products/lte_co, issue #416):
// the request the dialog sends, the answer it shows, and the CSV it saves.
//
// The backend owns the physics and says where every number came from; this
// side only has to ask for the right thing and repeat the answer without
// losing its units or its assumptions. Header-only and free of widgets so the
// unit tests can exercise it directly.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <limits>
#include <optional>

namespace visivo::lte {

inline const QString k13co = QStringLiteral("13co");
inline const QString kC18o = QStringLiteral("c18o");
inline const QString k12co = QStringLiteral("12co");

/// What the user chose in the dialog. An unset optional is left out of the
/// request, and the backend then resolves it (from the header, the gradients
/// or its documented default) and says so in the answer.
struct Inputs
{
    QString transition;   ///< "1-0" or "3-2"
    QString dataset13co;
    QString datasetC18o;
    QString dataset12co;   ///< optional
    std::optional<double> vMinKms;
    std::optional<double> vMaxKms;
    std::optional<double> eta13co;
    std::optional<double> etaC18o;
    std::optional<double> eta12co;
    std::optional<double> xRatio;
    std::optional<double> rGcKpc;
    std::optional<double> distanceKpc;
    QString carbonGradient{ QStringLiteral("milam2005") };
    std::optional<double> tExK;
    QString h2From{ QStringLiteral("c18o") };
    std::optional<double> x12co;
    double snrMin{ 3.0 };
    QJsonArray regions;   ///< canonical image-frame regions on the 13CO grid
    QString outputBasename;
};

/// A number typed in a field, in the C locale or the user's; empty is unset.
/// "8.5e-5" and "8,5e-5" both read, a stray word does not.
inline std::optional<double> optionalNumber(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return std::nullopt;
    // Without RejectGroupSeparator the C locale reads "2,500" as 2500, and a
    // user who writes decimal commas gets a distance a thousand times off.
    QLocale c = QLocale::c();
    c.setNumberOptions(QLocale::RejectGroupSeparator);
    QLocale user;
    user.setNumberOptions(QLocale::RejectGroupSeparator);
    bool ok = false;
    double v = c.toDouble(t, &ok);
    if (!ok)
        v = user.toDouble(t, &ok);
    if (!ok || !std::isfinite(v))
        return std::nullopt;
    return v;
}

/// Why the request cannot be sent, or an empty string. The kinds ("cube",
/// "image", or empty when unknown) decide what else is needed: a cube needs
/// the line's window, and T_ex needs either ¹²CO, a typed value, or the
/// peaks that only cubes of ¹³CO and C¹⁸O have.
inline QString validate(const Inputs &in, const QString &kind13, const QString &kind18,
                        const QString &kind12)
{
    const QString cube = QStringLiteral("cube");
    const bool cubeInputs = kind13 == cube || kind18 == cube
            || (!in.dataset12co.isEmpty() && kind12 == cube);
    if (in.transition != QLatin1String("1-0") && in.transition != QLatin1String("3-2"))
        return QStringLiteral("Choose the transition, J = 1–0 or 3–2.");
    if (in.dataset13co.isEmpty() || in.datasetC18o.isEmpty())
        return QStringLiteral("Choose a ¹³CO and a C¹⁸O dataset.");
    if (in.dataset13co == in.datasetC18o)
        return QStringLiteral("The ¹³CO and C¹⁸O datasets are the same one.");
    if (!in.dataset12co.isEmpty()
        && (in.dataset12co == in.dataset13co || in.dataset12co == in.datasetC18o))
        return QStringLiteral("The ¹²CO dataset is also chosen as ¹³CO or C¹⁸O.");
    if (cubeInputs) {
        if (!in.vMinKms || !in.vMaxKms)
            return QStringLiteral("A cube needs the line's velocity window (km/s).");
        if (!(*in.vMinKms < *in.vMaxKms))
            return QStringLiteral("The velocity window's start must be below its end.");
    }
    const auto badEta = [](const std::optional<double> &e) {
        return e && !(*e > 0.0 && *e <= 1.0);
    };
    if (badEta(in.eta13co) || badEta(in.etaC18o)
        || (!in.dataset12co.isEmpty() && badEta(in.eta12co)))
        return QStringLiteral("η_mb must be above 0 and at most 1.");
    if (!in.xRatio && !in.rGcKpc && !in.distanceKpc)
        return QStringLiteral("Give X(¹³CO)/X(C¹⁸O), R_GC or the distance: X comes from the "
                              "galactocentric gradients at R_GC.");
    if (!in.tExK && in.dataset12co.isEmpty() && !kind13.isEmpty() && !kind18.isEmpty()
        && (kind13 != cube || kind18 != cube))
        return QStringLiteral("T_ex needs the line peaks, which a 2-D map does not have: type a "
                              "fixed T_ex, add ¹²CO, or use ¹³CO and C¹⁸O cubes.");
    if (in.xRatio && !(*in.xRatio > 1.0))
        return QStringLiteral("X(¹³CO)/X(C¹⁸O) must be above 1.");
    if (in.rGcKpc && *in.rGcKpc < 0.0)
        return QStringLiteral("R_GC cannot be negative.");
    if (in.distanceKpc && !(*in.distanceKpc > 0.0))
        return QStringLiteral("The distance must be above 0.");
    if (in.tExK && !(*in.tExK > 2.73))
        return QStringLiteral("A fixed T_ex must be above the background, 2.73 K.");
    if (in.x12co && !(*in.x12co > 0.0))
        return QStringLiteral("X(¹²CO) must be above 0.");
    if (!(in.snrMin >= 0.0))
        return QStringLiteral("The S/N threshold cannot be negative.");
    return {};
}

inline QJsonObject requestBody(const Inputs &in)
{
    QJsonObject body{
        { QStringLiteral("transition"), in.transition },
        { QStringLiteral("dataset_id_13co"), in.dataset13co },
        { QStringLiteral("dataset_id_c18o"), in.datasetC18o },
        { QStringLiteral("carbon_gradient"), in.carbonGradient },
        { QStringLiteral("h2_from"), in.h2From },
        { QStringLiteral("snr_min"), in.snrMin },
        { QStringLiteral("regions"), in.regions },
        { QStringLiteral("write_maps"), true },
    };
    if (!in.dataset12co.isEmpty())
        body.insert(QStringLiteral("dataset_id_12co"), in.dataset12co);
    const auto put = [&body](const char *key, const std::optional<double> &v) {
        if (v)
            body.insert(QLatin1String(key), *v);
    };
    put("v_min_kms", in.vMinKms);
    put("v_max_kms", in.vMaxKms);
    put("eta_mb_13co", in.eta13co);
    put("eta_mb_c18o", in.etaC18o);
    if (!in.dataset12co.isEmpty())
        put("eta_mb_12co", in.eta12co);
    put("x_ratio", in.xRatio);
    put("r_gc_kpc", in.rGcKpc);
    put("distance_kpc", in.distanceKpc);
    put("t_ex_k", in.tExK);
    put("x12co", in.x12co);
    if (!in.outputBasename.trimmed().isEmpty())
        body.insert(QStringLiteral("output_basename"), in.outputBasename.trimmed());
    return body;
}

// ── The answer ───────────────────────────────────────────────────────────────

inline constexpr double kUnknown = std::numeric_limits<double>::quiet_NaN();

/// A number of the answer; null (not computable) reads as NaN.
inline double number(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : kUnknown;
}

struct RegionResult
{
    int index{ -1 };
    int nPixels{ 0 };
    double beamPixels{ kUnknown };
    double w13co{ kUnknown }, sigmaW13co{ kUnknown };
    double wC18o{ kUnknown }, sigmaWC18o{ kUnknown };
    double ratio{ kUnknown }, sigmaRatio{ kUnknown };
    double tau13co{ kUnknown }, tauC18o{ kUnknown };
    double tau13coLow{ kUnknown }, tau13coHigh{ kUnknown };
    int flag{ -1 };
    double tExK{ kUnknown };
    double n13co{ kUnknown }, nC18o{ kUnknown }, nH2{ kUnknown };
    double massMsun{ kUnknown };
    QString note;
    QString error;
};

struct MapResult
{
    QString quantity;
    QString outputPath;
    QString workspaceFilename;
    QString bunit;
    double rangeMin{ 0.0 };
    double rangeMax{ 0.0 };
};

struct Response
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    QJsonObject assumptions;
    QStringList warnings;
    QVector<RegionResult> regions;
    QVector<MapResult> maps;
    QVector<QPair<QString, int>> pixelFlags;
};

inline RegionResult parseRegion(const QJsonObject &o)
{
    RegionResult r;
    r.index = o.value(QStringLiteral("index")).toInt(-1);
    r.nPixels = o.value(QStringLiteral("n_pixels")).toInt();
    r.beamPixels = number(o, "beam_pixels");
    r.w13co = number(o, "w_13co_kkms");
    r.sigmaW13co = number(o, "sigma_w_13co_kkms");
    r.wC18o = number(o, "w_c18o_kkms");
    r.sigmaWC18o = number(o, "sigma_w_c18o_kkms");
    r.ratio = number(o, "ratio");
    r.sigmaRatio = number(o, "sigma_ratio");
    r.tau13co = number(o, "tau_13co");
    r.tauC18o = number(o, "tau_c18o");
    const QJsonArray interval = o.value(QStringLiteral("tau_13co_interval")).toArray();
    if (interval.size() == 2) {
        r.tau13coLow = interval.at(0).isDouble() ? interval.at(0).toDouble() : kUnknown;
        r.tau13coHigh = interval.at(1).isDouble() ? interval.at(1).toDouble() : kUnknown;
    }
    r.flag = o.value(QStringLiteral("flag")).toInt(-1);
    r.tExK = number(o, "t_ex_k");
    r.n13co = number(o, "n_13co_cm2");
    r.nC18o = number(o, "n_c18o_cm2");
    r.nH2 = number(o, "n_h2_cm2");
    r.massMsun = number(o, "mass_msun");
    r.note = o.value(QStringLiteral("note")).toString();
    r.error = o.value(QStringLiteral("error")).toString();
    return r;
}

inline Response parseResponse(const QJsonObject &o)
{
    Response r;
    r.valid = o.value(QStringLiteral("valid")).toBool();
    r.error = o.value(QStringLiteral("error")).toString();
    r.width = o.value(QStringLiteral("width")).toInt();
    r.height = o.value(QStringLiteral("height")).toInt();
    r.assumptions = o.value(QStringLiteral("assumptions")).toObject();
    for (const QJsonValue &w : o.value(QStringLiteral("warnings")).toArray())
        r.warnings.append(w.toString());
    for (const QJsonValue &v : o.value(QStringLiteral("regions")).toArray())
        r.regions.append(parseRegion(v.toObject()));
    for (const QJsonValue &v : o.value(QStringLiteral("maps")).toArray()) {
        const QJsonObject m = v.toObject();
        MapResult map;
        map.quantity = m.value(QStringLiteral("quantity")).toString();
        map.outputPath = m.value(QStringLiteral("output_path")).toString();
        map.workspaceFilename = m.value(QStringLiteral("workspace_filename")).toString();
        map.bunit = m.value(QStringLiteral("bunit")).toString();
        map.rangeMin = m.value(QStringLiteral("range_min")).toDouble();
        map.rangeMax = m.value(QStringLiteral("range_max")).toDouble();
        r.maps.append(map);
    }
    // The backend's order (ok, thin, no_solution, below_snr, no_tex) is the
    // order they are worth reading in; a QJsonObject would sort them.
    static const char *const kFlagOrder[] = { "ok", "thin", "no_solution", "below_snr", "no_tex" };
    const QJsonObject flags = o.value(QStringLiteral("pixel_flags")).toObject();
    for (const char *key : kFlagOrder)
        if (flags.contains(QLatin1String(key)))
            r.pixelFlags.append({ QLatin1String(key), flags.value(QLatin1String(key)).toInt() });
    for (auto it = flags.begin(); it != flags.end(); ++it) {
        bool known = false;
        for (const char *key : kFlagOrder)
            known = known || it.key() == QLatin1String(key);
        if (!known)
            r.pixelFlags.append({ it.key(), it.value().toInt() });
    }
    return r;
}

/// The species whose η_mb the backend asked for ("eta_mb for 13CO is
/// required…"), as the request keys (13co, c18o, 12co).
inline QStringList etaRequiredFor(const QString &error)
{
    QStringList out;
    static const QRegularExpression re(QStringLiteral("eta_mb for (13CO|C18O|12CO) is required"));
    auto it = re.globalMatch(error);
    while (it.hasNext()) {
        const QString species = it.next().captured(1).toLower();
        if (!out.contains(species))
            out.append(species);
    }
    return out;
}

/// The η_mb each species was given and where it came from, from the answer's
/// assumptions, as {request key, value, source}. Measurements already on the
/// T_mb scale give one "all" entry.
struct EtaUsed
{
    QString species;
    double value{ kUnknown };
    QString source;
};

inline QVector<EtaUsed> etaResolution(const QJsonObject &assumptions)
{
    QVector<EtaUsed> out;
    const QJsonObject eta = assumptions.value(QStringLiteral("eta_mb")).toObject();
    static const char *const kOrder[][2] = { { "13CO", "13co" }, { "C18O", "c18o" },
                                             { "12CO", "12co" }, { "all", "all" } };
    for (const auto &pair : kOrder) {
        const QJsonArray a = eta.value(QLatin1String(pair[0])).toArray();
        if (a.size() == 2)
            out.append({ QLatin1String(pair[1]), a.at(0).toDouble(kUnknown), a.at(1).toString() });
    }
    return out;
}

// ── Names, units and the words shown to the user ────────────────────────────

inline QString speciesLabel(const QString &key)
{
    if (key == k13co)
        return QStringLiteral("¹³CO");
    if (key == kC18o)
        return QStringLiteral("C¹⁸O");
    if (key == k12co)
        return QStringLiteral("¹²CO");
    return key;
}

/// "13CO", "C18O", "12CO" as the backend writes them, in Unicode.
inline QString prettySpecies(QString text)
{
    text.replace(QStringLiteral("13CO"), QStringLiteral("¹³CO"));
    text.replace(QStringLiteral("C18O"), QStringLiteral("C¹⁸O"));
    text.replace(QStringLiteral("12CO"), QStringLiteral("¹²CO"));
    return text;
}

inline QString transitionLabel(const QString &transition)
{
    if (transition == QLatin1String("1-0"))
        return QStringLiteral("J = 1–0");
    if (transition == QLatin1String("3-2"))
        return QStringLiteral("J = 3–2");
    return transition;
}

/// The map's name as a label, with its unit.
inline QString mapLabel(const QString &quantity)
{
    if (quantity == QLatin1String("ratio"))
        return QStringLiteral("R = W(¹³CO)/W(C¹⁸O)");
    if (quantity == QLatin1String("tau_13co"))
        return QStringLiteral("τ(¹³CO)");
    if (quantity == QLatin1String("tau_c18o"))
        return QStringLiteral("τ(C¹⁸O)");
    if (quantity == QLatin1String("tex"))
        return QStringLiteral("T_ex [K]");
    if (quantity == QLatin1String("n_13co"))
        return QStringLiteral("N(¹³CO) [cm⁻²]");
    if (quantity == QLatin1String("n_c18o"))
        return QStringLiteral("N(C¹⁸O) [cm⁻²]");
    if (quantity == QLatin1String("n_h2"))
        return QStringLiteral("N(H₂) [cm⁻²]");
    if (quantity == QLatin1String("flag"))
        return QStringLiteral("Flag (0 ok, 1 thin, 2 no LTE solution, 3 below S/N)");
    return quantity;
}

inline QString flagText(int flag)
{
    switch (flag) {
    case 0: return QStringLiteral("ok");
    case 1: return QStringLiteral("optically thin (R ≥ X)");
    case 2: return QStringLiteral("no LTE solution (R ≤ 1)");
    case 3: return QStringLiteral("below S/N");
    default: return QString();
    }
}

inline QString pixelFlagLabel(const QString &key)
{
    if (key == QLatin1String("ok"))
        return QStringLiteral("solved");
    if (key == QLatin1String("thin"))
        return QStringLiteral("optically thin");
    if (key == QLatin1String("no_solution"))
        return QStringLiteral("no LTE solution");
    if (key == QLatin1String("below_snr"))
        return QStringLiteral("below S/N");
    if (key == QLatin1String("no_tex"))
        return QStringLiteral("of which T_ex unknown");
    return key;
}

/// A value for the table, or "–" when it could not be computed.
inline QString fmt(double v, int digits = 3)
{
    if (!std::isfinite(v))
        return QStringLiteral("–");
    const double a = std::abs(v);
    if (a != 0.0 && (a >= 1e5 || a < 1e-3))
        return QString::number(v, 'e', digits - 1);
    return QString::number(v, 'g', digits);
}

inline QString withError(double v, double sigma, int digits = 3)
{
    if (!std::isfinite(v))
        return QStringLiteral("–");
    if (!std::isfinite(sigma))
        return fmt(v, digits);
    return QStringLiteral("%1 ± %2").arg(fmt(v, digits), fmt(sigma, 2));
}

/// τ at R ± σ_R. An open end means R − σ_R has no LTE solution (τ unbounded
/// above), or R + σ_R none below; "–" would read as "not computed".
inline QString tauInterval(const RegionResult &r)
{
    const bool lo = std::isfinite(r.tau13coLow), hi = std::isfinite(r.tau13coHigh);
    if (!lo && !hi)
        return QStringLiteral("–");
    if (!hi)
        return QStringLiteral("≥ %1").arg(fmt(r.tau13coLow));
    if (!lo)
        return QStringLiteral("≤ %1").arg(fmt(r.tau13coHigh));
    return QStringLiteral("%1 – %2").arg(fmt(r.tau13coLow), fmt(r.tau13coHigh));
}

/// The table's columns, with units, in the order they are shown and saved.
inline QStringList tableColumns()
{
    return { QStringLiteral("Region"),
             QStringLiteral("Pixels"),
             QStringLiteral("W_mb(¹³CO) [K km/s]"),
             QStringLiteral("W_mb(C¹⁸O) [K km/s]"),
             QStringLiteral("R"),
             QStringLiteral("τ(¹³CO)"),
             QStringLiteral("τ(¹³CO) at R ± σ_R"),
             QStringLiteral("τ(C¹⁸O)"),
             QStringLiteral("T_ex [K]"),
             QStringLiteral("N(¹³CO) [cm⁻²]"),
             QStringLiteral("N(C¹⁸O) [cm⁻²]"),
             QStringLiteral("N(H₂) [cm⁻²]"),
             QStringLiteral("Mass [M☉]"),
             QStringLiteral("Flag") };
}

inline QStringList tableRow(const RegionResult &r, const QString &name)
{
    QString flag = !r.error.isEmpty() ? r.error : flagText(r.flag);
    if (r.error.isEmpty() && (r.flag == 0 || r.flag == 1) && !std::isfinite(r.tExK))
        flag += QStringLiteral(", T_ex unknown");
    return { name,
             QString::number(r.nPixels),
             withError(r.w13co, r.sigmaW13co),
             withError(r.wC18o, r.sigmaWC18o),
             withError(r.ratio, r.sigmaRatio),
             fmt(r.tau13co),
             tauInterval(r),
             fmt(r.tauC18o),
             fmt(r.tExK),
             fmt(r.n13co),
             fmt(r.nC18o),
             fmt(r.nH2),
             fmt(r.massMsun),
             flag };
}

/// The assumptions of the answer as sentences, one per line, in the order an
/// astronomer checks them.
inline QStringList assumptionLines(const QJsonObject &a)
{
    QStringList out;
    const auto pair = [&a](const char *key, double &value, QString &source) {
        const QJsonArray p = a.value(QLatin1String(key)).toArray();
        if (p.size() != 2)
            return false;
        value = p.at(0).toDouble(kUnknown);
        source = p.at(1).toString();
        return true;
    };
    if (a.contains(QStringLiteral("transition")))
        out << QStringLiteral("Transition: %1")
                       .arg(transitionLabel(a.value(QStringLiteral("transition")).toString()));
    for (const EtaUsed &e : etaResolution(a)) {
        const QString who = e.species == QLatin1String("all")
                ? QStringLiteral("all lines")
                : speciesLabel(e.species);
        out << QStringLiteral("η_mb %1 = %2 (%3); T_mb = T_A*/η_mb")
                       .arg(who, fmt(e.value, 4), prettySpecies(e.source));
    }
    double v = kUnknown;
    QString src;
    if (pair("r_gc_kpc", v, src))
        out << QStringLiteral("R_GC = %1 kpc (%2)").arg(fmt(v, 4), src);
    if (pair("x_ratio", v, src))
        out << QStringLiteral("X(¹³CO)/X(C¹⁸O) = %1 (%2)").arg(fmt(v, 4), prettySpecies(src));
    const QJsonObject tex = a.value(QStringLiteral("t_ex")).toObject();
    if (!tex.isEmpty()) {
        const QString how = prettySpecies(tex.value(QStringLiteral("source")).toString());
        out << (tex.contains(QStringLiteral("value_k"))
                        ? QStringLiteral("T_ex = %1 K: %2")
                                  .arg(fmt(tex.value(QStringLiteral("value_k")).toDouble(), 4), how)
                        : QStringLiteral("T_ex from the %1").arg(how));
    }
    const QJsonObject h2 = a.value(QStringLiteral("h2")).toObject();
    if (!h2.isEmpty()) {
        out << QStringLiteral("N(H₂) = N(%1)/%2 (%3)")
                       .arg(prettySpecies(h2.value(QStringLiteral("from")).toString()),
                            fmt(h2.value(QStringLiteral("abundance")).toDouble(), 3),
                            prettySpecies(h2.value(QStringLiteral("source")).toString()));
    }
    if (a.contains(QStringLiteral("distance_kpc")))
        out << QStringLiteral("Distance %1 kpc").arg(fmt(a.value(QStringLiteral("distance_kpc")).toDouble(), 4));
    if (pair("mu_h2", v, src))
        out << QStringLiteral("μ_H₂ = %1 (%2)").arg(fmt(v, 3), src);
    if (a.contains(QStringLiteral("snr_min")))
        out << QStringLiteral("Pixels below %1σ in either line are not solved")
                       .arg(fmt(a.value(QStringLiteral("snr_min")).toDouble(), 3));
    if (a.contains(QStringLiteral("t_bg_k")))
        out << QStringLiteral("T_bg = %1 K").arg(fmt(a.value(QStringLiteral("t_bg_k")).toDouble(), 4));
    if (a.contains(QStringLiteral("ratio_from")))
        out << QStringLiteral("R from %1; both lines share J(T_ex) − J(T_bg)")
                       .arg(a.value(QStringLiteral("ratio_from")).toString());
    if (a.contains(QStringLiteral("column_density")))
        out << QStringLiteral("N: %1").arg(a.value(QStringLiteral("column_density")).toString());
    if (a.contains(QStringLiteral("partition_function")))
        out << QStringLiteral("Q(T_ex): %1").arg(a.value(QStringLiteral("partition_function")).toString());
    const QJsonValue inputs = a.value(QStringLiteral("inputs"));
    if (inputs.isObject()) {
        const QJsonObject o = inputs.toObject();
        for (const char *sp : { "13CO", "C18O", "12CO" })
            for (const QJsonValue &n : o.value(QLatin1String(sp)).toArray())
                out << QStringLiteral("%1: %2").arg(prettySpecies(QLatin1String(sp)), n.toString());
    } else if (inputs.isString()) {
        out << QStringLiteral("Inputs: %1").arg(inputs.toString());
    }
    return out;
}

// ── Guesses that fill the dialog ─────────────────────────────────────────────

/// The species a file name names: 13co, c18o, 12co, or empty. A plain "co"
/// (for instance "_CO_") is ¹²CO only when no isotopologue is named.
inline QString guessSpecies(const QString &name)
{
    const QString n = name.toLower();
    if (n.contains(QStringLiteral("c18o")) || n.contains(QStringLiteral("c-18-o")))
        return kC18o;
    if (n.contains(QStringLiteral("13co")) || n.contains(QStringLiteral("13c16o"))
        || n.contains(QStringLiteral("13c-o")))
        return k13co;
    static const QRegularExpression plainCo(QStringLiteral("(^|[^a-z0-9])(12)?co([^a-z0-9]|$)"));
    if (n.contains(QStringLiteral("12co")) || plainCo.match(n).hasMatch())
        return k12co;
    return {};
}

struct LineGuess
{
    QString species;
    QString transition;
};

/// The line a rest frequency is, within 0.05 % (CDMS frequencies).
inline LineGuess guessLine(double restFreqHz)
{
    struct Line { double ghz; const char *species; const char *transition; };
    static const Line kLines[] = {
        { 110.2013543, "13co", "1-0" }, { 109.7821757, "c18o", "1-0" },
        { 115.2712018, "12co", "1-0" }, { 330.5879653, "13co", "3-2" },
        { 329.3305523, "c18o", "3-2" }, { 345.7959899, "12co", "3-2" },
    };
    if (!(restFreqHz > 0.0))
        return {};
    for (const Line &l : kLines)
        if (std::abs(restFreqHz / (l.ghz * 1e9) - 1.0) < 5e-4)
            return { QLatin1String(l.species), QLatin1String(l.transition) };
    return {};
}

/// A spectral value in km/s, when its unit is a velocity.
inline std::optional<double> toKms(double value, const QString &unit)
{
    if (!std::isfinite(value))
        return std::nullopt;
    QString u = unit.trimmed().toLower();
    u.remove(QLatin1Char(' '));
    u.replace(QStringLiteral("."), QString());
    if (u == QLatin1String("km/s") || u == QLatin1String("kms-1") || u == QLatin1String("kms^-1")
        || u == QLatin1String("kms**-1") || u == QLatin1String("kmps"))
        return value;
    if (u == QLatin1String("m/s") || u == QLatin1String("ms-1") || u == QLatin1String("ms^-1")
        || u == QLatin1String("ms**-1"))
        return value / 1000.0;
    return std::nullopt;
}

/// Shapes the backend measures (lines, vectors and text have no area).
inline bool isAreaRegion(const QJsonObject &canonical)
{
    static const QStringList kShapes{ QStringLiteral("circle"),  QStringLiteral("ellipse"),
                                      QStringLiteral("box"),     QStringLiteral("annulus"),
                                      QStringLiteral("polygon"), QStringLiteral("point"),
                                      QStringLiteral("ellipse_annulus"),
                                      QStringLiteral("box_annulus") };
    return kShapes.contains(canonical.value(QStringLiteral("shape")).toString().toLower());
}

// ── CSV ──────────────────────────────────────────────────────────────────────

inline QString csvField(const QString &s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n'))
        || s.contains(QLatin1Char('\r'))) {
        QString q = s;
        q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + q + QLatin1Char('"');
    }
    return s;
}

/// A full-precision number for the CSV; empty when not computable.
inline QString csvNumber(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 10) : QString();
}

/// The table as CSV: '#' lines of provenance (@p provenance, then the
/// assumptions and the warnings), a header with units, one row per region.
/// Values and their errors go in columns of their own, at full precision.
inline QString toCsv(const Response &r, const QStringList &regionNames,
                     const QStringList &provenance)
{
    QString out;
    for (const QString &line : provenance)
        out += QStringLiteral("# ") + line + QLatin1Char('\n');
    for (const QString &line : assumptionLines(r.assumptions))
        out += QStringLiteral("# ") + line + QLatin1Char('\n');
    for (const QString &w : r.warnings)
        out += QStringLiteral("# Warning: ") + prettySpecies(w) + QLatin1Char('\n');
    out += QStringLiteral(
            "region,n_pixels,beam_pixels,W_13CO_K_km_s,sigma_W_13CO_K_km_s,W_C18O_K_km_s,"
            "sigma_W_C18O_K_km_s,R,sigma_R,tau_13CO,tau_13CO_low,tau_13CO_high,tau_C18O,"
            "T_ex_K,N_13CO_cm-2,N_C18O_cm-2,N_H2_cm-2,mass_Msun,flag,flag_meaning,note\n");
    for (int i = 0; i < r.regions.size(); ++i) {
        const RegionResult &g = r.regions.at(i);
        const QString name = i < regionNames.size() ? regionNames.at(i)
                                                    : QStringLiteral("Region %1").arg(g.index + 1);
        QStringList f{ csvField(name),
                       QString::number(g.nPixels),
                       csvNumber(g.beamPixels),
                       csvNumber(g.w13co), csvNumber(g.sigmaW13co),
                       csvNumber(g.wC18o), csvNumber(g.sigmaWC18o),
                       csvNumber(g.ratio), csvNumber(g.sigmaRatio),
                       csvNumber(g.tau13co), csvNumber(g.tau13coLow),
                       !std::isfinite(g.tau13coHigh) && std::isfinite(g.tau13coLow)
                               ? QStringLiteral("inf")
                               : csvNumber(g.tau13coHigh),
                       csvNumber(g.tauC18o),
                       csvNumber(g.tExK),
                       csvNumber(g.n13co), csvNumber(g.nC18o), csvNumber(g.nH2),
                       csvNumber(g.massMsun),
                       g.flag >= 0 ? QString::number(g.flag) : QString(),
                       csvField(flagText(g.flag)),
                       csvField(!g.error.isEmpty() ? g.error : g.note) };
        out += f.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    return out;
}

}   // namespace visivo::lte

#endif   // LteCoRequest_h

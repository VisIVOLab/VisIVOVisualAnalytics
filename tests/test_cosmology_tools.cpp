// test_cosmology_tools.cpp
// See TestCosmologyTools.h.

#include "TestCosmologyTools.h"

#include "BackendClient.h"
#include "Catalogue3DParser.h"
#include "CosmologyDistanceTable.h"
#include "CosmologyModel.h"
#include "CosmologyReplay.h"
#include "PowerSpectrumSlope.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <cmath>
#include <functional>

using namespace Qt::StringLiterals;
namespace cosmo = visivo::cosmology;

namespace {

QJsonObject fromLiteral(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

QString backendSource(const QString &relative)
{
    QFile f(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend/app/"_s + relative);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

/// The fields of `class <name>(BaseModel):` in a router: one `name: type` per
/// line at four spaces of indent, up to the next blank-line-separated block.
QSet<QString> requestModelFields(const QString &relative, const QString &className)
{
    const QString src = backendSource(relative);
    const qsizetype start = src.indexOf(u"class %1(BaseModel):"_s.arg(className));
    if (start < 0)
        return {};
    const qsizetype end = src.indexOf(u"\n\n\n"_s, start);
    const QString body = src.mid(start, end > start ? end - start : -1);
    QSet<QString> fields;
    static const QRegularExpression field(u"^    (\\w+): "_s, QRegularExpression::MultilineOption);
    for (auto it = field.globalMatch(body); it.hasNext();)
        fields.insert(it.next().captured(1));
    return fields;
}

QSet<QString> keysOf(const QVariantMap &map)
{
    QSet<QString> keys;
    for (const QString &k : map.keys())
        keys.insert(k);
    return keys;
}

QString joined(const QSet<QString> &set)
{
    return QStringList(set.begin(), set.end()).join(u", "_s);
}

/// c/H0 in Mpc for H0 = 70 km/s/Mpc.
constexpr double kHubbleDistance70 = 299792.458 / 70.0;

}   // namespace

// ── CosmologyModel.h ─────────────────────────────────────────────────────────

void TestCosmologyTools::presetsAreTheBackendsFour()
{
    // The list app/dependencies.py loads: a preset the backend does not know
    // would be answered "Unknown cosmology model".
    const QString deps = backendSource(u"dependencies.py"_s);
    QVERIFY2(!deps.isEmpty(), "could not read backend/app/dependencies.py");
    for (const QString &name : cosmo::presetNames())
        QVERIFY2(deps.contains(u"\"%1\""_s.arg(name)), qPrintable(name));
    QCOMPARE(cosmo::presetNames().size(), 4);
}

void TestCosmologyTools::customModelIsValidatedLikeTheBackend()
{
    QVERIFY(cosmo::validate(cosmo::Model::fromPreset(u"WMAP9"_s)).isEmpty());
    QVERIFY(cosmo::validate(cosmo::Model::custom(70.0, 0.3, 0.7)).isEmpty());
    QVERIFY(cosmo::validate(cosmo::Model::custom(70.0, 0.3, 0.0)).isEmpty());   // open
    QVERIFY(!cosmo::validate(cosmo::Model::custom(39.9, 0.3, 0.7)).isEmpty());
    QVERIFY(!cosmo::validate(cosmo::Model::custom(120.1, 0.3, 0.7)).isEmpty());
    QVERIFY(!cosmo::validate(cosmo::Model::custom(70.0, -0.1, 0.7)).isEmpty());
    QVERIFY(!cosmo::validate(cosmo::Model::custom(70.0, 0.3, 1.6)).isEmpty());
    // Closed: the backend refuses Ωm + ΩΛ > 1, and so does the form.
    const QString closed = cosmo::validate(cosmo::Model::custom(70.0, 0.4, 0.7));
    QVERIFY(closed.contains(u"closed"_s));
    // de Sitter: an infinite age, which the server answers with HTTP 500.
    QVERIFY(cosmo::validate(cosmo::Model::custom(70.0, 0.0, 1.0)).contains(u"de Sitter"_s));
    QVERIFY(cosmo::validate(cosmo::Model::custom(70.0, 0.0, 0.9)).isEmpty());   // finite age
    // The bounds are the route's own.
    const QString route = backendSource(u"routers/cosmology.py"_s);
    QVERIFY(route.contains(u"40.0 <= H0 <= 120.0"_s));
    QVERIFY(route.contains(u"0.0 <= Om0 <= 1.5"_s));
    QVERIFY(route.contains(u"(Om0 + Ode0) > 1.0"_s));
}

void TestCosmologyTools::describeSaysWhenACustomModelIsNotFlat()
{
    QCOMPARE(cosmo::describe(cosmo::Model::fromPreset(u"Planck15"_s)), u"Planck15"_s);
    const QString flat = cosmo::describe(cosmo::Model::custom(70.0, 0.3, 0.7));
    QVERIFY2(flat.contains(u"flat"_s), qPrintable(flat));
    QVERIFY(!flat.contains(u"open"_s));
    const QString open = cosmo::describe(cosmo::Model::custom(70.0, 0.3, 0.6));
    QVERIFY2(open.contains(u"open"_s) && open.contains(u"0.1000"_s), qPrintable(open));
    QVERIFY(!open.contains(u"flat"_s));
    QVERIFY(cosmo::describeAscii(cosmo::Model::custom(70.0, 0.3, 0.6)).contains(u"Ok0=0.1000"_s));
    QVERIFY(cosmo::describeAscii(cosmo::Model::custom(70.0, 0.3, 0.7)).contains(u"Tcmb0=0"_s));
    // A spin box's rounding is not curvature.
    QVERIFY(cosmo::Model::custom(70.0, 0.3089, 1.0 - 0.3089).isFlat());
}

void TestCosmologyTools::redshiftListAcceptsCommasSpacesAndLines()
{
    const auto parsed = cosmo::parseRedshifts(u"0.1, 0.5;1\n2\t3   0"_s);
    QCOMPARE(parsed.redshifts, (QVector<double>{ 0.1, 0.5, 1.0, 2.0, 3.0, 0.0 }));
    QVERIFY(parsed.rejected.isEmpty());
}

void TestCosmologyTools::redshiftListReportsWhatItRefuses()
{
    const auto parsed = cosmo::parseRedshifts(u"0.2 abc -1 nan 1e400 4"_s);
    QCOMPARE(parsed.redshifts, (QVector<double>{ 0.2, 4.0 }));
    QCOMPARE(parsed.rejected, (QStringList{ u"abc"_s, u"-1"_s, u"nan"_s, u"1e400"_s }));
}

void TestCosmologyTools::scaleIsAngularDiameterDistancePerArcsecond()
{
    // 1 Mpc at 1″ subtends 1000 kpc × π / 648000.
    QCOMPARE(cosmo::kpcPerArcsecFromAngularDiameter(1000.0), 1000.0 * 1000.0 * M_PI / 648000.0);
    // astropy 7: Planck18.angular_diameter_distance(1) = 1697.8172 Mpc and
    // Planck18.kpc_proper_per_arcmin(1) / 60 = 8.2312502 kpc/″ — the same
    // quantity, so the conversion must reproduce it to the digits given.
    QVERIFY(std::abs(cosmo::kpcPerArcsecFromAngularDiameter(1697.8172355757831) - 8.231250238)
            < 1e-8);
    QVERIFY(std::isnan(cosmo::kpcPerArcsecFromAngularDiameter(std::nan(""))));
}

void TestCosmologyTools::tableNamesTheModelAndLeavesMissingCellsEmpty()
{
    cosmo::Row row;
    row.z = 0.5;
    row.comovingMpc = 1946.4174;
    row.luminosityMpc = 2919.6261;
    row.kpcPerArcsec = 6.1813;
    const QString csv = cosmo::formatTable(cosmo::Model::fromPreset(u"WMAP9"_s), { row }, u',');
    const QStringList lines = csv.split(u'\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0), u"# cosmology: WMAP9"_s);
    QVERIFY(lines.at(1).startsWith(u"z,D_C comoving (Mpc),D_L luminosity (Mpc)"_s));
    // D_A, lookback and age are not provided under WMAP9: empty, never 0.
    QCOMPARE(lines.at(2), u"0.5,1946.417,2919.626,,,,6.1813,"_s);
}

void TestCosmologyTools::tableQuotesCellsThatHoldTheSeparator()
{
    cosmo::Row row;
    row.z = 25.0;
    row.error = u"refused, z > 20"_s;
    const QString csv = cosmo::formatTable(cosmo::Model::custom(70.0, 0.3, 0.7), { row }, u',');
    QVERIFY(csv.startsWith(u"# cosmology: Custom LambdaCDM (H0=70 km/s/Mpc, Om0=0.3, Ode0=0.7"_s));
    QVERIFY2(csv.contains(u"\"refused, z > 20\""_s), qPrintable(csv));
    const QString tsv = cosmo::formatTable(cosmo::Model::custom(70.0, 0.3, 0.7), { row }, u'\t');
    QVERIFY(tsv.contains(u"\trefused, z > 20\n"_s));
}

void TestCosmologyTools::customModelRoundTripsThroughSettings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(u"cosmo.ini"_s), QSettings::IniFormat);
    const cosmo::Model fresh = cosmo::loadCustom(settings);
    QVERIFY(fresh.isCustom());
    QCOMPARE(fresh.h0, 70.0);
    QVERIFY(fresh.isFlat());
    cosmo::saveCustom(settings, cosmo::Model::custom(67.5, 0.25, 0.6));
    const cosmo::Model back = cosmo::loadCustom(settings);
    QCOMPARE(back, cosmo::Model::custom(67.5, 0.25, 0.6));
}

// ── CosmologyDistanceTable.h ─────────────────────────────────────────────────

void TestCosmologyTools::distinctUsableDropsUnmeasuredAndRepeats()
{
    const QVector<double> z{ 0.3, -1.0, 0.1, 0.3, 0.0, std::nan(""), 2.0, 0.1 };
    QCOMPARE(cosmo::distinctUsable(z), (QVector<double>{ 0.1, 0.3, 2.0 }));
}

void TestCosmologyTools::tableNodesSpanTheRangeExactly()
{
    const QVector<double> nodes = cosmo::tableNodes(0.01, 3.0);
    QCOMPARE(nodes.size(), cosmo::kTableNodes);
    QCOMPARE(nodes.first(), 0.01);
    QCOMPARE(nodes.last(), 3.0);
    for (int i = 1; i < nodes.size(); ++i)
        QVERIFY(nodes[i] > nodes[i - 1]);
    QVERIFY(cosmo::tableNodes(0.0, 1.0).isEmpty());
    QVERIFY(cosmo::tableNodes(1.0, 1.0).isEmpty());
}

void TestCosmologyTools::interpolationIsWithinOnePartIn1e4()
{
    // Three stand-ins for the backend, two of them closed-form: Einstein–de
    // Sitter (Ωm = 1), the empty Milne universe (Ωk = 1), and the client's own
    // flat Planck18 integral. Measured against astropy the worst case over
    // flat, open and matter-only models was 6e-5 at 128 nodes.
    const std::function<double(double)> models[] = {
        [](double z) { return 2.0 * kHubbleDistance70 * (1.0 - 1.0 / std::sqrt(1.0 + z)); },
        [](double z) { return kHubbleDistance70 * std::log1p(z); },
        [](double z) { return Catalogue3DParser::detail::comovingDistanceMpc(z); },
    };
    const std::pair<double, double> ranges[] = { { 1e-4, 0.05 }, { 1e-3, 1.0 }, { 1e-4, 20.0 },
                                                  { 0.5, 0.6 } };
    for (const auto &dc : models) {
        for (const auto &[lo, hi] : ranges) {
            const QVector<double> nodes = cosmo::tableNodes(lo, hi);
            QVector<double> d;
            for (double z : nodes)
                d.append(dc(z));
            double worst = 0.0;
            for (int i = 0; i <= 4000; ++i) {
                const double z = lo + (hi - lo) * i / 4000.0;
                const double truth = dc(z);
                worst = std::max(worst, std::abs(cosmo::interpolate(nodes, d, z) - truth) / truth);
            }
            QVERIFY2(worst < 1e-4, qPrintable(u"range %1–%2: worst %3"_s.arg(lo).arg(hi).arg(worst)));
        }
    }
}

void TestCosmologyTools::interpolationReturnsNodeValuesExactly()
{
    // The exact table (every distinct redshift a node) must give back what the
    // backend said for each, bit for bit.
    const QVector<double> nodes{ 0.0123, 0.5, 1.7 };
    const QVector<double> d{ 52.9, 1888.1, 4900.3 };
    for (int i = 0; i < nodes.size(); ++i)
        QCOMPARE(cosmo::interpolate(nodes, d, nodes[i]), d[i]);
    const QVector<double> one{ 0.25 };
    QCOMPARE(cosmo::interpolate(one, { 1000.0 }, 0.25), 1000.0);
    QVERIFY(std::isnan(cosmo::interpolate(one, { 1000.0 }, 0.26)));
}

void TestCosmologyTools::interpolationNeverExtrapolates()
{
    const QVector<double> nodes = cosmo::tableNodes(0.1, 1.0);
    QVector<double> d;
    for (double z : nodes)
        d.append(kHubbleDistance70 * std::log1p(z));
    QVERIFY(std::isnan(cosmo::interpolate(nodes, d, 0.09)));
    QVERIFY(std::isnan(cosmo::interpolate(nodes, d, 1.01)));
    QVERIFY(std::isnan(cosmo::interpolate(nodes, d, 0.0)));
    QVERIFY(std::isnan(cosmo::interpolate(nodes, d, -1.0)));
    QVERIFY(std::isnan(cosmo::interpolate(nodes, d, std::nan(""))));
}

// ── PowerSpectrumSlope.h ─────────────────────────────────────────────────────

void TestCosmologyTools::slopeOfAnExactPowerLaw()
{
    QVector<double> ell, power;
    QVector<int> modes;
    for (int i = 0; i < 30; ++i) {
        const double l = 100.0 * std::pow(10.0, i / 10.0);
        ell.append(l);
        power.append(3e5 * std::pow(l, -2.7));
        modes.append(50);
    }
    const auto fit = visivo::fitPowerSlope(ell, power, modes);
    QVERIFY(fit.valid);
    QVERIFY(std::abs(fit.slope + 2.7) < 1e-10);
    QVERIFY(fit.slopeError < 1e-8);
    QCOMPARE(fit.binsUsed, 30);
    QVERIFY(std::abs(fit.intercept - std::log10(3e5)) < 1e-9);
}

void TestCosmologyTools::slopeHonoursRangeAndModeCount()
{
    // Two power laws: slope −1 below ℓ = 1000, −3 above. The range picks one.
    QVector<double> ell, power;
    QVector<int> modes;
    for (int i = 0; i < 40; ++i) {
        const double l = 10.0 * std::pow(10.0, i / 10.0);
        ell.append(l);
        power.append(l < 1000.0 ? 1e6 / l : 1e12 * std::pow(l, -3.0));
        modes.append(i < 5 ? 2 : 100);   // the lowest bins hold few modes
    }
    visivo::PowerSlopeOptions low;
    low.ellMax = 900.0;
    low.minModes = 8;
    const auto lowFit = visivo::fitPowerSlope(ell, power, modes, low);
    QVERIFY(lowFit.valid);
    QVERIFY(std::abs(lowFit.slope + 1.0) < 1e-9);
    QVERIFY(lowFit.ellLow > 30.0);   // the five sparse bins were left out
    QCOMPARE(lowFit.binsUsed, 15);
    visivo::PowerSlopeOptions high;
    high.ellMin = 1100.0;
    const auto highFit = visivo::fitPowerSlope(ell, power, modes, high);
    QVERIFY(std::abs(highFit.slope + 3.0) < 1e-9);
    // Without the mode cut every bin counts (an older backend sends no n_modes).
    const auto all = visivo::fitPowerSlope(ell, power, {}, low);
    QCOMPARE(all.binsUsed, 20);
}

void TestCosmologyTools::slopeNeedsThreeBins()
{
    const auto two = visivo::fitPowerSlope({ 10.0, 100.0 }, { 1.0, 0.1 }, {});
    QVERIFY(!two.valid);
    QCOMPARE(two.binsUsed, 2);
    QVERIFY(std::isnan(two.slope));
}

void TestCosmologyTools::slopeSkipsNonPositivePower()
{
    const auto fit = visivo::fitPowerSlope({ 10.0, 20.0, 40.0, 80.0, 160.0 },
                                           { 1.0, 0.0, 0.25, -1.0, 1.0 / 16.0 }, {});
    QVERIFY(fit.valid);
    QCOMPARE(fit.binsUsed, 3);
    QVERIFY(std::abs(fit.slope + 1.0) < 1e-12);
}

void TestCosmologyTools::slopeIsWeightedByTheBinErrors()
{
    // A clean power law of slope -2 except one bin far off the line with a
    // huge error: weighting must ignore it, OLS would not. A bin with a null
    // error (NaN) carries no weight at all.
    QVector<double> ell, power, err;
    for (int i = 0; i < 10; ++i) {
        const double l = 100.0 * std::pow(2.0, i);
        ell.append(l);
        power.append(1e8 * std::pow(l, -2.0));
        err.append(0.01 * power.last());
    }
    power[4] *= 10.0;
    err[4] = 1e6 * power[4];
    err[7] = std::nan("");
    const auto fit = visivo::fitPowerSlope(ell, power, {}, {}, err);
    QVERIFY(fit.valid);
    QVERIFY(fit.weighted);
    QCOMPARE(fit.binsUsed, 9);
    QVERIFY2(std::abs(fit.slope + 2.0) < 1e-6, qPrintable(QString::number(fit.slope)));
    // The error is the weighted covariance's: σ_log = 0.01 / ln 10 for every
    // kept bin but the outlier, whose weight is negligible.
    const double sigmaLog = 0.01 / std::log(10.0);
    double mx = 0.0;
    QVector<double> xs;
    for (int i = 0; i < 10; ++i)
        if (i != 4 && i != 7)
            xs.append(std::log10(ell[i]));
    for (double x : xs)
        mx += x;
    mx /= xs.size();
    double sxx = 0.0;
    for (double x : xs)
        sxx += (x - mx) * (x - mx);
    QVERIFY(std::abs(fit.slopeError - sigmaLog / std::sqrt(sxx)) < 1e-6);
    const auto ols = visivo::fitPowerSlope(ell, power, {});
    QVERIFY(!ols.weighted);
    QVERIFY(std::abs(ols.slope + 2.0) > 0.01);   // the outlier pulls it
}

// ── BackendClient bindings ───────────────────────────────────────────────────

void TestCosmologyTools::distancesQueryUsesTheRoutesParameterNames()
{
    BackendCosmologyParams p;
    p.h0 = 67.66;
    p.om0 = 0.30966;
    p.ode0 = 0.6888463055445441;
    const QUrlQuery q(BackendClient::cosmologyDistancesQuery(0.123456789, &p));
    QCOMPARE(q.queryItemValue(u"z"_s).toDouble(), 0.123456789);
    QCOMPARE(q.queryItemValue(u"H0"_s).toDouble(), 67.66);
    QCOMPARE(q.queryItemValue(u"Om0"_s).toDouble(), 0.30966);
    // Every digit travels: the server must get the model the form shows.
    QCOMPARE(q.queryItemValue(u"Ode0"_s).toDouble(), 0.6888463055445441);
    // …and these are the names the route reads.
    const QString route = backendSource(u"routers/cosmology.py"_s);
    const qsizetype start = route.indexOf(u"async def cosmology_distances("_s);
    QVERIFY(start >= 0);
    const QString signature = route.mid(start, route.indexOf(u"):"_s, start) - start);
    for (const QString &name : { u"z"_s, u"H0"_s, u"Om0"_s, u"Ode0"_s })
        QVERIFY2(signature.contains(u"    %1: float"_s.arg(name)), qPrintable(name));
}

void TestCosmologyTools::distancesQueryOmitsParametersForPlanck18()
{
    const QUrlQuery q(BackendClient::cosmologyDistancesQuery(1.0, nullptr));
    QCOMPARE(q.queryItems().size(), 1);
    QCOMPARE(q.queryItemValue(u"z"_s), u"1"_s);
}

void TestCosmologyTools::parseDistancesReadsEveryField()
{
    // The keys of the route's answer, as routers/cosmology.py writes them.
    const auto r = BackendClient::parseCosmologyDistancesObject(fromLiteral(R"({
        "valid": true, "error": "", "z": 1.0,
        "comoving_Mpc": 3395.6341, "luminosity_Mpc": 6791.2682,
        "angular_diameter_Mpc": 1697.8171, "lookback_time_Gyr": 7.9298,
        "age_Gyr": 5.8573 })"));
    QVERIFY(r.valid);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.z, 1.0);
    QCOMPARE(r.comovingMpc, 3395.6341);
    QCOMPARE(r.luminosityMpc, 6791.2682);
    QCOMPARE(r.angularDiameterMpc, 1697.8171);
    QCOMPARE(r.lookbackGyr, 7.9298);
    QCOMPARE(r.ageGyr, 5.8573);
    const QString route = backendSource(u"routers/cosmology.py"_s);
    for (const QString &key : { u"comoving_Mpc"_s, u"luminosity_Mpc"_s, u"angular_diameter_Mpc"_s,
                                u"lookback_time_Gyr"_s, u"age_Gyr"_s })
        QVERIFY2(route.contains(u"\"%1\""_s.arg(key)), qPrintable(key));
}

void TestCosmologyTools::parseDistancesTurnsAMissingFieldIntoNaN()
{
    const auto r = BackendClient::parseCosmologyDistancesObject(
            fromLiteral(R"({"valid": true, "z": 0.5, "comoving_Mpc": 1888.6})"));
    QVERIFY(r.valid);
    QCOMPARE(r.comovingMpc, 1888.6);
    QVERIFY(std::isnan(r.angularDiameterMpc));
    QVERIFY(std::isnan(r.ageGyr));
}

void TestCosmologyTools::parseDistancesReadsARefusalsDetail()
{
    const auto r = BackendClient::parseCosmologyDistancesObject(
            fromLiteral(R"({"detail": "Redshift z must be in [0, 20]."})"));
    QVERIFY(!r.valid);
    QCOMPARE(r.error, u"Redshift z must be in [0, 20]."_s);
}

void TestCosmologyTools::parsePresetDistanceReadsTheScale()
{
    const auto r = BackendClient::parseCosmologyDistanceObject(fromLiteral(R"({
        "valid": true, "error": "", "z": 0.5, "model": "WMAP9",
        "comoving_Mpc": 1888.62, "luminosity_Mpc": 2832.93,
        "proper_motion_kpc_per_arcsec": 6.105 })"));
    QVERIFY(r.valid);
    QCOMPARE(r.model, u"WMAP9"_s);
    QCOMPARE(r.kpcPerArcsec, 6.105);
    QVERIFY(backendSource(u"routers/cosmology.py"_s).contains(u"proper_motion_kpc_per_arcsec"_s));
    const auto refused = BackendClient::parseCosmologyDistanceObject(
            fromLiteral(R"({"valid": false, "error": "Redshift z must be >= 0."})"));
    QVERIFY(!refused.valid);
    QCOMPARE(refused.error, u"Redshift z must be >= 0."_s);
    QVERIFY(std::isnan(refused.comovingMpc));
}

void TestCosmologyTools::parseAngularPowerReadsTheFullAnswer()
{
    const auto r = BackendClient::parseAngularPowerObject(fromLiteral(R"({
        "valid": true, "error": "", "channel": 0, "width": 256, "height": 128,
        "n_bins": 3, "k_bins": [0.01, 0.1, 0.4], "ell": [2160, 21600, 86400],
        "cl": [5.0, 0.5, 0.05], "power": [5.0, 0.5, 0.05], "cl_err": [null, 0.1, 0.01],
        "n_modes": [2, 40, 900], "ell_is_angular": true, "pixel_scale_arcsec": 6.0,
        "mode_coupling": 1.94, "calibrated": false,
        "note": "relative (uncalibrated) power spectrum",
        "k_unit": "pixel^-1", "power_unit": "counts^2" })"));
    QVERIFY(r.valid);
    QCOMPARE(r.nBins, 3);
    QCOMPARE(r.ell, (QVector<double>{ 2160, 21600, 86400 }));
    QCOMPARE(r.power, (QVector<double>{ 5.0, 0.5, 0.05 }));
    QVERIFY(std::isnan(r.clErr.at(0)));   // null: fewer than 3 modes
    QCOMPARE(r.clErr.at(1), 0.1);
    QCOMPARE(r.nModes, (QVector<int>{ 2, 40, 900 }));
    QVERIFY(r.ellIsAngular);
    QCOMPARE(r.pixelScaleArcsec, 6.0);
    QCOMPARE(r.modeCoupling, 1.94);
    QVERIFY(!r.calibrated);
    QVERIFY(r.note.contains(u"uncalibrated"_s));
    // The keys are the worker's.
    const QString worker = backendSource(u"compute/cosmology.py"_s);
    for (const QString &key : { u"ell"_s, u"cl"_s, u"cl_err"_s, u"n_modes"_s, u"ell_is_angular"_s,
                                u"pixel_scale_arcsec"_s, u"mode_coupling"_s, u"calibrated"_s })
        QVERIFY2(worker.contains(u"\"%1\":"_s.arg(key)), qPrintable(key));
}

void TestCosmologyTools::parseAngularPowerFallsBackOnLegacyFields()
{
    // An older backend: `power` only, no ell.
    const auto r = BackendClient::parseAngularPowerObject(fromLiteral(R"({
        "valid": true, "k_bins": [0.01, 0.1], "power": [3.0, 1.0] })"));
    QCOMPARE(r.power, (QVector<double>{ 3.0, 1.0 }));
    QCOMPARE(r.ell, r.kBins);
    QVERIFY(!r.ellIsAngular);
    QVERIFY(r.nModes.isEmpty());
}

void TestCosmologyTools::skeletonBodySendsNullForAutoAndNoPruning()
{
    BackendSkeletonRequest auto_;
    const QJsonObject a = BackendClient::skeletonRequestBody(auto_);
    QVERIFY(a.value(u"threshold"_s).isNull());
    QVERIFY(a.value(u"persistence_threshold"_s).isNull());
    QCOMPARE(a.value(u"mode"_s).toString(), u"skeleton"_s);
    QCOMPARE(a.value(u"smooth_sigma_pix"_s).toDouble(), 0.0);

    BackendSkeletonRequest set;
    set.thresholdAuto = false;
    set.threshold = 0.0;   // a chosen zero is a value, not "auto"
    set.mode = u"void"_s;
    set.smoothSigmaPix = 2.5;
    set.prune = true;
    set.persistenceThreshold = 1.5;
    const QJsonObject b = BackendClient::skeletonRequestBody(set);
    QVERIFY(b.value(u"threshold"_s).isDouble());
    QCOMPARE(b.value(u"threshold"_s).toDouble(), 0.0);
    QCOMPARE(b.value(u"persistence_threshold"_s).toDouble(), 1.5);
    QCOMPARE(b.value(u"mode"_s).toString(), u"void"_s);
}

void TestCosmologyTools::parseSkeletonDecodesTheMask()
{
    const float mask[6] = { 0.f, 255.f, 0.f, 0.f, 255.f, 255.f };
    const QByteArray raw(reinterpret_cast<const char *>(mask), sizeof(mask));
    QJsonObject obj{ { u"valid"_s, true }, { u"error"_s, u""_s }, { u"channel"_s, 0 },
                     { u"width"_s, 3 }, { u"height"_s, 2 }, { u"threshold"_s, 0.42 },
                     { u"mode"_s, u"skeleton"_s }, { u"smooth_sigma_pix"_s, 1.0 },
                     { u"n_skeleton_pixels"_s, 3 }, { u"range_min"_s, 0.0 },
                     { u"range_max"_s, 255.0 },
                     { u"data_base64"_s, QString::fromLatin1(raw.toBase64()) },
                     { u"endpoints"_s, QJsonArray{ QJsonArray{ 0, 1 }, QJsonArray{ 1, 2 } } } };
    const auto r = BackendClient::parseSkeletonObject(obj);
    QVERIFY(r.valid);
    QCOMPARE(r.width, 3);
    QCOMPARE(r.height, 2);
    QCOMPARE(r.threshold, 0.42);   // the applied one, recorded for replay
    QCOMPARE(r.nSkeletonPixels, 3);
    QCOMPARE(r.nEndpoints, 2);
    QCOMPARE(r.data, raw);
}

void TestCosmologyTools::parseSkeletonRefusesAMaskOfTheWrongSize()
{
    const float mask[2] = { 0.f, 255.f };
    const QByteArray raw(reinterpret_cast<const char *>(mask), sizeof(mask));
    const auto r = BackendClient::parseSkeletonObject(
            QJsonObject{ { u"valid"_s, true }, { u"width"_s, 3 }, { u"height"_s, 2 },
                         { u"data_base64"_s, QString::fromLatin1(raw.toBase64()) } });
    QVERIFY(!r.valid);
    QVERIFY(r.data.isEmpty());
    QVERIFY(!r.error.isEmpty());
    const auto missing = BackendClient::parseSkeletonObject(
            fromLiteral(R"({"detail": "scikit-image is required for skeleton detection."})"));
    QVERIFY(!missing.valid);
    QVERIFY(missing.error.contains(u"scikit-image"_s));
}

// ── CosmologyReplay.h ────────────────────────────────────────────────────────

void TestCosmologyTools::angularPowerReplayUsesTheRoutesFieldNames()
{
    const QSet<QString> service =
            requestModelFields(u"routers/cosmology.py"_s, u"AngularPowerRequest"_s);
    QVERIFY2(!service.isEmpty(), "could not read AngularPowerRequest");
    const ReplayStep step = cosmo::angularPowerStep(0, 64);
    QCOMPARE(keysOf(step.request), service);
    QCOMPARE(step.route, u"/v1/cosmology/angular_power/{dataset_id}"_s);
    QVERIFY(backendSource(u"routers/cosmology.py"_s)
                    .contains(u"\"/v1/cosmology/angular_power/{dataset_id}\""_s));
}

void TestCosmologyTools::skeletonReplayUsesTheRoutesFieldNames()
{
    const QSet<QString> service =
            requestModelFields(u"routers/analysis.py"_s, u"SkeletonRequest"_s);
    QVERIFY2(!service.isEmpty(), "could not read SkeletonRequest");
    BackendSkeletonRequest request;
    request.prune = true;
    QCOMPARE(keysOf(cosmo::skeletonStep(request).request), service);
    QVERIFY(backendSource(u"routers/analysis.py"_s)
                    .contains(u"\"/v1/analysis/skeleton/{dataset_id}\""_s));
}

void TestCosmologyTools::momentReplayUsesTheTaskRoutesFieldNames()
{
    const QSet<QString> service =
            requestModelFields(u"routers/products.py"_s, u"MomentSaveFitsRequest"_s);
    QVERIFY2(!service.isEmpty(), "could not read MomentSaveFitsRequest");
    cosmo::MomentSource m;
    m.datasetId = u"abc"_s;
    m.order = 1;
    const QList<ReplayStep> steps = cosmo::momentDatasetSteps(m);
    QCOMPARE(steps.size(), 2);
    const QSet<QString> recorded = keysOf(steps.at(0).request);
    const QSet<QString> unknown = recorded - service;
    QVERIFY2(unknown.isEmpty(), qPrintable(u"not taken by the route: "_s + joined(unknown)));
    // The one field not sent is the variance switch: its default (dispersion)
    // is what the viewer's moment 2 shows.
    QCOMPARE(service - recorded, (QSet<QString>{ u"moment2_variance"_s }));
    QCOMPARE(steps.at(0).route, u"/v1/tasks/moment_fits"_s);
    QCOMPARE(steps.at(1).route, u"/v1/datasets/open"_s);
    QVERIFY(steps.at(1).request.contains(u"path"_s));
}

void TestCosmologyTools::runLabelsPutWhatDiffersFirst()
{
    QCOMPARE(cosmo::powerSpectrumRunLabel(true, -3.6417, u"14:32:05"_s),
             u"n = -3.64 · 14:32:05"_s);
    QCOMPARE(cosmo::powerSpectrumRunLabel(true, -3.6417, u"14:32:05"_s, u"Moment 0"_s),
             u"n = -3.64 · 14:32:05 · Moment 0"_s);
    QCOMPARE(cosmo::powerSpectrumRunLabel(false, std::nan(""), u"09:00:00"_s),
             u"no slope · 09:00:00"_s);
    QCOMPARE(cosmo::skeletonRunLabel(false, 412, u"14:32:05"_s, u"Moment 0"_s),
             u"Filaments · 412 px · 14:32:05 · Moment 0"_s);
    QCOMPARE(cosmo::skeletonRunLabel(true, 7, u"14:33:00"_s), u"Voids · 7 px · 14:33:00"_s);
    QCOMPARE(cosmo::runFinishedAt().size(), 8);   // HH:mm:ss
}

void TestCosmologyTools::momentExportNameIsStablePerRecipe()
{
    const QString cube = u"/data/hcg44.fits"_s;
    const QString rev = u"7340032|2026-11-06T10:00:00+00:00"_s;
    const auto name = [&](const cosmo::MomentSource &m, const QString &path = u"/data/hcg44.fits"_s,
                          const QString &fingerprint = u"7340032|2026-11-06T10:00:00+00:00"_s) {
        return cosmo::momentExportBasename(path, u"hcg44"_s, m, fingerprint);
    };
    cosmo::MomentSource m;
    m.order = 0;
    m.channelStart = 11;
    m.channelEnd = 79;
    const QString a = name(m);
    QVERIFY(a.startsWith(u"hcg44_m0_"_s));
    QVERIFY(a.endsWith(u"_maptools.fits"_s));
    QCOMPARE(cosmo::momentExportBasename(cube, u"hcg44"_s, m, rev), a);
    // The dataset id is volatile and must not enter the name.
    cosmo::MomentSource sameRecipe = m;
    sameRecipe.datasetId = u"another-session"_s;
    QCOMPARE(name(sameRecipe), a);
    cosmo::MomentSource other = m;
    other.channelEnd = 80;
    QVERIFY(name(other) != a);
    QVERIFY(name(m, u"/other/hcg44.fits"_s) != a);
    cosmo::MomentSource masked = m;
    masked.maskEnabled = true;
    masked.thresholdValue = 0.003;
    QVERIFY(name(masked) != a);
    // The same path holding another file (replaced, or rewritten): a new name,
    // so the moment of the old cube is never reused for the new one.
    QVERIFY(name(m, cube, u"7340032|2026-11-07T09:00:00+00:00"_s) != a);
    QVERIFY(name(m, cube, u"7340040|2026-11-06T10:00:00+00:00"_s) != a);
}

void TestCosmologyTools::skeletonDisplayParamsStateTheAppliedThreshold()
{
    BackendSkeletonRequest request;   // auto threshold
    const QVariantMap p = cosmo::skeletonDisplayParams(request, 0.0123);
    QVERIFY(p.value(u"Threshold"_s).toString().contains(u"0.0123"_s));
    QVERIFY(p.value(u"Threshold"_s).toString().contains(u"auto"_s));
    QCOMPARE(p.value(u"Smoothing σ (px)"_s).toString(), u"none"_s);
}

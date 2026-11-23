// test_molecular_line_client.cpp
// See TestMolecularLineClient.h.

#include "TestMolecularLineClient.h"

#include "app/DatasetBeam.h"
#include "app/LineFitRequest.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include <cmath>

namespace linefit = visivo::linefit;
namespace beam = visivo::beam;

namespace {

QVector<double> velocityAxis()
{
    QVector<double> axis;
    for (int i = 0; i < 400; ++i)
        axis.append(-50.0 + 0.5 * i);
    return axis;
}

}   // namespace

void TestMolecularLineClient::aZoomedPlotSendsItsRangeAsTheFitWindow()
{
    // The C18O case of issue #416: zoomed on the 84 km/s line, away from the
    // edge spike at 149 km/s. The axis may come reversed (upper < lower).
    const QVector<double> axis = velocityAxis();
    const QVector<double> values(axis.size(), 0.1);
    const linefit::FitWindow window = linefit::windowFromRange(95.0, 70.0, axis);
    QVERIFY(window.active);
    QCOMPARE(window.xMin, 70.0);
    QCOMPARE(window.xMax, 95.0);
    QCOMPARE(linefit::channelsInWindow(axis, values, window), 51);

    const QJsonObject body = linefit::requestBody(values, axis, true, window);
    QCOMPARE(body.value("x_min").toDouble(), 70.0);
    QCOMPARE(body.value("x_max").toDouble(), 95.0);
    QCOMPARE(body.value("values").toArray().size(), axis.size());
    QVERIFY(body.value("with_baseline").toBool());
}

void TestMolecularLineClient::aPlotShowingEverythingSendsNoWindow()
{
    const QVector<double> axis = velocityAxis();
    const QVector<double> values(axis.size(), 0.1);
    const linefit::FitWindow window = linefit::windowFromRange(-60.0, 160.0, axis);
    QVERIFY(!window.active);
    const QJsonObject body = linefit::requestBody(values, axis, true, window);
    QVERIFY(!body.contains("x_min"));
    QVERIFY(!body.contains("x_max"));
    QVERIFY(!linefit::windowFromRange(std::nan(""), 10.0, axis).active);
    QVERIFY(!linefit::windowFromRange(10.0, 10.0, axis).active);
}

void TestMolecularLineClient::aWindowOutsideTheDataLeavesNoChannelsToFit()
{
    const QVector<double> axis = velocityAxis();
    QVector<double> values(axis.size(), 0.1);
    values[100] = std::nan("");
    const linefit::FitWindow window = linefit::windowFromRange(300.0, 400.0, axis);
    QVERIFY(window.active);
    QCOMPARE(linefit::channelsInWindow(axis, values, window), 0);
    QCOMPARE(linefit::channelsInWindow(axis, values, {}), axis.size() - 1);
}

void TestMolecularLineClient::theFitWarningIsParsed()
{
    const QJsonObject response{
        { "valid", true },
        { "peak", 0.617 },
        { "center", 83.98 },
        { "fwhm", 1.86 },
        { "peak_err", 0.094 },
        { "center_err", QJsonValue() },
        { "fit_warning", "No resolved solution: the best one is returned." },
        { "baseline", QJsonArray{ 0.01, -0.001 } },
    };
    const BackendLineFitResult fit = linefit::parseResponse(response);
    QVERIFY(fit.valid);
    QCOMPARE(fit.peak, 0.617);
    QCOMPARE(fit.peakErr, 0.094);
    QCOMPARE(fit.centerErr, -1.0);
    QCOMPARE(fit.baselineSlope, -0.001);
    QCOMPARE(fit.fitWarning, QStringLiteral("No resolved solution: the best one is returned."));
    QVERIFY(linefit::warningText(fit, true).contains(QStringLiteral("No resolved solution")));
}

void TestMolecularLineClient::aNegativePeakOnAnEmissionSpectrumIsFlagged()
{
    // The pre-fix result on the C18O clump: peak -0.064 K at 54.3 km/s.
    const QVector<double> axis = velocityAxis();
    QVector<double> values(axis.size(), 0.0);
    for (int i = 0; i < axis.size(); ++i)
        values[i] = 0.62 * std::exp(-0.5 * std::pow((axis[i] - 84.0) / 0.8, 2.0));
    QVERIFY(linefit::looksLikeEmission(axis, values, {}));

    BackendLineFitResult fit;
    fit.valid = true;
    fit.peak = -0.064;
    const QString warning = linefit::warningText(fit, true);
    QVERIFY(warning.contains(QStringLiteral("negative")));

    fit.peak = 0.617;
    QVERIFY(linefit::warningText(fit, true).isEmpty());
}

void TestMolecularLineClient::aNegativePeakOnAnAbsorptionSpectrumIsNot()
{
    const QVector<double> axis = velocityAxis();
    QVector<double> values(axis.size(), 1.0);
    for (int i = 0; i < axis.size(); ++i)
        values[i] -= 0.5 * std::exp(-0.5 * std::pow((axis[i] - 20.0) / 2.0, 2.0));
    QVERIFY(!linefit::looksLikeEmission(axis, values, {}));
    BackendLineFitResult fit;
    fit.peak = -0.5;
    QVERIFY(linefit::warningText(fit, false).isEmpty());
}

void TestMolecularLineClient::theBeamRequestCarriesArcsecAndClear()
{
    beam::BeamRequest request;
    request.datasetId = QStringLiteral("ds1");
    request.majorArcsec = 15.0;
    QJsonObject body = beam::requestBody(request);
    QCOMPARE(body.value("dataset_id").toString(), QStringLiteral("ds1"));
    QCOMPARE(body.value("major_arcsec").toDouble(), 15.0);
    QVERIFY(!body.contains("minor_arcsec"));   // the backend makes it circular
    QCOMPARE(body.value("pa_deg").toDouble(), 0.0);
    QVERIFY(!body.contains("clear"));

    request.minorArcsec = 12.0;
    request.paDeg = 30.0;
    body = beam::requestBody(request);
    QCOMPARE(body.value("minor_arcsec").toDouble(), 12.0);
    QCOMPARE(body.value("pa_deg").toDouble(), 30.0);

    request.clear = true;
    body = beam::requestBody(request);
    QVERIFY(body.value("clear").toBool());
    QVERIFY(!body.contains("major_arcsec"));
}

void TestMolecularLineClient::theBeamResponseIsParsedInDegrees()
{
    const beam::BeamResult set = beam::parseResponse(QJsonObject{
            { "valid", true },
            { "beam_major", 15.0 / 3600.0 },
            { "beam_minor", 15.0 / 3600.0 },
            { "beam_pa", 0.0 },
            { "beam_source", "user" },
            { "note", "The header has an incomplete beam" } });
    QVERIFY(set.valid);
    QCOMPARE(set.majorDeg, 15.0 / 3600.0);
    QCOMPARE(set.source, QStringLiteral("user"));
    QVERIFY(set.note.startsWith(QStringLiteral("The header")));

    const beam::BeamResult cleared = beam::parseResponse(QJsonObject{
            { "valid", true }, { "beam_major", QJsonValue() }, { "beam_minor", QJsonValue() },
            { "beam_pa", QJsonValue() }, { "beam_source", "none" } });
    QVERIFY(std::isnan(cleared.majorDeg));
    QCOMPARE(cleared.paDeg, 0.0);

    const beam::BeamResult refused = beam::parseResponse(QJsonObject{
            { "valid", false }, { "error", "The header already has a beam" },
            { "beam_source", "header" } });
    QVERIFY(!refused.valid);
    QCOMPARE(refused.error, QStringLiteral("The header already has a beam"));
}

void TestMolecularLineClient::aBeamCanBeTypedOnlyWhenTheHeaderHasNone()
{
    QVERIFY(!beam::canSetBeam(QStringLiteral("header")));
    QVERIFY(beam::canSetBeam(QStringLiteral("none")));
    QVERIFY(beam::canSetBeam(QStringLiteral("user")));
    // No source: an older backend without the route, or a dataset that is not
    // a FITS image or cube (a VBT field, a dynamic spectrum).
    QVERIFY(!beam::canSetBeam(QString()));
}

void TestMolecularLineClient::theBeamSummarySaysWhereItCameFrom()
{
    const double d = 15.0 / 3600.0;
    QCOMPARE(beam::summary(d, d, 0.0, QStringLiteral("user")),
             QStringLiteral("15.0″ × 15.0″, PA 0° (set by user)"));
    QVERIFY(beam::summary(d, d, 0.0, QStringLiteral("header")).endsWith(QStringLiteral("(header)")));
    QCOMPARE(beam::summary(std::nan(""), std::nan(""), 0.0, QStringLiteral("none")),
             QStringLiteral("none in the header"));
}

void TestMolecularLineClient::theInferredUnitIsPreferredAndSaidOnTheAxis()
{
    // CHIMPS: no BUNIT, TEMPSCAL = 'TA*'.
    const QString note = QStringLiteral("Source has no BUNIT: its intensities were taken as K "
                                        "on the TA* scale (TEMPSCAL), not converted.");
    QCOMPARE(beam::intensityUnit(QString(), QStringLiteral("K")), QStringLiteral("K"));
    QCOMPARE(beam::intensityUnit(QStringLiteral("Jy/beam"), QString()), QStringLiteral("Jy/beam"));
    QCOMPARE(beam::intensityAxisLabel(QStringLiteral("K"), note),
             QStringLiteral("Intensity (K, no BUNIT)"));
    QCOMPARE(beam::intensityAxisLabel(QStringLiteral("Jy/beam"), QString()),
             QStringLiteral("Intensity (Jy/beam)"));
    QCOMPARE(beam::intensityAxisLabel(QString(), QStringLiteral("unknown")),
             QStringLiteral("Intensity (unit unknown)"));
    QCOMPARE(beam::unitDisplay(QStringLiteral("K"), note), QStringLiteral("K (no BUNIT)"));
    QCOMPARE(beam::unitDisplay(QString(), QString()), QStringLiteral("-"));
}

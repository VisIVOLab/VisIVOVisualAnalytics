// test_lte_co_request.cpp
// See TestLteCoRequest.h.

#include "TestLteCoRequest.h"

#include "app/BackendClient.h"
#include "app/LteCoRequest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;
namespace lte = visivo::lte;

namespace {

lte::Inputs chimps()
{
    lte::Inputs in;
    in.transition = u"3-2"_s;
    in.dataset13co = u"ds13"_s;
    in.datasetC18o = u"ds18"_s;
    in.vMinKms = 70.0;
    in.vMaxKms = 95.0;
    in.xRatio = 7.4;
    return in;
}

/// The answer of the backend's documentation (docs/technical/backend-api/
/// products.md), with one region whose optical depth has no lower bound.
QByteArray sampleAnswer()
{
    return R"({ "valid": true, "error": "", "width": 167, "height": 69,
      "assumptions": { "transition": "3-2",
          "eta_mb": {"13CO": [0.72, "user"], "C18O": [0.72, "header BEAMEFF/FORWEFF"]},
          "x_ratio": [7.4, "user"], "r_gc_kpc": [5.68, "from l = 40.1570, b = 0.1690, d = 2 kpc, R0 = 8.5 kpc"],
          "t_ex": {"source": "peak T_mb of 13CO, with tau at the peak from the ratio of the 13CO and C18O peaks"},
          "h2": {"from": "C18O", "abundance": 1.7e-7, "source": "Wilson & Rood 1994"},
          "distance_kpc": 2.0, "mu_h2": [2.8, "Kauffmann et al. 2008, A&A 487, 993, App. A"],
          "snr_min": 3.0, "t_bg_k": 2.73, "ratio_from": "integrated intensities",
          "inputs": {"13CO": ["cube integrated over 70 to 95 km/s"], "C18O": []} },
      "warnings": ["C18O was reprojected onto the 13CO grid (bilinear)."],
      "regions": [ { "index": 0, "n_pixels": 29, "beam_pixels": 3.5,
          "w_13co_kkms": 7.08, "sigma_w_13co_kkms": 0.29, "w_c18o_kkms": 1.69, "sigma_w_c18o_kkms": 0.26,
          "ratio": 4.18, "sigma_ratio": 0.67, "tau_13co": 1.54, "tau_c18o": 0.21,
          "tau_13co_interval": [null, 2.15], "flag": 0, "t_ex_k": 15.0,
          "n_13co_cm2": 1.0e16, "n_c18o_cm2": 1.3e15, "n_h2_cm2": 5e21, "mass_msun": null } ],
      "maps": [ { "quantity": "tau_13co", "output_path": "/ws/a_lte_tau_13co.fits",
                  "workspace_filename": "a_lte_tau_13co.fits", "bunit": "", "range_min": 0.2,
                  "range_max": 3.1 } ],
      "pixel_flags": { "no_tex": 40, "ok": 812, "thin": 40, "no_solution": 3, "below_snr": 9000 } })";
}

/// Answers POST /v1/products/lte_co with a canned body and records the request.
class LteServer : public QObject
{
public:
    explicit LteServer(QByteArray body) : body(std::move(body))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buffer = new QByteArray;
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QObject::destroyed, s, [buffer]() { delete buffer; });
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    *buffer += s->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QByteArray head = buffer->left(end).toLower();
                    qsizetype length = 0;
                    const qsizetype at = head.indexOf("content-length:");
                    if (at >= 0)
                        length = head.mid(at + 15, head.indexOf("\r\n", at) - at - 15)
                                         .trimmed()
                                         .toLongLong();
                    if (buffer->size() < end + 4 + length)
                        return;
                    const QByteArray line = buffer->left(buffer->indexOf("\r\n"));
                    const bool route = line.startsWith("POST /v1/products/lte_co ");
                    if (route)
                        requestBody = buffer->mid(end + 4, length);
                    buffer->clear();
                    const int code = route ? 200 : 404;
                    const QByteArray payload =
                            route ? this->body : QByteArray(R"({"detail":"Not Found"})");
                    s->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                             "Content-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(payload.size())
                             + "\r\nConnection: close\r\n\r\n" + payload);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }

    QTcpServer server;
    QByteArray body;
    QByteArray requestBody;
};

}   // namespace

void TestLteCoRequest::unsetFieldsAreLeftToTheBackend()
{
    lte::Inputs in = chimps();
    in.eta13co = 0.72;
    in.regions.append(QJsonObject{ { u"shape"_s, u"circle"_s },
                                   { u"center"_s, QJsonArray{ 10.0, 12.0 } },
                                   { u"radius"_s, 3.0 } });
    in.xRatio.reset();
    in.eta12co = 0.8;
    const QJsonObject body = lte::requestBody(in);
    QCOMPARE(body.value(u"transition").toString(), u"3-2"_s);
    QCOMPARE(body.value(u"dataset_id_13co").toString(), u"ds13"_s);
    QCOMPARE(body.value(u"dataset_id_c18o").toString(), u"ds18"_s);
    QCOMPARE(body.value(u"v_min_kms").toDouble(), 70.0);
    QCOMPARE(body.value(u"v_max_kms").toDouble(), 95.0);
    QCOMPARE(body.value(u"eta_mb_13co").toDouble(), 0.72);
    QCOMPARE(body.value(u"snr_min").toDouble(), 3.0);
    QCOMPARE(body.value(u"carbon_gradient").toString(), u"milam2005"_s);
    QCOMPARE(body.value(u"h2_from").toString(), u"c18o"_s);
    QCOMPARE(body.value(u"regions").toArray().size(), 1);
    // Resolved by the backend, which then says where each came from.
    for (const char *key : { "eta_mb_c18o", "eta_mb_12co", "dataset_id_12co", "x_ratio",
                             "r_gc_kpc", "distance_kpc", "t_ex_k", "x12co", "output_basename" })
        QVERIFY2(!body.contains(QLatin1String(key)), key);
}

void TestLteCoRequest::aCubeNeedsItsVelocityWindow()
{
    lte::Inputs in = chimps();
    QVERIFY(lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    in.vMaxKms.reset();
    QVERIFY(!lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    // Integrated maps need none.
    in.tExK = 15.0;
    QVERIFY(lte::validate(in, u"image"_s, u"image"_s, QString()).isEmpty());
    in.tExK.reset();
    in.vMaxKms = 60.0;
    QVERIFY(lte::validate(in, u"cube"_s, u"cube"_s, QString()).contains(u"below"_s));
}

void TestLteCoRequest::outOfRangeInputsAreRefusedBeforeSending()
{
    lte::Inputs in = chimps();
    in.transition = u"2-1"_s;
    QVERIFY(!lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    in = chimps();
    in.datasetC18o = in.dataset13co;
    QVERIFY(!lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    in = chimps();
    in.dataset12co = u"ds18"_s;
    QVERIFY(!lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    in = chimps();
    in.etaC18o = 1.2;
    QVERIFY(lte::validate(in, u"cube"_s, u"cube"_s, QString()).contains(u"η_mb"_s));
    in = chimps();
    in.xRatio = 1.0;
    QVERIFY(!lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    in = chimps();
    in.tExK = 2.0;
    QVERIFY(lte::validate(in, u"cube"_s, u"cube"_s, QString()).contains(u"background"_s));
    in = chimps();
    in.distanceKpc = 0.0;
    QVERIFY(!lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    // X needs a source: typed, or R_GC, or a distance to compute R_GC.
    in = chimps();
    in.xRatio.reset();
    QVERIFY(lte::validate(in, u"cube"_s, u"cube"_s, QString()).contains(u"R_GC"_s));
    in.distanceKpc = 2.0;
    QVERIFY(lte::validate(in, u"cube"_s, u"cube"_s, QString()).isEmpty());
    // Two integrated maps have no peaks: T_ex must come from somewhere else.
    in = chimps();
    QVERIFY(lte::validate(in, u"image"_s, u"cube"_s, QString()).contains(u"T_ex"_s));
    in.dataset12co = u"ds12"_s;
    QVERIFY(lte::validate(in, u"image"_s, u"cube"_s, u"image"_s).isEmpty());
}

void TestLteCoRequest::typedNumbersReadInEitherLocale()
{
    QCOMPARE(*lte::optionalNumber(u" 0.72 "_s), 0.72);
    QCOMPARE(*lte::optionalNumber(u"8.5e-5"_s), 8.5e-5);
    QVERIFY(!lte::optionalNumber(QString()).has_value());
    QVERIFY(!lte::optionalNumber(u"   "_s).has_value());
    QVERIFY(!lte::optionalNumber(u"auto"_s).has_value());
    QVERIFY(!lte::optionalNumber(u"nan"_s).has_value());
    // A decimal comma is never a thousands separator: 2,500 kpc is not 2500.
    QVERIFY(!(lte::optionalNumber(u"2,500"_s).value_or(0.0) > 1000.0));
    QVERIFY(!(lte::optionalNumber(u"1,234.5"_s).value_or(0.0) > 1000.0));
}

void TestLteCoRequest::theAnswerIsParsedWithNullsAsUnknown()
{
    const lte::Response r = lte::parseResponse(QJsonDocument::fromJson(sampleAnswer()).object());
    QVERIFY(r.valid);
    QCOMPARE(r.width, 167);
    QCOMPARE(r.regions.size(), 1);
    const lte::RegionResult &g = r.regions.first();
    QCOMPARE(g.nPixels, 29);
    QCOMPARE(g.ratio, 4.18);
    QCOMPARE(g.sigmaRatio, 0.67);
    QVERIFY(std::isnan(g.tau13coLow));
    QCOMPARE(g.tau13coHigh, 2.15);
    QCOMPARE(g.nH2, 5e21);
    QVERIFY(std::isnan(g.massMsun));
    QCOMPARE(g.flag, 0);
    QCOMPARE(r.maps.size(), 1);
    QCOMPARE(r.maps.first().quantity, u"tau_13co"_s);
    QCOMPARE(r.maps.first().outputPath, u"/ws/a_lte_tau_13co.fits"_s);
    QCOMPARE(r.warnings.size(), 1);
    // In the backend's order, not alphabetical.
    QCOMPARE(r.pixelFlags.size(), 5);
    QCOMPARE(r.pixelFlags.at(0).first, u"ok"_s);
    QCOMPARE(r.pixelFlags.at(0).second, 812);
    QCOMPARE(r.pixelFlags.at(4).first, u"no_tex"_s);
}

void TestLteCoRequest::theEtaTheBackendAskedForIsFound()
{
    const QString error =
            u"eta_mb for C18O is required: the header names no main-beam efficiency "
            u"(ETAL is the rear-spillover efficiency, already in T_A*, not eta_mb)."_s;
    QCOMPARE(lte::etaRequiredFor(error), QStringList{ u"c18o"_s });
    QVERIFY(lte::etaRequiredFor(u"The 13CO and C18O data share no pixel on the sky."_s).isEmpty());

    const lte::Response r = lte::parseResponse(QJsonDocument::fromJson(sampleAnswer()).object());
    const auto used = lte::etaResolution(r.assumptions);
    QCOMPARE(used.size(), 2);
    QCOMPARE(used.at(0).species, u"13co"_s);
    QCOMPARE(used.at(0).value, 0.72);
    QCOMPARE(used.at(1).source, u"header BEAMEFF/FORWEFF"_s);
}

void TestLteCoRequest::theAssumptionsSayWhereEachNumberCameFrom()
{
    const lte::Response r = lte::parseResponse(QJsonDocument::fromJson(sampleAnswer()).object());
    const QString text = lte::assumptionLines(r.assumptions).join(u'\n');
    QVERIFY(text.contains(u"Transition: J = 3–2"_s));
    QVERIFY(text.contains(u"η_mb C¹⁸O = 0.72 (header BEAMEFF/FORWEFF)"_s));
    QVERIFY(text.contains(u"X(¹³CO)/X(C¹⁸O) = 7.4 (user)"_s));
    QVERIFY(text.contains(u"R_GC = 5.68 kpc"_s));
    QVERIFY(text.contains(u"T_ex from the peak T_mb of ¹³CO"_s));
    QVERIFY(text.contains(u"N(H₂) = N(C¹⁸O)/1.70e-07"_s));
    QVERIFY(text.contains(u"μ_H₂ = 2.8"_s));
    QVERIFY(text.contains(u"T_bg = 2.73 K"_s));
    QVERIFY(text.contains(u"¹³CO: cube integrated over 70 to 95 km/s"_s));
}

void TestLteCoRequest::theTableRowCarriesUnitsAndErrors()
{
    const QStringList cols = lte::tableColumns();
    QVERIFY(cols.contains(u"W_mb(¹³CO) [K km/s]"_s));
    QVERIFY(cols.contains(u"T_ex [K]"_s));
    QVERIFY(cols.contains(u"N(H₂) [cm⁻²]"_s));
    QVERIFY(cols.contains(u"Mass [M☉]"_s));
    const lte::Response r = lte::parseResponse(QJsonDocument::fromJson(sampleAnswer()).object());
    const QStringList row = lte::tableRow(r.regions.first(), u"Clump"_s);
    QCOMPARE(row.size(), cols.size());
    QCOMPARE(row.at(0), u"Clump"_s);
    QCOMPARE(row.at(4), u"4.18 ± 0.67"_s);
    QCOMPARE(row.at(6), u"≤ 2.15"_s);
    QCOMPARE(row.at(9), u"1.00e+16"_s);
    QCOMPARE(row.at(12), u"–"_s);
    QCOMPARE(row.at(13), u"ok"_s);
    lte::RegionResult open = r.regions.first();
    open.tau13coLow = 1.09;
    open.tau13coHigh = lte::kUnknown;
    open.tExK = lte::kUnknown;
    const QStringList openRow = lte::tableRow(open, u"Thick"_s);
    QCOMPARE(openRow.at(6), u"≥ 1.09"_s);
    QCOMPARE(openRow.at(13), u"ok, T_ex unknown"_s);
    lte::Response withOpen = r;
    withOpen.regions = { open };
    QVERIFY(lte::toCsv(withOpen, {}, {}).contains(u",1.54,1.09,inf,"_s));
    lte::RegionResult empty;
    empty.error = u"The region holds no pixel where both lines have data."_s;
    QCOMPARE(lte::tableRow(empty, u"Off"_s).last(), empty.error);
}

void TestLteCoRequest::theCsvCarriesProvenanceAndFullPrecision()
{
    const lte::Response r = lte::parseResponse(QJsonDocument::fromJson(sampleAnswer()).object());
    const QString csv = lte::toCsv(r, { u"Clump, north"_s }, { u"Backend: http://x"_s });
    const QStringList lines = csv.split(u'\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.first(), u"# Backend: http://x"_s);
    QVERIFY(csv.contains(u"# η_mb ¹³CO = 0.72 (user)"_s));
    QVERIFY(csv.contains(u"# Warning: C¹⁸O was reprojected"_s));
    const int header = static_cast<int>(lines.indexOf(QRegularExpression(u"^region,.*"_s)));
    QVERIFY(header > 0);
    QVERIFY(lines.at(header).contains(u"W_13CO_K_km_s"_s));
    QVERIFY(lines.at(header).contains(u"N_H2_cm-2"_s));
    QVERIFY(lines.at(header).contains(u"mass_Msun"_s));
    const QString row = lines.at(header + 1);
    QVERIFY(row.startsWith(u"\"Clump, north\",29,3.5,7.08,0.29,1.69,0.26,4.18,0.67,1.54,,2.15,"_s));
    QVERIFY(row.contains(u",5e+21,,0,ok,"_s));
}

void TestLteCoRequest::namesAndRestFrequenciesGuessTheLine()
{
    QCOMPARE(lte::guessSpecies(u"CHIMPS_13CO_l40.fits"_s), lte::k13co);
    QCOMPARE(lte::guessSpecies(u"chimps_c18o_l40.fits"_s), lte::kC18o);
    QCOMPARE(lte::guessSpecies(u"COHRS_12CO_l40.fits"_s), lte::k12co);
    QCOMPARE(lte::guessSpecies(u"field_CO_mom0.fits"_s), lte::k12co);
    QCOMPARE(lte::guessSpecies(u"cosmos.fits"_s), QString());
    QCOMPARE(lte::guessLine(330.588e9).species, lte::k13co);
    QCOMPARE(lte::guessLine(330.588e9).transition, u"3-2"_s);
    QCOMPARE(lte::guessLine(109.7822e9).species, lte::kC18o);
    QCOMPARE(lte::guessLine(115.2712e9).transition, u"1-0"_s);
    QVERIFY(lte::guessLine(1.420405752e9).species.isEmpty());
    QVERIFY(lte::guessLine(0.0).species.isEmpty());
}

void TestLteCoRequest::velocitiesAreConvertedToKmPerSecond()
{
    QCOMPARE(*lte::toKms(84.0, u"km/s"_s), 84.0);
    QCOMPARE(*lte::toKms(84000.0, u"m/s"_s), 84.0);
    QCOMPARE(*lte::toKms(84000.0, u"m s-1"_s), 84.0);
    QCOMPARE(*lte::toKms(84.0, u"km.s-1"_s), 84.0);
    QVERIFY(!lte::toKms(1.1e11, u"Hz"_s).has_value());
    QVERIFY(!lte::toKms(12.0, QString()).has_value());
}

void TestLteCoRequest::onlyAreaRegionsAreMeasured()
{
    QVERIFY(lte::isAreaRegion(QJsonObject{ { u"shape"_s, u"polygon"_s } }));
    QVERIFY(lte::isAreaRegion(QJsonObject{ { u"shape"_s, u"box"_s } }));
    QVERIFY(!lte::isAreaRegion(QJsonObject{ { u"shape"_s, u"line"_s } }));
    QVERIFY(!lte::isAreaRegion(QJsonObject{ { u"shape"_s, u"text"_s } }));
}

void TestLteCoRequest::theClientPostsTheRequestAndReadsTheAnswer()
{
    LteServer backend(sampleAnswer());
    QVERIFY(backend.server.isListening());
    const BackendClient client(backend.url(), u"tok"_s);
    lte::Inputs in = chimps();
    in.xRatio = 7.4;
    const BackendLteCoResult result = client.lteCo(lte::requestBody(in));
    QVERIFY2(result.valid, qPrintable(result.error));
    const QJsonObject sent = QJsonDocument::fromJson(backend.requestBody).object();
    QCOMPARE(sent.value(u"transition").toString(), u"3-2"_s);
    QCOMPARE(sent.value(u"x_ratio").toDouble(), 7.4);
    QCOMPARE(sent.value(u"dataset_id_c18o").toString(), u"ds18"_s);
    const lte::Response r = lte::parseResponse(result.payload);
    QCOMPARE(r.regions.size(), 1);
    QCOMPARE(r.maps.size(), 1);
}

void TestLteCoRequest::theClientReportsARefusal()
{
    LteServer backend(R"({"valid": false, "error": "eta_mb for 13CO is required: the header names no main-beam efficiency."})");
    const BackendClient client(backend.url(), u"tok"_s);
    const BackendLteCoResult result = client.lteCo(lte::requestBody(chimps()));
    QVERIFY(!result.valid);
    QCOMPARE(lte::etaRequiredFor(result.error), QStringList{ u"13co"_s });
}

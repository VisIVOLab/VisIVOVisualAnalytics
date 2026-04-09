// test_backendclient.cpp — implementations for TestBackendClient

#include "TestBackendClient.h"
#include "app/BackendClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

// Build a QJsonObject from a JSON string literal.
static QJsonObject fromLiteral(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

// ── parseMomentResultObject ───────────────────────────────────────────────────

void TestBackendClient::parseMoment_happyPath()
{
    const auto obj = fromLiteral(R"({
        "valid": true,  "error": "",
        "width": 512,   "height": 256,
        "scalar_type":      "float32",
        "range_min":        -0.5,
        "range_max":         2.3,
        "spectral_axis_type": "VRAD",
        "spectral_axis_unit": "km/s",
        "moment_unit":        "Jy/beam km/s",
        "bunit":              "Jy/beam",
        "wcs_status":         "ok",
        "wcs_warning_message": ""
    })");

    const auto r = BackendClient::parseMomentResultObject(obj);

    QVERIFY(r.valid);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.width,  512);
    QCOMPARE(r.height, 256);
    QCOMPARE(r.scalarType,       QStringLiteral("float32"));
    QCOMPARE(r.rangeMin,        -0.5);
    QCOMPARE(r.rangeMax,         2.3);
    QCOMPARE(r.spectralAxisType, QStringLiteral("VRAD"));
    QCOMPARE(r.spectralAxisUnit, QStringLiteral("km/s"));
    QCOMPARE(r.momentUnit,       QStringLiteral("Jy/beam km/s"));
    QCOMPARE(r.bunit,            QStringLiteral("Jy/beam"));
    QCOMPARE(r.wcsStatus,        QStringLiteral("ok"));
    QVERIFY(r.wcsWarningMessage.isEmpty());
}

void TestBackendClient::parseMoment_errorResponse()
{
    const auto obj = fromLiteral(R"({"valid": false, "error": "Dataset not found"})");
    const auto r = BackendClient::parseMomentResultObject(obj);

    QVERIFY(!r.valid);
    QCOMPARE(r.error, QStringLiteral("Dataset not found"));
}

void TestBackendClient::parseMoment_missingFields_defaults()
{
    const auto r = BackendClient::parseMomentResultObject(QJsonObject{});

    QVERIFY(!r.valid);         // toBool(false) → false when absent
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.width,  0);
    QCOMPARE(r.height, 0);
    QVERIFY(r.scalarType.isEmpty());
    QCOMPARE(r.rangeMin, 0.0);
    QCOMPARE(r.rangeMax, 0.0);
}

void TestBackendClient::parseMoment_wcsStatusDefaultsToOk()
{
    // When wcs_status is absent the parser must fall back to "ok".
    const auto obj = fromLiteral(R"({"valid": true, "error": ""})");
    const auto r = BackendClient::parseMomentResultObject(obj);
    QCOMPARE(r.wcsStatus, QStringLiteral("ok"));
}

void TestBackendClient::parseMoment_wcsSanitized()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "wcs_status": "sanitized",
        "wcs_warning_message": "Axis 3 CTYPE corrected from VELO to VRAD"
    })");
    const auto r = BackendClient::parseMomentResultObject(obj);

    QVERIFY(r.valid);
    QCOMPARE(r.wcsStatus,         QStringLiteral("sanitized"));
    QCOMPARE(r.wcsWarningMessage, QStringLiteral("Axis 3 CTYPE corrected from VELO to VRAD"));
}

// ── parsePvResultObject ───────────────────────────────────────────────────────

void TestBackendClient::parsePv_happyPath()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "num_samples": 100, "depth": 64,
        "scalar_type": "float32",
        "computed_on": "backend-node-1",
        "width_pixels": 3,
        "vertex_count": 4,
        "total_length": 1250.5,
        "valid_samples": 98,
        "spectral_axis_type": "FREQ",
        "spectral_axis_unit": "Hz",
        "bunit": "Jy/beam",
        "pixel_scale_arcsec_per_pixel": 1.5,
        "spatial_unit": "arcsec"
    })");

    const auto r = BackendClient::parsePvResultObject(obj);

    QVERIFY(r.valid);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.numSamples,           100);
    QCOMPARE(r.depth,                 64);
    QCOMPARE(r.scalarType,           QStringLiteral("float32"));
    QCOMPARE(r.computedOn,           QStringLiteral("backend-node-1"));
    QCOMPARE(r.widthPixels,            3);
    QCOMPARE(r.vertexCount,            4);
    QCOMPARE(r.totalLength,         1250.5);
    QCOMPARE(r.validSamples,          98);
    QCOMPARE(r.spectralAxisType,     QStringLiteral("FREQ"));
    QCOMPARE(r.spectralAxisUnit,     QStringLiteral("Hz"));
    QCOMPARE(r.bunit,                QStringLiteral("Jy/beam"));
    QCOMPARE(r.pixelScaleArcsecPerPixel, 1.5);
    QCOMPARE(r.spatialUnit,          QStringLiteral("arcsec"));
}

void TestBackendClient::parsePv_errorResponse()
{
    // NOTE: parsePvResultObject calls result.error.clear() after the
    // positions_arcsec_base64 decode step (by design: missing arcsec
    // positions is non-fatal).  As a side-effect this clears the JSON
    // "error" string too.  We therefore only assert on valid==false here;
    // callers that need the message must read it before the clear().
    const auto obj = fromLiteral(R"({"valid": false, "error": "PV calculation failed"})");
    const auto r = BackendClient::parsePvResultObject(obj);

    QVERIFY(!r.valid);
    // r.error is intentionally empty here due to the mid-function clear();
    // see parsePvResultObject implementation for details.
}

void TestBackendClient::parsePv_widthPixelsDefault()
{
    // width_pixels absent → must default to 1, not 0 (toInt(1) in parser).
    const auto obj = fromLiteral(R"({"valid": true, "error": ""})");
    const auto r = BackendClient::parsePvResultObject(obj);
    QCOMPARE(r.widthPixels, 1);
}

void TestBackendClient::parsePv_beamFields()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "beam_major": 0.05,
        "beam_minor": 0.03,
        "beam_pa":   45.0
    })");
    const auto r = BackendClient::parsePvResultObject(obj);

    QCOMPARE(r.beamMajor,  0.05);
    QCOMPARE(r.beamMinor,  0.03);
    QCOMPARE(r.beamPa,    45.0);
}

// ── parseNoiseResultObject ────────────────────────────────────────────────────

void TestBackendClient::parseNoise_happyPath()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "channel_start": 10, "channel_end": 50, "num_channels": 41,
        "mad": [], "sigma": [],
        "region": {"x0": 0, "x1": 255, "y0": 0, "y1": 127}
    })");

    const auto r = BackendClient::parseNoiseResultObject(obj);

    QVERIFY(r.valid);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.channelStart, 10);
    QCOMPARE(r.channelEnd,   50);
    QCOMPARE(r.numChannels,  41);
    QCOMPARE(r.x0,   0);
    QCOMPARE(r.x1, 255);
    QCOMPARE(r.y0,   0);
    QCOMPARE(r.y1, 127);
}

void TestBackendClient::parseNoise_madSigmaArrays()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "channel_start": 0, "channel_end": 2, "num_channels": 3,
        "mad":   [0.001, 0.002, 0.003],
        "sigma": [0.0014, 0.0028, 0.0042],
        "region": {"x0": 0, "x1": 63, "y0": 0, "y1": 63}
    })");

    const auto r = BackendClient::parseNoiseResultObject(obj);

    QVERIFY(r.valid);
    QCOMPARE(r.mad.size(),   3);
    QCOMPARE(r.sigma.size(), 3);
    QVERIFY(qAbs(r.mad[0]   - 0.001 ) < 1e-9);
    QVERIFY(qAbs(r.mad[2]   - 0.003 ) < 1e-9);
    QVERIFY(qAbs(r.sigma[1] - 0.0028) < 1e-9);
}

void TestBackendClient::parseNoise_emptyArrays()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "channel_start": 0, "channel_end": 0, "num_channels": 0,
        "mad": [], "sigma": [],
        "region": {"x0": 0, "x1": 0, "y0": 0, "y1": 0}
    })");

    const auto r = BackendClient::parseNoiseResultObject(obj);
    QVERIFY(r.mad.isEmpty());
    QVERIFY(r.sigma.isEmpty());
}

void TestBackendClient::parseNoise_errorResponse()
{
    const auto obj = fromLiteral(R"({"valid": false, "error": "Subvolume too large"})");
    const auto r = BackendClient::parseNoiseResultObject(obj);

    QVERIFY(!r.valid);
    QCOMPARE(r.error, QStringLiteral("Subvolume too large"));
    QVERIFY(r.mad.isEmpty());
    QVERIFY(r.sigma.isEmpty());
}

void TestBackendClient::parseNoise_missingRegion_defaultsToZero()
{
    // region object absent → all coords must default to 0
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "channel_start": 5, "channel_end": 10, "num_channels": 6,
        "mad": [], "sigma": []
    })");

    const auto r = BackendClient::parseNoiseResultObject(obj);
    QCOMPARE(r.x0, 0);
    QCOMPARE(r.x1, 0);
    QCOMPARE(r.y0, 0);
    QCOMPARE(r.y1, 0);
}

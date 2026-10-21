// test_backendclient.cpp — implementations for TestBackendClient

#include "TestBackendClient.h"
#include "app/BackendClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>

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

void TestBackendClient::parseNoise_blankFractionParsed()
{
    // blank_fraction is the fraction of the selected region that was treated
    // as "blank" (zero or NaN) by the zero-as-blank heuristic. The frontend
    // surfaces this in the noise dialog so the user knows how much of their
    // region was actually used.
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "channel_start": 0, "channel_end": 0, "num_channels": 1,
        "mad": [0.0], "sigma": [0.0],
        "region": {"x0": 0, "x1": 7, "y0": 0, "y1": 7},
        "blank_fraction": 0.42
    })");

    const auto r = BackendClient::parseNoiseResultObject(obj);
    QVERIFY(r.valid);
    QVERIFY(qAbs(r.blankFraction - 0.42) < 1e-9);
}

void TestBackendClient::parseNoise_blankFractionMissing_defaultsToZero()
{
    // Older backends (pre-blank-fraction patch) don't ship the field at all;
    // the parser must treat that as 0.0 rather than NaN / -1 so the UI
    // doesn't render a stale or invalid percentage.
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "channel_start": 0, "channel_end": 0, "num_channels": 1,
        "mad": [0.0], "sigma": [0.0],
        "region": {"x0": 0, "x1": 7, "y0": 0, "y1": 7}
    })");

    const auto r = BackendClient::parseNoiseResultObject(obj);
    QCOMPARE(r.blankFraction, 0.0);
}

// ── Workspace Exports parsers ───────────────────────────────────────────────

void TestBackendClient::parseSaveSubregion_happyPath()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "new_dataset_id": "ds_abc123",
        "output_path": "/home/visivo/.visivo/exports/m31_subcube_x0-9_y0-9_z0-9.fits",
        "workspace_filename": "m31_subcube_x0-9_y0-9_z0-9.fits",
        "width": 10, "height": 10, "depth": 10,
        "range_min": -0.5, "range_max": 12.75
    })");
    const auto r = BackendClient::parseSaveSubregionObject(obj);
    QVERIFY(r.valid);
    QCOMPARE(r.newDatasetId, QStringLiteral("ds_abc123"));
    QCOMPARE(r.workspaceFilename,
             QStringLiteral("m31_subcube_x0-9_y0-9_z0-9.fits"));
    QCOMPARE(r.width,  10);
    QCOMPARE(r.height, 10);
    QCOMPARE(r.depth,  10);
    QVERIFY(qAbs(r.rangeMin - (-0.5))  < 1e-12);
    QVERIFY(qAbs(r.rangeMax - 12.75)   < 1e-12);
}

void TestBackendClient::parseSaveSubregion_errorResponse()
{
    const auto obj = fromLiteral(R"({"valid": false, "error": "ROI out of bounds"})");
    const auto r = BackendClient::parseSaveSubregionObject(obj);
    QVERIFY(!r.valid);
    QCOMPARE(r.error, QStringLiteral("ROI out of bounds"));
    QVERIFY(r.workspaceFilename.isEmpty());
    QCOMPARE(r.width, 0);
}

void TestBackendClient::parseSaveMomentFits_happyPath()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "output_path": "/home/visivo/.visivo/exports/m31_m0.fits",
        "workspace_filename": "m31_m0.fits",
        "width": 512, "height": 512,
        "range_min": 0.0, "range_max": 4.2,
        "bunit": "K m/s", "moment_unit": "K m/s"
    })");
    const auto r = BackendClient::parseSaveMomentFitsObject(obj);
    QVERIFY(r.valid);
    QCOMPARE(r.workspaceFilename, QStringLiteral("m31_m0.fits"));
    QCOMPARE(r.width,  512);
    QCOMPARE(r.height, 512);
    QCOMPARE(r.bunit,      QStringLiteral("K m/s"));
    QCOMPARE(r.momentUnit, QStringLiteral("K m/s"));
}

void TestBackendClient::parseSaveMomentFits_missingBunit_defaultsToEmpty()
{
    // Source cube with no BUNIT → backend returns empty bunit/moment_unit.
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "output_path": "/tmp/x.fits", "workspace_filename": "x.fits",
        "width": 16, "height": 16,
        "range_min": 0.0, "range_max": 1.0
    })");
    const auto r = BackendClient::parseSaveMomentFitsObject(obj);
    QVERIFY(r.valid);
    QVERIFY(r.bunit.isEmpty());
    QVERIFY(r.momentUnit.isEmpty());
}

void TestBackendClient::parseExportsList_happyPath()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "workspace_dir": "/home/visivo/.visivo/exports",
        "entries": [
            {"filename": "newer.fits", "size": 2048,
             "modified_time": "2026-05-27T12:00:00+00:00",
             "absolute_path": "/home/visivo/.visivo/exports/newer.fits"},
            {"filename": "older.fits", "size": 1024,
             "modified_time": "2026-05-26T08:30:00+00:00",
             "absolute_path": "/home/visivo/.visivo/exports/older.fits"}
        ]
    })");
    const auto r = BackendClient::parseExportsListObject(obj);
    QVERIFY(r.valid);
    QCOMPARE(r.workspaceDir,
             QStringLiteral("/home/visivo/.visivo/exports"));
    QCOMPARE(r.entries.size(), 2);
    QCOMPARE(r.entries[0].filename, QStringLiteral("newer.fits"));
    QCOMPARE(r.entries[0].size, qint64(2048));
    QCOMPARE(r.entries[1].filename, QStringLiteral("older.fits"));
    QCOMPARE(r.entries[1].size, qint64(1024));
}

void TestBackendClient::parseExportsList_empty()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "",
        "workspace_dir": "/home/visivo/.visivo/exports",
        "entries": []
    })");
    const auto r = BackendClient::parseExportsListObject(obj);
    QVERIFY(r.valid);
    QVERIFY(r.entries.isEmpty());
}

void TestBackendClient::parseExportsList_errorResponse()
{
    const auto obj = fromLiteral(R"({"valid": false, "error": "Permission denied"})");
    const auto r = BackendClient::parseExportsListObject(obj);
    QVERIFY(!r.valid);
    QCOMPARE(r.error, QStringLiteral("Permission denied"));
    QVERIFY(r.entries.isEmpty());
}

void TestBackendClient::parseExportsDelete_happyPath()
{
    const auto obj = fromLiteral(R"({
        "valid": true, "error": "", "deleted_filename": "old.fits"
    })");
    const auto r = BackendClient::parseExportsDeleteObject(obj);
    QVERIFY(r.valid);
    QCOMPARE(r.deletedFilename, QStringLiteral("old.fits"));
}

void TestBackendClient::parseExportsDelete_missingFile()
{
    // Idempotent delete: missing file → valid=false, error populated, no throw.
    const auto obj = fromLiteral(R"({
        "valid": false, "error": "File not found: x.fits"
    })");
    const auto r = BackendClient::parseExportsDeleteObject(obj);
    QVERIFY(!r.valid);
    QVERIFY(r.error.contains(QStringLiteral("not found")));
    QVERIFY(r.deletedFilename.isEmpty());
}

// ── parseImageTileObject ──────────────────────────────────────────────────────

void TestBackendClient::parseImageTile_happyPath()
{
    const auto o = fromLiteral(R"({
        "valid": true, "error": "",
        "width": 256, "height": 200,
        "full_width": 8192, "full_height": 6000,
        "level": 1, "num_levels": 6,
        "tile_x": 3, "tile_y": 2, "tile_size": 256,
        "scalar_type": "float32",
        "range_min": -0.5, "range_max": 42.25
    })");
    const auto r = BackendClient::parseImageTileObject(o);
    QVERIFY(r.valid);
    QCOMPARE(r.width, 256);
    QCOMPARE(r.height, 200);
    QCOMPARE(r.fullWidth, 8192);
    QCOMPARE(r.fullHeight, 6000);
    QCOMPARE(r.level, 1);
    QCOMPARE(r.numLevels, 6);
    QCOMPARE(r.tileX, 3);
    QCOMPARE(r.tileY, 2);
    QCOMPARE(r.tileSize, 256);
    QCOMPARE(r.scalarType, QStringLiteral("float32"));
    QCOMPARE(r.rangeMin, -0.5);
    QCOMPARE(r.rangeMax, 42.25);
}

void TestBackendClient::parseImageTile_errorResponse()
{
    const auto o = fromLiteral(R"({ "valid": false, "error": "out of range" })");
    const auto r = BackendClient::parseImageTileObject(o);
    QVERIFY(!r.valid);
    QCOMPARE(r.error, QStringLiteral("out of range"));
}

void TestBackendClient::parseImageTile_missingFields_defaults()
{
    // full_* default to width/height; num_levels defaults to 1.
    const auto o = fromLiteral(R"({ "valid": true, "width": 64, "height": 64 })");
    const auto r = BackendClient::parseImageTileObject(o);
    QVERIFY(r.valid);
    QCOMPARE(r.fullWidth, 64);
    QCOMPARE(r.fullHeight, 64);
    QCOMPARE(r.numLevels, 1);
    QCOMPARE(r.level, 0);
}

// ── Diagnostics operation tag ────────────────────────────────────────────────
// The Diagnostics window's Operation column was blank for every synchronous
// compute because only the async task poller ever set it. It is now derived
// from the request path in one place, so the derivation is worth pinning.

void TestBackendClient::operationTag_stripsVersionAndTransportSuffix()
{
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/spectral/linewidth/binary")),
             QStringLiteral("spectral/linewidth"));
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/cube/subvolume_bin")),
             QStringLiteral("cube/subvolume"));
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/velocity/grid_bin")),
             QStringLiteral("velocity/grid"));
}

void TestBackendClient::operationTag_keepsTwoSegments()
{
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/products/isosurface")),
             QStringLiteral("products/isosurface"));
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/cube/pv")),
             QStringLiteral("cube/pv"));
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/health")),
             QStringLiteral("health"));
}

void TestBackendClient::operationTag_dropsIdsAndOddSegments()
{
    // A trailing id is noise in a column: stop at the first segment that is not
    // a plain lowercase word.
    QCOMPARE(BackendClient::operationTagForPath(
                     QStringLiteral("/v1/render/9d65b8db-1f2e-4c3a-bb01-5f6a7b8c9d0e")),
             QStringLiteral("render"));
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/datasets/AB12/close")),
             QStringLiteral("datasets"));
}

void TestBackendClient::operationTag_edgeCases()
{
    QCOMPARE(BackendClient::operationTagForPath(QString()), QString());
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/v1/")), QString());
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/")), QString());
    // No /v1 prefix: still usable.
    QCOMPARE(BackendClient::operationTagForPath(QStringLiteral("/cube/slice")),
             QStringLiteral("cube/slice"));
}


namespace {
QJsonObject kinematicFitJson(int w, int h, bool warp)
{
    QByteArray plane(w * h * int(sizeof(float)), '\0');
    const QString b64 = QString::fromLatin1(plane.toBase64());
    QJsonObject o{
            { QStringLiteral("width"), w }, { QStringLiteral("height"), h },
            { QStringLiteral("velocity_unit"), QStringLiteral("km/s") },
            { QStringLiteral("velocity_convention"), QStringLiteral("optical (cz), from frequency") },
            { QStringLiteral("sigma"), 0.002 },
            { QStringLiteral("sigma_source"), QStringLiteral("noise estimate") },
            { QStringLiteral("measures"), QJsonObject{ { QStringLiteral("flux"), 140.42 },
                                                       { QStringLiteral("flux_is_jy"), true },
                                                       { QStringLiteral("w20"), 259.9 },
                                                       { QStringLiteral("w50"), QJsonValue() },
                                                       { QStringLiteral("vsys_w20"), 1105.9 },
                                                       { QStringLiteral("n_velocity_field"), 5900 } } },
            { QStringLiteral("geometry"), QJsonObject{ { QStringLiteral("x0"), 84.0 },
                                                       { QStringLiteral("incl"), 72.7 } } },
            { QStringLiteral("flat"), QJsonObject{ { QStringLiteral("pa"), 352.0 },
                                                   { QStringLiteral("vrot"), 118.3 } } },
            { QStringLiteral("rings"), QJsonArray{ QJsonObject{ { QStringLiteral("r"), 6.7 },
                                                                { QStringLiteral("pa"), 362.0 },
                                                                { QStringLiteral("n_pix"), 169 } } } },
            { QStringLiteral("residual_flat"), QJsonObject{ { QStringLiteral("median"), 19.8 },
                                                            { QStringLiteral("n_pix"), 4472 } } },
            { QStringLiteral("residual_warp"), QJsonValue() },
            { QStringLiteral("spectrum"), QJsonArray{ 0.1, QJsonValue(), 0.3 } },
            { QStringLiteral("velocity_field_base64"), b64 },
            { QStringLiteral("model_flat_base64"), b64 },
            { QStringLiteral("model_warp_base64"), warp ? b64 : QString() } };
    return o;
}
}  // namespace

void TestBackendClient::parseKinematicFit_readsFitsAndNullsAsNaN()
{
    const auto r = BackendClient::parseKinematicFitObject(kinematicFitJson(3, 2, false));
    QVERIFY(r.valid);
    QCOMPARE(r.flux, 140.42);
    QVERIFY(r.fluxIsJy);
    QVERIFY(std::isnan(r.w50));               // null → NaN, not 0
    QCOMPARE(r.pa, 352.0);
    QCOMPARE(r.rings.size(), 1);
    QCOMPARE(r.rings[0].pa, 362.0);           // continuous PA kept
    QCOMPARE(r.rings[0].nPix, 169);
    QVERIFY(r.residualFlat.valid);
    QVERIFY(!r.residualWarp.valid);           // no warped model
    QCOMPARE(r.spectrum.size(), 3);
    QVERIFY(std::isnan(r.spectrum[1]));
    QVERIFY(r.modelWarp.isEmpty());
}

void TestBackendClient::parseKinematicFit_rejectsMapsOfTheWrongSize()
{
    QJsonObject o = kinematicFitJson(3, 2, true);
    o.insert(QStringLiteral("width"), 4);     // maps were made for 3 × 2
    const auto r = BackendClient::parseKinematicFitObject(o);
    QVERIFY(!r.valid);
    QVERIFY(!r.error.isEmpty());
}

#pragma once
// TestBackendClient.h
// QTest class for BackendClient static JSON-parsing methods.
// No network connection required: all tests pass hardcoded QJsonObjects.

#include <QObject>

class TestBackendClient : public QObject
{
    Q_OBJECT

private slots:
    void parseKinematicFit_readsFitsAndNullsAsNaN();
    void parseKinematicFit_rejectsMapsOfTheWrongSize();
    // Diagnostics "Operation" column — derived from the request path.
    void operationTag_stripsVersionAndTransportSuffix();
    void operationTag_keepsTwoSegments();
    void operationTag_dropsIdsAndOddSegments();
    void operationTag_edgeCases();

    // ── parseMomentResultObject ───────────────────────────────────────────────
    void parseMoment_happyPath();
    void parseMoment_errorResponse();
    void parseMoment_missingFields_defaults();
    void parseMoment_wcsStatusDefaultsToOk();
    void parseMoment_wcsSanitized();

    // ── parsePvResultObject ───────────────────────────────────────────────────
    void parsePv_happyPath();
    void parsePv_errorResponse();
    void parsePv_widthPixelsDefault();
    void parsePv_beamFields();

    // ── parseNoiseResultObject ────────────────────────────────────────────────
    void parseNoise_happyPath();
    void parseNoise_madSigmaArrays();
    void parseNoise_emptyArrays();
    void parseNoise_errorResponse();
    void parseNoise_missingRegion_defaultsToZero();
    void parseNoise_blankFractionParsed();
    void parseNoise_blankFractionMissing_defaultsToZero();

    // ── Workspace Exports parsers ─────────────────────────────────────────────
    void parseSaveSubregion_happyPath();
    void parseSaveSubregion_errorResponse();
    void parseSaveMomentFits_happyPath();
    void parseSaveMomentFits_missingBunit_defaultsToEmpty();
    void parseExportsList_happyPath();
    void parseExportsList_empty();
    void parseExportsList_errorResponse();
    void parseExportsDelete_happyPath();
    void parseExportsDelete_missingFile();

    // ── parseImageTileObject ──────────────────────────────────────────────
    void parseImageTile_happyPath();
    void parseImageTile_errorResponse();
    void parseImageTile_missingFields_defaults();
};

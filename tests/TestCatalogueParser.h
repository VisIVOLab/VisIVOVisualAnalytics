#pragma once
// TestCatalogueParser.h
// QTest class for Catalogue3DParser inline helper functions.
// These are header-only functions in Catalogue3DParser.h; no backend needed.

#include <QObject>

class TestCatalogueParser : public QObject
{
    Q_OBJECT

private slots:
    // ── detectedRedshiftField ─────────────────────────────────────────────────
    void redshift_lowercase_z();
    void redshift_uppercase_REDSHIFT();
    void redshift_ZSPEC();
    void redshift_ZMEAN();
    void redshift_ZPHOT();
    void redshift_absent();
    void redshift_caseInsensitive_mixed();
    void redshift_multipleAliases_firstInAliasListWins();
    void redshift_unrelated_fields_notMatched();

    // ── detectedDistanceField ─────────────────────────────────────────────────
    void distance_DISTANCE();
    void distance_DIST();
    void distance_DMPC();
    void distance_absent();

    // ── comovingDistanceMpc ───────────────────────────────────────────────────
    void comoving_zeroReturnsZero();
    void comoving_negativeReturnsZero();
    void comoving_z01_reasonable();
    void comoving_z1_reasonable();
    void comoving_z01_gt_z005();

    // ── entryDistanceMpc ──────────────────────────────────────────────────────
    void entryDist_fromDistanceField();
    void entryDist_fromRedshiftField();
    void entryDist_noDistance_isUnplaced();
    void entryDist_positionsOnly_onTheSkySphere();
    void entryDist_measuredDistance_beatsModelOverride();
    void entryDist_modelOverride_placesRedshiftOnlySources();
    void entryDist_negativeDistanceField_fallsBackToRedshift();
    void entryDist_zeroRedshift_isUnplaced();
    void entryDist_negativeRedshift_isUnplaced();
    void entryDist_negativeRedshift_withDistanceColumn_isPlaced();
    void entryDist_lowCz_isFlaggedUncertain();
    void entryDist_czBoundary_comparesTheCzTheFileShows();

    // ── issue #265: unplaced rows, the status line, the pick readout ──────────
    void parse_unplacedRowsAreKeptApart();
    void parse_onlyUnplacedRows_isValidButSaysWhy();
    void parse_positionsOnly_allOnTheSkySphere();
    void placement_statusLineCounts();
    void placement_pickLinesNameTheOrigin();
    void placement_unplacedRows();
    void placement_headsetSkyRefusesCustom();

    // ── distance-type (comoving / luminosity / angular-diameter) ──────────────
    void distanceKind_DL_isLuminosity();
    void distanceKind_DA_isAngularDiameter();
    void distanceKind_generic_isComoving();
    void entryDist_luminosityConvertedToComoving();
    void entryDist_angularDiameterConvertedToComoving();
    void entryDist_luminosityWithoutRedshift_usedAsIs();

    // ── R-14 redshift-space distance mode (naive cz/H0) ───────────────────────
    void redshiftSpaceMode_usesHubbleDistance();
    void redshiftSpaceMode_bypassesDistanceOverride();
    void redshiftSpaceMode_noRedshift_fallsBackToRealSpace();
    void redshiftSpaceMode_usesTheModelsH0();
};

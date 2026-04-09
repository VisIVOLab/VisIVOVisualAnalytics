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
    void entryDist_fallback300Mpc();
    void entryDist_distanceMpcOverride_takesPriority();
    void entryDist_negativeDistanceField_fallsBackToRedshift();
    void entryDist_zeroRedshift_fallsBackTo300();
};

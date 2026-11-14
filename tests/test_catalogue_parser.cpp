// test_catalogue_parser.cpp — implementations for TestCatalogueParser

#include "TestCatalogueParser.h"
#include "gui/Catalogue3DParser.h"
#include "gui/CataloguePlacement.h"
#include "app/CosmologyModel.h"

#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Catalogue3DSchema makeSchema(const QStringList &fieldNames)
{
    Catalogue3DSchema schema;
    for (const QString &name : fieldNames) {
        schema.fields.push_back({ name, QStringLiteral("double") });
        schema.headers.append(name);
    }
    return schema;
}

static Catalogue3DEntry makeEntry(const Catalogue3DSchema &schema,
                                  const QMap<QString, QVariant> &fields = {})
{
    // Catalogue3DEntry::fields is now an index-ordered QVariantList aligned with
    // schema.fields (the memory-optimised layout that replaced the old per-row
    // name→value QMap). Place each named value at its schema index.
    Catalogue3DEntry entry;
    entry.fields.reserve(static_cast<int>(schema.fields.size()));
    for (const auto &f : schema.fields) {
        entry.fields.append(fields.value(f.name, QVariant()));
    }
    return entry;
}

// ── detectedRedshiftField ─────────────────────────────────────────────────────

void TestCatalogueParser::redshift_lowercase_z()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    QCOMPARE(Catalogue3DParser::detail::detectedRedshiftField(s), QStringLiteral("z"));
}

void TestCatalogueParser::redshift_uppercase_REDSHIFT()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("REDSHIFT") });
    QCOMPARE(Catalogue3DParser::detail::detectedRedshiftField(s), QStringLiteral("REDSHIFT"));
}

void TestCatalogueParser::redshift_ZSPEC()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("Zspec") });
    QCOMPARE(Catalogue3DParser::detail::detectedRedshiftField(s), QStringLiteral("Zspec"));
}

void TestCatalogueParser::redshift_ZMEAN()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("zmean") });
    QCOMPARE(Catalogue3DParser::detail::detectedRedshiftField(s), QStringLiteral("zmean"));
}

void TestCatalogueParser::redshift_ZPHOT()
{
    const auto s = makeSchema({ QStringLiteral("name"), QStringLiteral("RA"),
                                 QStringLiteral("Dec"), QStringLiteral("Zphoto_z") });
    QCOMPARE(Catalogue3DParser::detail::detectedRedshiftField(s), QStringLiteral("Zphoto_z"));
}

void TestCatalogueParser::redshift_absent()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("flux") });
    QVERIFY(Catalogue3DParser::detail::detectedRedshiftField(s).isEmpty());
}

void TestCatalogueParser::redshift_caseInsensitive_mixed()
{
    // "Redshift" normalises to "REDSHIFT" and must match the alias.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Redshift") });
    QCOMPARE(Catalogue3DParser::detail::detectedRedshiftField(s), QStringLiteral("Redshift"));
}

void TestCatalogueParser::redshift_multipleAliases_firstInAliasListWins()
{
    // Both "z" (→ "Z") and "redshift" (→ "REDSHIFT") present.
    // Alias list order: Z, REDSHIFT, … → "z" wins.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("redshift"), QStringLiteral("z") });
    QCOMPARE(Catalogue3DParser::detail::detectedRedshiftField(s), QStringLiteral("z"));
}

void TestCatalogueParser::redshift_unrelated_fields_notMatched()
{
    // Compound names like "flux_z" do NOT normalise to "Z" (letters+numbers only,
    // normalizeColName strips "_"), so "FLUXZ" doesn't match "Z".
    const auto s = makeSchema({ QStringLiteral("flux_z"), QStringLiteral("size_z"),
                                  QStringLiteral("RA") });
    // "flux_z" → normalized "FLUXZ", "size_z" → "SIZEZ" — neither matches "Z"
    QVERIFY(Catalogue3DParser::detail::detectedRedshiftField(s).isEmpty());
}

// ── detectedDistanceField ─────────────────────────────────────────────────────

void TestCatalogueParser::distance_DISTANCE()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("distance") });
    QCOMPARE(Catalogue3DParser::detail::detectedDistanceField(s), QStringLiteral("distance"));
}

void TestCatalogueParser::distance_DIST()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("Dist") });
    QCOMPARE(Catalogue3DParser::detail::detectedDistanceField(s), QStringLiteral("Dist"));
}

void TestCatalogueParser::distance_DMPC()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("dMpc") });
    QCOMPARE(Catalogue3DParser::detail::detectedDistanceField(s), QStringLiteral("dMpc"));
}

void TestCatalogueParser::distance_absent()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec") });
    QVERIFY(Catalogue3DParser::detail::detectedDistanceField(s).isEmpty());
}

// ── comovingDistanceMpc ───────────────────────────────────────────────────────

void TestCatalogueParser::comoving_zeroReturnsZero()
{
    QCOMPARE(Catalogue3DParser::detail::comovingDistanceMpc(0.0), 0.0);
}

void TestCatalogueParser::comoving_negativeReturnsZero()
{
    QCOMPARE(Catalogue3DParser::detail::comovingDistanceMpc(-1.0), 0.0);
}

void TestCatalogueParser::comoving_z01_reasonable()
{
    // Planck18-like constants (H0=67.74, Ωm=0.3089, ΩΛ=0.6911).
    // Independent estimate: D_C(z=0.1) ≈ 415–440 Mpc.
    const double d = Catalogue3DParser::detail::comovingDistanceMpc(0.1);
    QVERIFY2(d > 400.0 && d < 460.0,
             qPrintable(QStringLiteral("z=0.1 → %1 Mpc; expected 400–460").arg(d)));
}

void TestCatalogueParser::comoving_z1_reasonable()
{
    // D_C(z=1) ≈ 3200–3400 Mpc
    const double d = Catalogue3DParser::detail::comovingDistanceMpc(1.0);
    QVERIFY2(d > 3000.0 && d < 3600.0,
             qPrintable(QStringLiteral("z=1.0 → %1 Mpc; expected 3000–3600").arg(d)));
}

void TestCatalogueParser::comoving_z01_gt_z005()
{
    // Monotonicity: D_C is strictly increasing with z.
    const double d005 = Catalogue3DParser::detail::comovingDistanceMpc(0.05);
    const double d010 = Catalogue3DParser::detail::comovingDistanceMpc(0.10);
    QVERIFY(d010 > d005);
}

// ── entryDistanceMpc ──────────────────────────────────────────────────────────

void TestCatalogueParser::entryDist_fromDistanceField()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("distance") });
    auto e = makeEntry(s, { { QStringLiteral("distance"), QVariant(500.0) } });
    QCOMPARE(Catalogue3DParser::detail::entryDistanceMpc(e, s), 500.0);
}

void TestCatalogueParser::entryDist_fromRedshiftField()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("z"), QVariant(0.1) } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    QVERIFY2(d > 400.0 && d < 460.0,
             qPrintable(QStringLiteral("via z=0.1 got %1 Mpc; expected 400–460").arg(d)));
}

void TestCatalogueParser::entryDist_noDistance_isUnplaced()
{
    // No usable distance or redshift: no distance at all (issue #265), where
    // there used to be a 300 Mpc default.
    using namespace Catalogue3DParser::detail;
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("flux") });
    auto e = makeEntry(s);
    QVERIFY(std::isnan(entryDistanceMpc(e, s)));
    const EntryDistance d = entryDistance(e, s);
    QVERIFY(!d.placed());
    QVERIFY(d.reason == UnplacedReason::NoRedshift);
}

void TestCatalogueParser::entryDist_measuredDistance_beatsModelOverride()
{
    // entry.distanceMpc is the cosmology selector's distance for the redshift;
    // a measured distance wins under every model (issue #265). It used to be
    // the other way round, so WMAP9 replaced a catalogue's DIST.
    using namespace Catalogue3DParser::detail;
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("distance"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("distance"), QVariant(500.0) },
                            { QStringLiteral("z"), QVariant(0.1) } });
    e.distanceMpc = 750.0;
    const EntryDistance d = entryDistance(e, s);
    QCOMPARE(d.mpc, 500.0);
    QVERIFY(d.origin == DistanceOrigin::Column);
    QCOMPARE(d.column, QStringLiteral("distance"));
}

void TestCatalogueParser::entryDist_modelOverride_placesRedshiftOnlySources()
{
    using namespace Catalogue3DParser::detail;
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("z"), QVariant(0.1) } });
    e.distanceMpc = 750.0;
    const EntryDistance d = entryDistance(e, s);
    QCOMPARE(d.mpc, 750.0);
    QVERIFY(d.origin == DistanceOrigin::Redshift && d.fromModel);
}

void TestCatalogueParser::entryDist_negativeDistanceField_fallsBackToRedshift()
{
    // Negative distance is not physically meaningful → parser must ignore it
    // and fall back to the redshift column.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("distance"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("distance"), QVariant(-100.0) },
                         { QStringLiteral("z"),        QVariant(0.05)   } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    // comovingDistanceMpc(0.05) ≈ 200 Mpc
    QVERIFY2(d > 150.0 && d < 280.0,
             qPrintable(QStringLiteral("negative dist + z=0.05 gave %1 Mpc; expected 150–280").arg(d)));
}

void TestCatalogueParser::entryDist_zeroRedshift_isUnplaced()
{
    // z == 0 means "not measured" in a catalogue: no distance, in every mode
    // (issue #265; this test pinned the 300 Mpc default before).
    using namespace Catalogue3DParser::detail;
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("z"), QVariant(0.0) } });
    for (const auto mode : { CatalogueDistanceMode::Comoving, CatalogueDistanceMode::RedshiftSpace }) {
        s.distanceMode = mode;
        const EntryDistance d = entryDistance(e, s);
        QVERIFY(std::isnan(d.mpc));
        QVERIFY(d.reason == UnplacedReason::ZeroRedshift);
    }
    QCOMPARE(unplacedReasonText(UnplacedReason::ZeroRedshift), QStringLiteral("z = 0 (not measured)"));
}

void TestCatalogueParser::entryDist_negativeRedshift_isUnplaced()
{
    // M31: cz = −300 km/s. cz/H0 < 0 has no meaning in redshift space either.
    using namespace Catalogue3DParser::detail;
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("z"), QVariant(-0.001001) } });
    e.distanceMpc = 750.0;   // no override places it either
    for (const auto mode : { CatalogueDistanceMode::Comoving, CatalogueDistanceMode::RedshiftSpace }) {
        s.distanceMode = mode;
        const EntryDistance d = entryDistance(e, s);
        QVERIFY(!d.placed());
        QVERIFY(d.reason == UnplacedReason::NegativeRedshift);
    }
    QCOMPARE(unplacedReasonText(UnplacedReason::NegativeRedshift), QStringLiteral("z ≤ 0"));
}

void TestCatalogueParser::entryDist_negativeRedshift_withDistanceColumn_isPlaced()
{
    // A distance column > 0 places the source whatever its redshift.
    using namespace Catalogue3DParser::detail;
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z"),
                          QStringLiteral("DIST") });
    auto e = makeEntry(s, { { QStringLiteral("z"), QVariant(-0.001001) },
                            { QStringLiteral("DIST"), QVariant(0.78) } });
    s.distanceMode = CatalogueDistanceMode::RedshiftSpace;
    const EntryDistance d = entryDistance(e, s);
    QCOMPARE(d.mpc, 0.78);
    QVERIFY(d.origin == DistanceOrigin::Column);
    QVERIFY(!d.uncertain());
}

void TestCatalogueParser::entryDist_lowCz_isFlaggedUncertain()
{
    // 0 < cz < 1000 km/s from the redshift: placed, and flagged (peculiar
    // velocities of about 300 km/s). A measured distance is not flagged.
    using namespace Catalogue3DParser::detail;
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z"),
                          QStringLiteral("DIST") });
    const double z500 = 500.0 / kSpeedOfLightKms;
    auto low = makeEntry(s, { { QStringLiteral("z"), QVariant(z500) } });
    auto high = makeEntry(s, { { QStringLiteral("z"), QVariant(1500.0 / kSpeedOfLightKms) } });
    auto measured = makeEntry(s, { { QStringLiteral("z"), QVariant(z500) },
                                   { QStringLiteral("DIST"), QVariant(7.0) } });
    for (const auto mode : { CatalogueDistanceMode::Comoving, CatalogueDistanceMode::RedshiftSpace }) {
        s.distanceMode = mode;
        QVERIFY(entryDistance(low, s).placed());
        QVERIFY(entryDistance(low, s).uncertain());
        QVERIFY(!entryDistance(high, s).uncertain());
    }
    s.distanceMode = CatalogueDistanceMode::Comoving;
    QVERIFY(!entryDistance(measured, s).uncertain());
    // Redshift space places from z even with a column: then it is uncertain.
    s.distanceMode = CatalogueDistanceMode::RedshiftSpace;
    QVERIFY(entryDistance(measured, s).uncertain());
}

// ── issue #265: unplaced rows, the status line, the pick readout ──────────────

namespace {

Catalogue3DSchema skySchema()
{
    auto s = makeSchema({ QStringLiteral("NAME"), QStringLiteral("RA"), QStringLiteral("DEC"),
                          QStringLiteral("Z") });
    s.raFieldName = QStringLiteral("RA");
    s.decFieldName = QStringLiteral("DEC");
    return s;
}

QVariantMap skyRow(const QString &name, double ra, double dec, const QVariant &z)
{
    return { { QStringLiteral("NAME"), name }, { QStringLiteral("RA"), ra },
             { QStringLiteral("DEC"), dec }, { QStringLiteral("Z"), z } };
}

}   // namespace

void TestCatalogueParser::parse_unplacedRowsAreKeptApart()
{
    const auto s = skySchema();
    const QList<QVariantMap> rows{ skyRow(u"NGC 1"_s, 10.0, 5.0, 0.02),
                                   skyRow(u"M31"_s, 10.68, 41.27, -0.001001),
                                   skyRow(u"blank"_s, 20.0, 0.0, QString()),
                                   skyRow(u"zero"_s, 30.0, 0.0, 0.0),
                                   skyRow(u"NGC 2"_s, 40.0, -5.0, 0.03) };
    const auto parsed = Catalogue3DParser::parseBackendSubset(s, rows, { 0, 1, 2, 3, 4 });
    QVERIFY(parsed.valid);
    QVERIFY(parsed.errorMessage.isEmpty());
    QCOMPARE(parsed.entries.size(), std::size_t(2));
    QCOMPARE(parsed.unplaced.size(), std::size_t(3));
    // Every placed entry has a finite position, and the ids stay its index.
    for (std::size_t i = 0; i < parsed.entries.size(); ++i) {
        const auto &e = parsed.entries[i];
        QVERIFY(std::isfinite(e.sceneX) && std::isfinite(e.sceneY) && std::isfinite(e.sceneZ));
        QCOMPARE(e.id, int(i));
    }
    // The unplaced keep their rows in the table, for the list and SAMP.
    QCOMPARE(parsed.unplaced[0].sourceRow, qint64(1));
    QCOMPARE(parsed.unplaced[1].sourceRow, qint64(2));
    QCOMPARE(parsed.unplaced[2].sourceRow, qint64(3));
    QCOMPARE(parsed.skippedEntries, 0);
}

void TestCatalogueParser::parse_onlyUnplacedRows_isValidButSaysWhy()
{
    const auto s = skySchema();
    const auto parsed = Catalogue3DParser::parseBackendSubset(
            s, { skyRow(u"M31"_s, 10.68, 41.27, -0.001001), skyRow(u"M33"_s, 23.46, 30.66, -0.0006) });
    QVERIFY(parsed.valid);   // a page or a filter may hold only these
    QVERIFY(parsed.entries.empty());
    QCOMPARE(parsed.unplaced.size(), std::size_t(2));
    QVERIFY(parsed.errorMessage.contains(u"None of the 2 sources has a distance"_s));
}

void TestCatalogueParser::placement_statusLineCounts()
{
    const auto s = skySchema();
    const QList<QVariantMap> rows{ skyRow(u"near"_s, 10.0, 5.0, 300.0 / 299792.458),
                                   skyRow(u"far"_s, 20.0, 5.0, 0.05),
                                   skyRow(u"M31"_s, 10.68, 41.27, -0.001001) };
    const auto parsed = Catalogue3DParser::parseBackendSubset(s, rows);
    const auto c = CataloguePlacement::count(parsed.entries, parsed.unplaced, s);
    QCOMPARE(c.placed, qsizetype(2));
    QCOMPARE(c.unplaced, qsizetype(1));
    QCOMPARE(c.uncertain, qsizetype(1));
    QCOMPARE(CataloguePlacement::statusText(c),
             QStringLiteral("2 placed · 1 unplaced (no distance) · 1 with cz < 1000 km/s "
                            "(distance uncertain: peculiar velocities)"));
}

void TestCatalogueParser::placement_pickLinesNameTheOrigin()
{
    using namespace Catalogue3DParser::detail;
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z"),
                          QStringLiteral("DIST") });
    auto fromZ = makeEntry(s, { { QStringLiteral("z"), QVariant(0.05) } });
    QStringList lines = CataloguePlacement::pickLines(entryDistance(fromZ, s), u"WMAP9"_s, 69.32);
    // Integrated locally (no override): Planck18, whatever model is selected.
    QVERIFY2(lines.value(0).startsWith(u"Distance "_s)
                     && lines.value(0).endsWith(u" Mpc, from z (comoving, Planck18)"_s),
             qPrintable(lines.value(0)));
    QCOMPARE(lines.size(), 1);
    fromZ.distanceMpc = 212.3;
    lines = CataloguePlacement::pickLines(entryDistance(fromZ, s), u"WMAP9"_s, 69.32);
    QCOMPARE(lines.value(0), QStringLiteral("Distance 212.3 Mpc, from z (comoving, WMAP9)"));

    auto measured = makeEntry(s, { { QStringLiteral("z"), QVariant(0.05) },
                                   { QStringLiteral("DIST"), QVariant(12.34) } });
    lines = CataloguePlacement::pickLines(entryDistance(measured, s), u"Planck18"_s, 67.66);
    QCOMPARE(lines.value(0), QStringLiteral("Distance 12.3 Mpc, from column DIST"));

    auto near = makeEntry(s, { { QStringLiteral("z"), QVariant(500.0 / kSpeedOfLightKms) } });
    lines = CataloguePlacement::pickLines(entryDistance(near, s), u"Planck18"_s, 67.66);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.value(1), QStringLiteral("cz < 1000 km/s: uncertain"));

    s.distanceMode = CatalogueDistanceMode::RedshiftSpace;
    s.hubbleH0 = 69.32;
    lines = CataloguePlacement::pickLines(entryDistance(fromZ, s), u"WMAP9"_s, s.hubbleH0);
    QVERIFY2(lines.value(0).endsWith(u"from z (redshift space, cz/H0 with H0 = 69.32)"_s),
             qPrintable(lines.value(0)));
}

void TestCatalogueParser::placement_unplacedRows()
{
    const auto s = skySchema();
    const auto parsed = Catalogue3DParser::parseBackendSubset(
            s, { skyRow(u"M31"_s, 10.68, 41.27, -0.001001), skyRow(u"blank"_s, 20.0, 0.0, QString()),
                 skyRow(u"zero"_s, 30.0, 1.0, 0.0) });
    const auto rows = CataloguePlacement::unplacedRows(parsed.unplaced, s);
    QCOMPARE(rows.size(), std::size_t(3));
    QCOMPARE(rows[0].name, QStringLiteral("M31"));
    QCOMPARE(rows[0].raDeg, 10.68);
    QCOMPARE(rows[0].decDeg, 41.27);
    QVERIFY(qAbs(rows[0].czKms - (-300.0)) < 1.0);
    QCOMPARE(rows[0].reason, QStringLiteral("z ≤ 0"));
    QVERIFY(std::isnan(rows[1].czKms));
    QCOMPARE(rows[1].reason, QStringLiteral("no redshift"));
    QCOMPARE(rows[2].czKms, 0.0);
    QCOMPARE(rows[2].reason, QStringLiteral("z = 0 (not measured)"));
}

// ── distance-type (comoving / luminosity / angular-diameter) ──────────────────

void TestCatalogueParser::distanceKind_DL_isLuminosity()
{
    using namespace Catalogue3DParser::detail;
    QVERIFY(distanceFieldKind(QStringLiteral("DL")) == DistanceKind::Luminosity);
    QVERIFY(distanceFieldKind(QStringLiteral("LuminosityDistance")) == DistanceKind::Luminosity);
    QVERIFY(distanceFieldKind(QStringLiteral("DL_Mpc")) == DistanceKind::Luminosity);
}

void TestCatalogueParser::distanceKind_DA_isAngularDiameter()
{
    using namespace Catalogue3DParser::detail;
    QVERIFY(distanceFieldKind(QStringLiteral("DA")) == DistanceKind::AngularDiameter);
    QVERIFY(distanceFieldKind(QStringLiteral("AngularDiameterDistance")) == DistanceKind::AngularDiameter);
}

void TestCatalogueParser::distanceKind_generic_isComoving()
{
    using namespace Catalogue3DParser::detail;
    QVERIFY(distanceFieldKind(QStringLiteral("DISTANCE")) == DistanceKind::Comoving);
    QVERIFY(distanceFieldKind(QStringLiteral("ComovingDistance")) == DistanceKind::Comoving);
}

void TestCatalogueParser::entryDist_luminosityConvertedToComoving()
{
    // D_L = 220 Mpc at z=0.1 → D_C = D_L/(1+z) = 200 Mpc.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                QStringLiteral("DL"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("DL"), QVariant(220.0) },
                            { QStringLiteral("z"),  QVariant(0.1)   } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    QVERIFY2(qAbs(d - 200.0) < 1e-6, qPrintable(QStringLiteral("got %1, expected 200").arg(d)));
}

void TestCatalogueParser::entryDist_angularDiameterConvertedToComoving()
{
    // D_A = 200 Mpc at z=0.1 → D_C = D_A*(1+z) = 220 Mpc.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                QStringLiteral("DA"), QStringLiteral("z") });
    auto e = makeEntry(s, { { QStringLiteral("DA"), QVariant(200.0) },
                            { QStringLiteral("z"),  QVariant(0.1)   } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    QVERIFY2(qAbs(d - 220.0) < 1e-6, qPrintable(QStringLiteral("got %1, expected 220").arg(d)));
}

void TestCatalogueParser::entryDist_luminosityWithoutRedshift_usedAsIs()
{
    // No redshift column → can't convert → luminosity distance used as-is.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                QStringLiteral("DL") });
    auto e = makeEntry(s, { { QStringLiteral("DL"), QVariant(220.0) } });
    QCOMPARE(Catalogue3DParser::detail::entryDistanceMpc(e, s), 220.0);
}

void TestCatalogueParser::redshiftSpaceMode_usesHubbleDistance()
{
    // R-14: in redshift-space mode a source is placed at the naive Hubble
    // distance cz/H0 = 299792.458*0.1/67.66 ≈ 443.1 Mpc — distinct from the
    // comoving integral (~423 Mpc), which is the whole point of the mode.
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    s.distanceMode = CatalogueDistanceMode::RedshiftSpace;
    auto e = makeEntry(s, { { QStringLiteral("z"), QVariant(0.1) } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    const double expected = 299792.458 * 0.1 / 67.66;   // cz/H0
    QVERIFY2(qAbs(d - expected) < 1e-6,
             qPrintable(QStringLiteral("got %1, expected %2 (cz/H0)").arg(d).arg(expected)));
    // And it must differ from the comoving placement for the same redshift.
    s.distanceMode = CatalogueDistanceMode::Comoving;
    const double dc = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    QVERIFY2(qAbs(d - dc) > 5.0,
             qPrintable(QStringLiteral("redshift-space %1 == comoving %2").arg(d).arg(dc)));
}

void TestCatalogueParser::redshiftSpaceMode_bypassesDistanceOverride()
{
    // Redshift-space placement ignores measured/model (real-space) distances and
    // the cosmology-selector override — it is a function of redshift alone.
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                          QStringLiteral("distance"), QStringLiteral("z") });
    s.distanceMode = CatalogueDistanceMode::RedshiftSpace;
    auto e = makeEntry(s, { { QStringLiteral("distance"), QVariant(500.0) },
                            { QStringLiteral("z"),        QVariant(0.1)   } });
    e.distanceMpc = 750.0;   // override that Comoving mode would honour
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    const double expected = 299792.458 * 0.1 / 67.66;
    QVERIFY2(qAbs(d - expected) < 1e-6,
             qPrintable(QStringLiteral("got %1, expected %2 (cz/H0, not override)").arg(d).arg(expected)));
}

void TestCatalogueParser::redshiftSpaceMode_noRedshift_fallsBackToRealSpace()
{
    // Without a usable redshift, redshift-space mode has nothing to place from and
    // must fall back to the real-space logic (here: the measured distance).
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                          QStringLiteral("distance") });
    s.distanceMode = CatalogueDistanceMode::RedshiftSpace;
    auto e = makeEntry(s, { { QStringLiteral("distance"), QVariant(500.0) } });
    QCOMPARE(Catalogue3DParser::detail::entryDistanceMpc(e, s), 500.0);
}

void TestCatalogueParser::redshiftSpaceMode_usesTheModelsH0()
{
    // Issue #260: cz/H0 with the applied model's H0 (WMAP9: 69.32), where it
    // was Planck18's 67.66 under every model.
    auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    s.distanceMode = CatalogueDistanceMode::RedshiftSpace;
    s.hubbleH0 = 69.32;
    auto e = makeEntry(s, { { QStringLiteral("z"), QVariant(0.02) } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    QVERIFY2(qAbs(d - 299792.458 * 0.02 / 69.32) < 1e-9, qPrintable(QString::number(d)));
    // The presets' H0 as astropy has them (backend app/catalogue_positions.py
    // hubble_constant reads astropy's own).
    QCOMPARE(visivo::cosmology::presetH0(u"Planck18"_s), 67.66);
    QCOMPARE(visivo::cosmology::presetH0(u"Planck15"_s), 67.74);
    QCOMPARE(visivo::cosmology::presetH0(u"Planck13"_s), 67.77);
    QCOMPARE(visivo::cosmology::presetH0(u"WMAP9"_s), 69.32);
    QVERIFY(std::isnan(visivo::cosmology::presetH0(u"Custom"_s)));
}

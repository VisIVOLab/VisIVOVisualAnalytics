// test_catalogue_parser.cpp — implementations for TestCatalogueParser

#include "TestCatalogueParser.h"
#include "gui/Catalogue3DParser.h"

#include <QTest>

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

static Catalogue3DEntry makeEntry(const QMap<QString, QVariant> &fields = {})
{
    Catalogue3DEntry entry;
    entry.fields = fields;
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
    auto e = makeEntry({ { QStringLiteral("distance"), QVariant(500.0) } });
    QCOMPARE(Catalogue3DParser::detail::entryDistanceMpc(e, s), 500.0);
}

void TestCatalogueParser::entryDist_fromRedshiftField()
{
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    auto e = makeEntry({ { QStringLiteral("z"), QVariant(0.1) } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    QVERIFY2(d > 400.0 && d < 460.0,
             qPrintable(QStringLiteral("via z=0.1 got %1 Mpc; expected 400–460").arg(d)));
}

void TestCatalogueParser::entryDist_fallback300Mpc()
{
    // No usable distance or redshift → hardcoded 300 Mpc default.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("flux") });
    auto e = makeEntry();
    QCOMPARE(Catalogue3DParser::detail::entryDistanceMpc(e, s), 300.0);
}

void TestCatalogueParser::entryDist_distanceMpcOverride_takesPriority()
{
    // entry.distanceMpc > 0 is the cosmology-selector override and beats
    // any catalogue field value.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("distance") });
    auto e = makeEntry({ { QStringLiteral("distance"), QVariant(500.0) } });
    e.distanceMpc = 750.0;
    QCOMPARE(Catalogue3DParser::detail::entryDistanceMpc(e, s), 750.0);
}

void TestCatalogueParser::entryDist_negativeDistanceField_fallsBackToRedshift()
{
    // Negative distance is not physically meaningful → parser must ignore it
    // and fall back to the redshift column.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"),
                                  QStringLiteral("distance"), QStringLiteral("z") });
    auto e = makeEntry({ { QStringLiteral("distance"), QVariant(-100.0) },
                         { QStringLiteral("z"),        QVariant(0.05)   } });
    const double d = Catalogue3DParser::detail::entryDistanceMpc(e, s);
    // comovingDistanceMpc(0.05) ≈ 200 Mpc
    QVERIFY2(d > 150.0 && d < 280.0,
             qPrintable(QStringLiteral("negative dist + z=0.05 gave %1 Mpc; expected 150–280").arg(d)));
}

void TestCatalogueParser::entryDist_zeroRedshift_fallsBackTo300()
{
    // z == 0 is treated as "not provided" → comovingDistanceMpc(0) == 0,
    // so the parser should fall through to the 300 Mpc default.
    const auto s = makeSchema({ QStringLiteral("RA"), QStringLiteral("Dec"), QStringLiteral("z") });
    auto e = makeEntry({ { QStringLiteral("z"), QVariant(0.0) } });
    QCOMPARE(Catalogue3DParser::detail::entryDistanceMpc(e, s), 300.0);
}

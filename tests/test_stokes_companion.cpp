// test_stokes_companion.cpp
// The companion search runs against a directory listing that may come from the
// backend rather than the local disk, so the patterns and their preference
// order are pinned here instead of being tangled with file-system access.

#include "TestStokesCompanion.h"

#include "StokesCompanion.h"

#include <QTest>

void TestStokesCompanion::stokesInName_isReplacedInEachCasing()
{
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("g123_StokesI_mos.fits"),
                                              QStringLiteral("Q"))
                    .contains(QStringLiteral("g123_StokesQ_mos.fits")));
    // Lower case keeps the role lower case too, so the name stays consistent.
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("g123_stokesI_mos.fits"),
                                              QStringLiteral("U"))
                    .contains(QStringLiteral("g123_stokesu_mos.fits")));
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("G123_STOKESI.fits"),
                                              QStringLiteral("V"))
                    .contains(QStringLiteral("G123_STOKESV.fits")));
    // The lower-case rule is deliberately asymmetric: it matches "stokesI"
    // (capital I, as the Stokes parameter is always written) and produces a
    // lower-case role letter to match the rest of the name. An all-lower
    // "stokesi" is not a pattern we claim to handle.
    QVERIFY(!visivo::stokesCompanionCandidates(QStringLiteral("g123_stokesI_mos.fits"),
                                               QStringLiteral("U"))
                     .contains(QStringLiteral("g123_stokesU_mos.fits")));
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("g123_stokesi_mos.fits"),
                                              QStringLiteral("U"))
                    .isEmpty());
}

void TestStokesCompanion::askapSuffix_isReplacedIncludingTheGzipVariant()
{
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("image.restored_I.fits"),
                                              QStringLiteral("Q"))
                    .contains(QStringLiteral("image.restored_Q.fits")));
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("image.restored_I_PB.fits"),
                                              QStringLiteral("U"))
                    .contains(QStringLiteral("image.restored_U_PB.fits")));
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("image.restored_I.fits.gz"),
                                              QStringLiteral("V"))
                    .contains(QStringLiteral("image.restored_V.fits.gz")));
}

void TestStokesCompanion::theStokesIFileItself_isNeverACandidate()
{
    // Several patterns miss, and a miss leaves the name unchanged; loading the
    // I file again as its own Q would be silently wrong.
    const QStringList candidates = visivo::stokesCompanionCandidates(
            QStringLiteral("g123_StokesI_mos.fits"), QStringLiteral("Q"));
    QVERIFY(!candidates.contains(QStringLiteral("g123_StokesI_mos.fits")));
    QVERIFY(!candidates.isEmpty());
}

void TestStokesCompanion::pick_prefersTheStokesNamePatternOverTheBareSuffix()
{
    // A name can match two patterns at once: "StokesI" -> "StokesQ" and the
    // ASKAP "_I.fits" -> "_Q.fits" rule. Both companions exist here, so the
    // order the candidates are generated in is what decides, and it has to be
    // the explicit Stokes name - the bare suffix is the looser guess.
    const QString iName = QStringLiteral("mosaic_StokesI.fits");
    const QStringList candidates =
            visivo::stokesCompanionCandidates(iName, QStringLiteral("Q"));
    QVERIFY(candidates.contains(QStringLiteral("mosaic_StokesQ.fits")));
    QCOMPARE(candidates.first(), QStringLiteral("mosaic_StokesQ.fits"));
    const QSet<QString> listing = { iName, QStringLiteral("mosaic_StokesQ.fits"),
                                    QStringLiteral("mosaic_StokesI_Q.fits") };
    QCOMPARE(visivo::pickStokesCompanion(iName, QStringLiteral("Q"), listing),
             QStringLiteral("mosaic_StokesQ.fits"));
}

void TestStokesCompanion::pick_takesTheFirstCandidateThatExists()
{
    const QSet<QString> listing = { QStringLiteral("survey_StokesI.fits"),
                                    QStringLiteral("survey_StokesQ.fits"),
                                    QStringLiteral("unrelated.fits") };
    QCOMPARE(visivo::pickStokesCompanion(QStringLiteral("survey_StokesI.fits"),
                                         QStringLiteral("Q"), listing),
             QStringLiteral("survey_StokesQ.fits"));
}

void TestStokesCompanion::pick_returnsNothingWhenTheCompanionIsAbsent()
{
    const QSet<QString> listing = { QStringLiteral("survey_StokesI.fits") };
    QVERIFY(visivo::pickStokesCompanion(QStringLiteral("survey_StokesI.fits"),
                                        QStringLiteral("V"), listing)
                    .isEmpty());
}

void TestStokesCompanion::aNameThatMatchesNoPattern_yieldsNoCandidates()
{
    QVERIFY(visivo::stokesCompanionCandidates(QStringLiteral("cube.fits"), QStringLiteral("Q"))
                    .isEmpty());
    QVERIFY(visivo::stokesCompanionCandidates(QString(), QStringLiteral("Q")).isEmpty());
}

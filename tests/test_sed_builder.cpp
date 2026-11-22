// test_sed_builder.cpp
// See TestSedBuilder.h. Exercises visivo::SedBuilder against the two catalogue
// shapes a SED can arrive in.

#include "TestSedBuilder.h"

#include "SedBuilder.h"

#include <QTest>

#include <cmath>

using visivo::SedPoint;
using visivo::SedBuildResult;
namespace SedBuilder = visivo::SedBuilder;

namespace {

CatalogueOverlayEntry bandEntry(const QString &band, const QString &groupKey,
                                const QList<QPair<QString, QString>> &fields)
{
    CatalogueOverlayEntry entry;
    entry.band = band;
    entry.groupKey = groupKey;
    entry.label = groupKey;
    entry.extra = fields;
    return entry;
}

std::vector<int> allIndices(std::size_t n)
{
    std::vector<int> out;
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(static_cast<int>(i));
    return out;
}

}   // namespace

void TestSedBuilder::aBandMergedGroupBecomesOnePointPerBand()
{
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("70", "HIGALBM001", { { "flux70", "12.5" }, { "err_flux70", "1.2" },
                                        { "designation70", "HIGALPB001" } }),
        bandEntry("250", "HIGALBM001", { { "flux250", "40.0" }, { "err_flux250", "4.0" } }),
        bandEntry("500", "HIGALBM001", { { "flux500", "15.0" }, { "err_flux500", "1.5" } }),
    };

    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QVERIFY2(sed.valid, qPrintable(sed.errorMessage));
    QCOMPARE(sed.points.size(), std::size_t(3));
    QCOMPARE(sed.points[0].wavelengthUm, 70.0);
    QCOMPARE(sed.points[0].fluxJy, 12.5);
    QCOMPARE(sed.points[0].errorJy, 1.2);
    QCOMPARE(sed.points[0].label, QStringLiteral("HIGALPB001"));
    QCOMPARE(sed.points[2].wavelengthUm, 500.0);
    QCOMPARE(sed.sourceLabel, QStringLiteral("HIGALBM001"));
}

void TestSedBuilder::bandsWithoutAUsableFluxAreReportedNotSilentlyDropped()
{
    // A band-merged row routinely carries "missing" bands with a zero or
    // negative flux. Dropping them is right; dropping them silently is not,
    // because the user then cannot tell a missing band from a parse failure.
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("70", "G1", { { "flux70", "0" } }),
        bandEntry("160", "G1", { { "flux160", "-3.0" } }),
        bandEntry("250", "G1", { { "flux250", "40.0" } }),
        bandEntry("350", "G1", { { "flux350", "not-a-number" } }),
        bandEntry("500", "G1", { { "flux500", "15.0" } }),
    };

    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QVERIFY(sed.valid);
    QCOMPARE(sed.points.size(), std::size_t(2));
    QCOMPARE(sed.skippedBands.size(), 3);
    QVERIFY(sed.skippedBands.contains(QStringLiteral("70")));
    QVERIFY(sed.skippedBands.contains(QStringLiteral("350")));
}

void TestSedBuilder::theMsxSubBandsMapToTheirRealWavelengths()
{
    // The legacy view names four MSX bands after the 21 µm one; their real
    // wavelengths are 8.28, 12.13, 14.65 and 21.34 µm. Taking the tag at face
    // value would stack different bands on top of each other at 21 µm.
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("21_a"), 8.28);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("21_c"), 12.13);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("21_d"), 14.65);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("21_e"), 21.34);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("250"), 250.0);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("ft"), 0.0);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm(""), 0.0);
}

void TestSedBuilder::aFlatRowWithSeveralFluxColumnsBecomesASed()
{
    CatalogueOverlayEntry entry;
    entry.label = QStringLiteral("MySource");
    entry.extra = { { "flux_70", "1.0" }, { "err_flux_70", "0.1" },
                    { "flux_250", "8.0" }, { "err_flux_250", "0.8" },
                    { "notes", "bright" } };

    const SedBuildResult sed = SedBuilder::buildSed({ entry }, { 0 });
    QVERIFY2(sed.valid, qPrintable(sed.errorMessage));
    QCOMPARE(sed.points.size(), std::size_t(2));
    QCOMPARE(sed.points[0].wavelengthUm, 70.0);
    QCOMPARE(sed.points[0].errorJy, 0.1);
    QCOMPARE(sed.points[1].wavelengthUm, 250.0);
    QCOMPARE(sed.sourceLabel, QStringLiteral("MySource"));
}

void TestSedBuilder::fluxColumnNamesAreRecognisedInTheirCommonSpellings()
{
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux250"), 250.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux_250"), 250.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("F350"), 350.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("S_870um"), 870.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("fint160"), 160.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux_21.0"), 21.0);
}

void TestSedBuilder::aColumnThatOnlyLooksLikeAFluxIsNotOne()
{
    // These would each add a phantom SED point if the name were matched loosely.
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("err_flux250"), 0.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux250_err"), 0.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("background250"), 0.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flag70"), 0.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("glon"), 0.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux"), 0.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux0"), 0.0);
}

void TestSedBuilder::errorColumnsFollowTheirFluxColumnSpelling()
{
    CatalogueOverlayEntry entry;
    entry.extra = { { "F350", "8.0" }, { "F350_err", "0.4" },
                    { "flux870", "2.0" }, { "e_flux870", "0.2" } };

    const SedBuildResult sed = SedBuilder::buildSed({ entry }, { 0 });
    QVERIFY2(sed.valid, qPrintable(sed.errorMessage));
    QCOMPARE(sed.points.size(), std::size_t(2));
    QCOMPARE(sed.points[0].wavelengthUm, 350.0);
    QCOMPARE(sed.points[0].errorJy, 0.4);
    QCOMPARE(sed.points[1].wavelengthUm, 870.0);
    QCOMPARE(sed.points[1].errorJy, 0.2);
}

void TestSedBuilder::duplicateBandsAreKeptAsBranches()
{
    // A band-merged source can have more than one counterpart in the same band.
    // Those are the branches the legacy tool drew; discarding them is a silent
    // choice of the brightest, made on the user's behalf and never shown.
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("250", "G1", { { "flux250", "40.0" } }),
        bandEntry("250", "G1", { { "flux250", "42.0" } }),
        bandEntry("500", "G1", { { "flux500", "15.0" } }),
    };
    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QVERIFY(sed.valid);
    QCOMPARE(sed.points.size(), std::size_t(3));

    // Brightest first, in the fit; the fainter one is its alternative.
    QCOMPARE(sed.points[0].wavelengthUm, 250.0);
    QCOMPARE(sed.points[0].fluxJy, 42.0);
    QVERIFY(sed.points[0].use);
    QVERIFY(!sed.points[0].alternative);

    QCOMPARE(sed.points[1].wavelengthUm, 250.0);
    QCOMPARE(sed.points[1].fluxJy, 40.0);
    QVERIFY(!sed.points[1].use);
    QVERIFY(sed.points[1].alternative);

    QVERIFY(sed.points[2].use);
    QVERIFY(!sed.points[2].alternative);
}

void TestSedBuilder::abandWithTwoBranchesIsStillOneBand()
{
    // Two counterparts at 250 µm are two measurements of ONE band. Counting
    // them as two would let a source with a single band pass the "a SED needs
    // at least two bands" rule.
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("250", "G1", { { "flux250", "40.0" } }),
        bandEntry("250", "G1", { { "flux250", "42.0" } }),
    };
    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QVERIFY(!sed.valid);
    QVERIFY(sed.errorMessage.contains(QStringLiteral("at least 2")));
}

void TestSedBuilder::pointsComeBackSortedByWavelength()
{
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("870", "G1", { { "flux870", "2.0" } }),
        bandEntry("70", "G1", { { "flux70", "12.0" } }),
        bandEntry("250", "G1", { { "flux250", "40.0" } }),
    };
    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QVERIFY(sed.valid);
    QCOMPARE(sed.points[0].wavelengthUm, 70.0);
    QCOMPARE(sed.points[1].wavelengthUm, 250.0);
    QCOMPARE(sed.points[2].wavelengthUm, 870.0);
}

void TestSedBuilder::aSourceWithOneBandIsRefusedWithAReason()
{
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("250", "G1", { { "flux250", "40.0" } }),
    };
    const SedBuildResult sed = SedBuilder::buildSed(entries, { 0 });
    QVERIFY(!sed.valid);
    QVERIFY(sed.errorMessage.contains(QStringLiteral("at least 2")));
}

void TestSedBuilder::anEmptySelectionIsRefusedWithAReason()
{
    const SedBuildResult sed = SedBuilder::buildSed({}, {});
    QVERIFY(!sed.valid);
    QVERIFY(!sed.errorMessage.isEmpty());
}

void TestSedBuilder::theGroupOfASourceIsItsWholeBandMergedFamily()
{
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("70", "G1", {}), bandEntry("250", "G2", {}),
        bandEntry("500", "G1", {}), bandEntry("870", "G1", {}),
    };
    const std::vector<int> group = SedBuilder::sourceGroupIndices(entries, 2);
    QCOMPARE(group.size(), std::size_t(3));
    QCOMPARE(group[0], 0);
    QCOMPARE(group[1], 2);
    QCOMPARE(group[2], 3);
}

void TestSedBuilder::anUngroupedSourceIsItsOwnGroup()
{
    std::vector<CatalogueOverlayEntry> entries(3);
    const std::vector<int> group = SedBuilder::sourceGroupIndices(entries, 1);
    QCOMPARE(group.size(), std::size_t(1));
    QCOMPARE(group[0], 1);
    QVERIFY(SedBuilder::sourceGroupIndices(entries, 7).empty());
}

void TestSedBuilder::sizeComesFromTheBandFwhmAndFallsBackToTheEllipse()
{
    CatalogueOverlayEntry withFwhm =
            bandEntry("250", "G1", { { "flux250", "40.0" },
                                     { "fwhma250", "36.0" }, { "fwhmb250", "25.0" } });
    SedPoint point;
    QVERIFY(SedBuilder::pointFromEntry(withFwhm, 0, &point));
    QCOMPARE(point.sizeArcsec, 30.0);   // √(36·25)

    // The VLKB path filters the FWHM columns out of `extra` as geometry, so the
    // angular size travels on the entry itself.
    CatalogueOverlayEntry withEntrySize = bandEntry("250", "G1", { { "flux250", "40.0" } });
    withEntrySize.sizeArcsec = 22.0;
    QVERIFY(SedBuilder::pointFromEntry(withEntrySize, 0, &point));
    QCOMPARE(point.sizeArcsec, 22.0);

    CatalogueOverlayEntry bare = bandEntry("250", "G1", { { "flux250", "40.0" } });
    QVERIFY(SedBuilder::pointFromEntry(bare, 0, &point));
    QCOMPARE(point.sizeArcsec, 0.0);
}

void TestSedBuilder::drawnRadiiAreNeverReadAsAnAngularSize()
{
    // radiusX/radiusY are PIXELS for every Image-frame entry — that is how the
    // VLKB ellipse and a ds9 image region are built. Taking them for arcsec
    // gave the optically thick fit a solid angle wrong by the pixel scale
    // squared, and therefore a mass wrong by the same factor.
    CatalogueOverlayEntry pixelRadii = bandEntry("250", "G1", { { "flux250", "40.0" } });
    pixelRadii.frame = CatalogueOverlayEntry::Frame::Image;
    pixelRadii.radiusX = 10.0;
    pixelRadii.radiusY = 10.0;
    SedPoint point;
    QVERIFY(SedBuilder::pointFromEntry(pixelRadii, 0, &point));
    QCOMPARE(point.sizeArcsec, 0.0);
}

void TestSedBuilder::aWavelengthUnitSuffixIsConvertedNotIgnored()
{
    // S_1.2mm read as 1.2 µm would put a sub-mm point at the other end of the
    // SED, where the greybody slope is fitted.
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("S_1.2mm"), 1200.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux_3cm"), 30000.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("F_850um"), 850.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux_500micron"), 500.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux_1m"), 1000000.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux_800nm"), 0.8);
}

void TestSedBuilder::theRepresentativeSizeIsTheMedianOfTheUsedBands()
{
    std::vector<SedPoint> points(4);
    points[0].sizeArcsec = 10.0;
    points[1].sizeArcsec = 20.0;
    points[2].sizeArcsec = 30.0;
    points[3].sizeArcsec = 1000.0;
    points[3].use = false;          // excluded by the user: must not drag the size
    QCOMPARE(SedBuilder::representativeSizeArcsec(points), 20.0);

    std::vector<SedPoint> none(2);
    QCOMPARE(SedBuilder::representativeSizeArcsec(none), 0.0);
}

namespace {

CatalogueOverlayEntry sizedEntry(const QString &band, const QString &groupKey, double size,
                                 const QList<QPair<QString, QString>> &fields)
{
    CatalogueOverlayEntry entry = bandEntry(band, groupKey, fields);
    entry.sizeArcsec = size;
    return entry;
}

const SedPoint *findPoint(const std::vector<SedPoint> &points, double wavelength,
                          bool primary = true)
{
    for (const SedPoint &point : points) {
        if (std::abs(point.wavelengthUm - wavelength) < 1e-6 && point.alternative != primary)
            return &point;
    }
    return nullptr;
}

// Columns the VLKB row copies onto every band: they belong to no band suffix.
QList<QPair<QString, QString>> withRowColumns(QList<QPair<QString, QString>> fields,
                                              const QList<QPair<QString, QString>> &row)
{
    fields.append(row);
    return fields;
}

}   // namespace

void TestSedBuilder::wiseAndTwoMassColumnsAreReadAtTheirRealWavelengths()
{
    // flux34 and flux46 are WISE 3.4 and 4.6 µm. Read as numbers they put two
    // mid-IR points at 34 and 46 µm, in the middle of the far-IR rise.
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux34"), 3.4);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("FLUX46"), 4.6);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux12"), 12.0);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux2m_j"), 1.235);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux2m_k"), 2.159);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux21_e"), 21.34);
    QCOMPARE(SedBuilder::wavelengthFromFluxColumn("flux250"), 250.0);

    CatalogueOverlayEntry flat;
    flat.label = QStringLiteral("Flat");
    flat.extra = { { "flux34", "0.0178" }, { "err_flux34", "0.0009" }, { "flux250", "16.9" } };
    const SedBuildResult sed = SedBuilder::buildSed({ flat }, { 0 });
    QVERIFY2(sed.valid, qPrintable(sed.errorMessage));
    QCOMPARE(sed.points.front().wavelengthUm, 3.4);
    QCOMPARE(sed.points.front().band, QStringLiteral("WISE W1"));
}

void TestSedBuilder::aVlkbGroupTakesItsCounterpartFluxesFromTheirOwnBand()
{
    // HIGAL_FT_39.935089+0.135547 as the VLKB overlay builds it: one entry per
    // band, the WISE/2MASS and MSX columns copied onto every band of a row.
    const QString g = QStringLiteral("HIGAL_FT_39.935089+0.135547");
    const QList<QPair<QString, QString>> wiseA{
        { "flux34", "0.0177975044038" }, { "err_flux34", "9.17958010652E-4" },
        { "flux46", "0.0159925650427" }, { "err_flux46", "5.00809689377E-4" },
        { "flux12", "0.137353811849" },  { "flux2m_k", "0.00290352676606" },
    };
    const QList<QPair<QString, QString>> wiseB{
        { "flux34", "0.00267152417833" }, { "flux46", "0.0043723822813" },
        { "flux12", "0.0795500975383" },
    };
    const QList<QPair<QString, QString>> msx{
        { "flux21_a", "0.43873" }, { "err_flux21_a", "4.5" },  { "flux21_c", "1.231" },
        { "err_flux21_c", "45.4" }, { "flux21_d", "-0.43147" }, { "err_flux21_d", "-99.0" },
        { "flux21_e", "-1.3391" },  { "err_flux21_e", "-99.0" },
    };
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("ft", g, withRowColumns({ { "designationft", g } }, wiseA)),
        bandEntry("22", g, withRowColumns({ { "flux22", "0.211791488236" },
                                            { "designation22", "J190347.38+061719.5" } },
                                          wiseA)),
        bandEntry("22", g, withRowColumns({ { "flux22", "0.137122430365" } }, wiseB)),
        bandEntry("21", g, withRowColumns({ { "designation21", "MSX6CG039.9347+00.1337" } },
                                          msx)),
        bandEntry("70", g, withRowColumns({ { "flux70", "8.463378" } }, wiseA)),
        bandEntry("250", g, withRowColumns({ { "flux250", "16.89034" } }, wiseA)),
    };

    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QVERIFY2(sed.valid, qPrintable(sed.errorMessage));
    for (const SedPoint &point : sed.points) {
        QVERIFY2(point.wavelengthUm != 34.0 && point.wavelengthUm != 46.0,
                 qPrintable(QStringLiteral("point at %1 µm").arg(point.wavelengthUm)));
    }

    // One branch per WISE counterpart, not one per band row it was copied to.
    int at34 = 0;
    for (const SedPoint &point : sed.points)
        at34 += point.wavelengthUm == 3.4 ? 1 : 0;
    QCOMPARE(at34, 2);
    const SedPoint *w1 = findPoint(sed.points, 3.4);
    QVERIFY(w1);
    QCOMPARE(w1->fluxJy, 0.0177975044038);
    QCOMPARE(w1->label, QStringLiteral("J190347.38+061719.5"));
    QVERIFY(findPoint(sed.points, 2.159));

    // MSX reaches the SED at A and C; D and E are sensitivity limits.
    const SedPoint *msxA = findPoint(sed.points, 8.28);
    QVERIFY(msxA);
    QCOMPARE(msxA->band, QStringLiteral("MSX A"));
    QCOMPARE(msxA->label, QStringLiteral("MSX6CG039.9347+00.1337"));
    QVERIFY(findPoint(sed.points, 12.13));
    QVERIFY(!findPoint(sed.points, 14.65));
    QVERIFY(!findPoint(sed.points, 21.34));
    QVERIFY(sed.skippedBands.contains(QStringLiteral("MSX D")));
    QVERIFY(!sed.skippedBands.contains(QStringLiteral("21")));
}

void TestSedBuilder::msxErrorsArePercentOfTheFlux()
{
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("21", "G", { { "flux21_a", "0.43873" }, { "err_flux21_a", "4.5" },
                               { "flux21_e", "2.0" }, { "err_flux21_e", "-99.0" } }),
    };
    const SedBuildResult sed = SedBuilder::buildSed(entries, { 0 });
    QVERIFY2(sed.valid, qPrintable(sed.errorMessage));
    QCOMPARE(sed.points[0].wavelengthUm, 8.28);
    QVERIFY(std::abs(sed.points[0].errorJy - 0.43873 * 0.045) < 1e-12);
    QCOMPARE(sed.points[1].wavelengthUm, 21.34);
    QCOMPARE(sed.points[1].errorJy, 0.0);
}

void TestSedBuilder::theDefaultFitStartsAt160Micron()
{
    std::vector<SedPoint> points(6);
    const double wavelengths[] = { 22.0, 70.0, 160.0, 250.0, 250.0, 500.0 };
    for (std::size_t i = 0; i < points.size(); ++i)
        points[i].wavelengthUm = wavelengths[i];
    points[4].alternative = true;
    QVERIFY(SedBuilder::applyDefaultFitRange(points,
                                             SedBuilder::kDefaultFitMinWavelengthUm));
    QVERIFY(!points[0].use);
    QVERIFY(!points[1].use);
    QVERIFY(points[2].use);
    QVERIFY(points[3].use);
    QVERIFY(!points[4].use);
    QVERIFY(points[5].use);

    QVERIFY(SedBuilder::applyDefaultFitRange(points, 0.0));
    QVERIFY(points[0].use && points[1].use);
    QVERIFY(!points[4].use);
}

void TestSedBuilder::theDefaultFitFallsBackToEveryBandWhenTooFewAreLeft()
{
    std::vector<SedPoint> points(3);
    points[0].wavelengthUm = 24.0;
    points[1].wavelengthUm = 70.0;
    points[2].wavelengthUm = 250.0;
    QVERIFY(!SedBuilder::applyDefaultFitRange(points, 160.0));
    for (const SedPoint &point : points)
        QVERIFY(point.use);
}

void TestSedBuilder::theCatalogueDistanceIsReadButInParsecs()
{
    // The VLKB view declares pc and stores kpc.
    QString column;
    QCOMPARE(SedBuilder::distancePcFromFields({ { "distance", "6.511718" } }, &column),
             6511.718);
    QCOMPARE(column, QStringLiteral("distance"));
    QCOMPARE(SedBuilder::distancePcFromFields({ { "distance", "11850" } }), 11850.0);
    QCOMPARE(SedBuilder::distancePcFromFields({ { "dist_kpc", "2.5" } }), 2500.0);
    QCOMPARE(SedBuilder::distancePcFromFields({ { "distance_pc", "40" } }), 40.0);
    QCOMPARE(SedBuilder::distancePcFromFields({ { "distance", "-9999" } }), 0.0);

    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("250", "G", { { "flux250", "10" }, { "distance", "6.511718" } }),
        bandEntry("500", "G", { { "flux500", "3" }, { "distance", "6.511718" } }),
    };
    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QCOMPARE(sed.catalogueDistancePc, 6511.718);
}

void TestSedBuilder::scalingTo250ReproducesTheCatalogueFluxes()
{
    // VLKB fluxes and FWHM (arcsec) of three clumps of the m030039 field, and
    // the 350/500 µm fluxes the Hi-GAL clump catalogue (Elia et al. 2021,
    // higal360clump) lists after scaling them to the 250 µm size.
    struct Clump
    {
        const char *name;
        double f350, f500, a250, b250, a350, b350, a500, b500, s350, s500, tolerance;
    };
    const Clump clumps[] = {
        { "HIGALBM39.9345+0.1325", 7.400366, 3.1522818, 26.82, 29.76, 36.97, 33.27, 50.12, 40.03,
          6.578, 2.558, 0.005 },
        { "HIGALBM39.9398-0.1493", 3.3374379, 4.6674876, 33.86, 25.58, 40.14, 33.11, 54.26, 69.3,
          2.95, 2.192, 0.005 },
        // The smallest of the three, about 1.15 beams wide: the catalogue
        // values are matched to 1.4 % for a reason the papers do not give.
        { "HIGALBM40.1051-0.0520", 4.552342, 2.2939873, 19.61, 21.03, 30.91, 27.41, 38.59,
          45.63, 2.944, 1.003, 0.02 },
    };
    for (const Clump &clump : clumps) {
        std::vector<SedPoint> points(3);
        points[0].wavelengthUm = 250.0;
        points[0].sizeArcsec = std::sqrt(clump.a250 * clump.b250);
        points[1].wavelengthUm = 350.0;
        points[1].fluxJy = clump.f350;
        points[1].sizeArcsec = std::sqrt(clump.a350 * clump.b350);
        points[2].wavelengthUm = 500.0;
        points[2].fluxJy = clump.f500;
        points[2].sizeArcsec = std::sqrt(clump.a500 * clump.b500);
        const SedBuilder::SizeScaling scaling = SedBuilder::sizeScalingTo250(points);
        QVERIFY2(scaling.valid, qPrintable(scaling.errorMessage));
        QCOMPARE(scaling.factors[0], 1.0);
        const double r350 = clump.f350 * scaling.factors[1] / clump.s350 - 1.0;
        const double r500 = clump.f500 * scaling.factors[2] / clump.s500 - 1.0;
        QVERIFY2(std::abs(r350) < clump.tolerance,
                 qPrintable(QStringLiteral("%1 350 µm off by %2").arg(clump.name).arg(r350)));
        QVERIFY2(std::abs(r500) < clump.tolerance,
                 qPrintable(QStringLiteral("%1 500 µm off by %2").arg(clump.name).arg(r500)));
    }
}

void TestSedBuilder::scalingTo250NeedsA250Size()
{
    std::vector<SedPoint> points(2);
    points[0].wavelengthUm = 250.0;
    points[1].wavelengthUm = 500.0;
    points[1].sizeArcsec = 50.0;
    QVERIFY(!SedBuilder::sizeScalingTo250(points).valid);

    // A source no larger than the beam keeps its observed size, and says so.
    points[0].sizeArcsec = 15.0;
    const SedBuilder::SizeScaling scaling = SedBuilder::sizeScalingTo250(points);
    QVERIFY(scaling.valid);
    QVERIFY(scaling.observedSizeUsed.contains(QStringLiteral("250")));
    QCOMPARE(scaling.factors[1], 15.0 / 50.0);
}

void TestSedBuilder::scalingTo250NeverMixesDeconvolvedAndObservedSizes()
{
    // 250 µm just above its beam deconvolves to 2.7″; 350 µm below its beam
    // stays at 24″. Their ratio would cut the flux tenfold.
    std::vector<SedPoint> points(2);
    points[0].wavelengthUm = 250.0;
    points[0].sizeArcsec = 18.0;
    points[1].wavelengthUm = 350.0;
    points[1].sizeArcsec = 24.0;
    const SedBuilder::SizeScaling scaling = SedBuilder::sizeScalingTo250(points);
    QVERIFY(scaling.valid);
    QCOMPARE(scaling.factors[1], 18.0 / 24.0);
    QVERIFY(scaling.observedSizeUsed.contains(QStringLiteral("350")));
}

void TestSedBuilder::observedPlusGreybodyLuminosityFollowsTheCatalogueRecipe()
{
    // HIGALBM39.9345+0.1325 in Elia et al. (2021): T = 20.0 K, M = 476.6 M☉ at
    // 11.85 kpc (β = 2, κ = 0.1 cm²/g at 300 µm), observed 22, 24, 70 and
    // 160 µm fluxes, catalogue L_bol = 4245.7 L☉.
    constexpr double h = 6.62607015e-27, k = 1.380649e-16, c = 2.99792458e10;
    constexpr double msun = 1.98847e33, pc = 3.0856775814913673e18;
    const double distancePc = 11850.0;
    const auto greybody = [&](double wavelengthUm) {
        const double nu = c / (wavelengthUm * 1e-4);
        const double nu0 = c / 300e-4;
        const double planck = 2.0 * h * nu * nu * nu / (c * c) / std::expm1(h * nu / (k * 20.0));
        const double d = distancePc * pc;
        return 476.6 * msun * 0.1 * (nu / nu0) * (nu / nu0) * planck / (d * d) * 1e23;
    };
    std::vector<double> wavelengths;
    std::vector<double> fluxes;
    for (int i = 0; i < 400; ++i) {
        const double wavelength = 5.0 * std::pow(400.0, i / 399.0);
        wavelengths.push_back(wavelength);
        fluxes.push_back(greybody(wavelength));
    }

    std::vector<SedPoint> points(5);
    const double observed[][2] = { { 22.0, 0.199 }, { 24.0, 0.077 }, { 70.0, 8.463 },
                                   { 160.0, 25.534 }, { 250.0, 16.89 } };
    for (std::size_t i = 0; i < points.size(); ++i) {
        points[i].wavelengthUm = observed[i][0];
        points[i].fluxJy = observed[i][1];
        points[i].use = observed[i][0] >= 160.0;
    }
    const auto chosen = SedBuilder::observedLuminosityPoints(points);
    QCOMPARE(chosen.size(), std::size_t(4));
    const double luminosity = SedBuilder::observedPlusGreybodyLuminosityLsun(
            chosen, wavelengths, fluxes, distancePc);
    QVERIFY2(std::abs(luminosity / 4245.7 - 1.0) < 0.01,
             qPrintable(QStringLiteral("L_bol = %1").arg(luminosity)));

    // No 70 µm detection: starless, the greybody alone is L_bol.
    points[2].upperLimit = true;
    QVERIFY(SedBuilder::observedLuminosityPoints(points).empty());
    QCOMPARE(SedBuilder::observedPlusGreybodyLuminosityLsun({}, wavelengths, fluxes, distancePc),
             0.0);
}

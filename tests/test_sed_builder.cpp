// test_sed_builder.cpp
// See TestSedBuilder.h. Exercises visivo::SedBuilder against the two catalogue
// shapes a SED can arrive in.

#include "TestSedBuilder.h"

#include "SedBuilder.h"

#include <QTest>

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
    // The legacy view names three MSX bands after the 21 µm one; their real
    // wavelengths are 12.1, 14.7 and 21 µm. Taking the tag at face value would
    // stack three different bands on top of each other at 21 µm.
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("21_c"), 12.1);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("21_d"), 14.7);
    QCOMPARE(SedBuilder::bandTagToWavelengthUm("21_e"), 21.0);
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

void TestSedBuilder::duplicateBandsCollapseToTheBrighterMeasurement()
{
    const std::vector<CatalogueOverlayEntry> entries{
        bandEntry("250", "G1", { { "flux250", "40.0" } }),
        bandEntry("250", "G1", { { "flux250", "42.0" } }),
        bandEntry("500", "G1", { { "flux500", "15.0" } }),
    };
    const SedBuildResult sed = SedBuilder::buildSed(entries, allIndices(entries.size()));
    QVERIFY(sed.valid);
    QCOMPARE(sed.points.size(), std::size_t(2));
    QCOMPARE(sed.points[0].wavelengthUm, 250.0);
    QCOMPARE(sed.points[0].fluxJy, 42.0);
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

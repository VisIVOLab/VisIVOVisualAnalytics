#include "TestSedPlotStyle.h"

#include "gui/SedPlotStyle.h"

#include <QtTest>

#include <cmath>

using namespace Qt::Literals::StringLiterals;
using visivo::SedPoint;
namespace sp = visivo::SedPlot;

namespace {

SedPoint band(double wavelength, double flux, double error = 0.0, bool upperLimit = false,
              bool use = true)
{
    SedPoint point;
    point.wavelengthUm = wavelength;
    point.fluxJy = flux;
    point.errorJy = error;
    point.upperLimit = upperLimit;
    point.use = use;
    return point;
}

}   // namespace

void TestSedPlotStyle::bandTagsMapToSurveys()
{
    QCOMPARE(sp::bandFamily(u"2MASS Ks"_s, 2.159), sp::Family::TwoMass);
    QCOMPARE(sp::bandFamily(u"WISE W1"_s, 3.4), sp::Family::Wise);
    QCOMPARE(sp::bandFamily(u"22"_s, 22.0), sp::Family::Wise);
    QCOMPARE(sp::bandFamily(u"MSX A"_s, 8.28), sp::Family::Msx);
    QCOMPARE(sp::bandFamily(u"21"_s, 21.34), sp::Family::Msx);
    QCOMPARE(sp::bandFamily(u"21_c"_s, 12.13), sp::Family::Msx);
    QCOMPARE(sp::bandFamily(u"8.0"_s, 8.0), sp::Family::SpitzerIrac);
    QCOMPARE(sp::bandFamily(QString(), 4.5), sp::Family::SpitzerIrac);
    QCOMPARE(sp::bandFamily(QString(), 4.6), sp::Family::Wise);
    QCOMPARE(sp::bandFamily(QString(), 12.0), sp::Family::Wise);
    QCOMPARE(sp::bandFamily(QString(), 12.13), sp::Family::Msx);
    QCOMPARE(sp::bandFamily(u"24"_s, 24.0), sp::Family::SpitzerMips);
    QCOMPARE(sp::bandFamily(u"70"_s, 70.0), sp::Family::Pacs);
    QCOMPARE(sp::bandFamily(u"160"_s, 160.0), sp::Family::Pacs);
    QCOMPARE(sp::bandFamily(u"250"_s, 250.0), sp::Family::Spire);
    QCOMPARE(sp::bandFamily(u"500"_s, 500.0), sp::Family::Spire);
    QCOMPARE(sp::bandFamily(u"870"_s, 870.0), sp::Family::Other);
    QCOMPARE(sp::bandFamily(u"pacs blue"_s, 70.0), sp::Family::Pacs);
    QCOMPARE(sp::familyName(sp::Family::Spire), u"Herschel SPIRE"_s);
}

void TestSedPlotStyle::paletteReadsOnBothThemes()
{
    for (bool dark : { false, true }) {
        const sp::Rgb surface = dark ? sp::kDarkSurface : sp::kLightSurface;
        const QString theme = dark ? u"dark"_s : u"light"_s;
        for (sp::Family family : sp::familyOrder()) {
            const sp::Rgb colour = sp::familyColour(family, dark);
            const double full = sp::contrastRatio(colour, surface);
            QVERIFY2(full >= 3.0,
                     qPrintable(u"%1 %2: %3"_s.arg(sp::familyName(family), theme).arg(full)));
            // A band out of the fit is drawn faded; it must still be seen.
            const double faded =
                    sp::contrastRatio(sp::blend(colour, sp::kUnusedAlpha, surface), surface);
            QVERIFY2(faded >= 2.5, qPrintable(u"%1 %2 faded: %3"_s
                                                      .arg(sp::familyName(family), theme)
                                                      .arg(faded)));
        }
        for (sp::Curve curve : { sp::Curve::Thin, sp::Curve::Thick, sp::Curve::Model })
            QVERIFY(sp::contrastRatio(sp::curveColour(curve, dark), surface) >= 3.0);
    }
}

void TestSedPlotStyle::fluxRangeComesFromPhotometryOnly()
{
    // Only the photometry goes in: a greybody at 1e-60 Jy has no way to reach
    // the range. The bounds sit a decade below the faintest band and half a
    // decade above the top of the brightest error bar.
    const std::vector<SedPoint> points{ band(70.0, 2.0, 0.2), band(160.0, 12.0, 1.0),
                                        band(250.0, 9.0, 0.9), band(500.0, 1.5, 0.3) };
    const sp::AxisRange range = sp::fluxRange(points, /*logScale=*/true);
    QVERIFY(range.valid);
    QVERIFY(qFuzzyCompare(range.lower, 0.15));
    QVERIFY(qFuzzyCompare(range.upper, 13.0 * std::sqrt(10.0)));
}

void TestSedPlotStyle::linearRangeIsProportional()
{
    const std::vector<SedPoint> points{ band(160.0, 10.0, 1.0), band(250.0, 20.0, 2.0) };
    const sp::AxisRange range = sp::fluxRange(points, /*logScale=*/false);
    QVERIFY(range.valid);
    // 9 to 22 Jy with the error bars, padded by 8 % of the 13 Jy span.
    QVERIFY(qFuzzyCompare(range.lower, 9.0 - 0.08 * 13.0));
    QVERIFY(qFuzzyCompare(range.upper, 22.0 + 0.08 * 13.0));
}

void TestSedPlotStyle::singleBandHasARange()
{
    const std::vector<SedPoint> one{ band(250.0, 4.0) };
    const sp::AxisRange log = sp::fluxRange(one, true);
    QVERIFY(log.valid && log.lower < 4.0 && log.upper > 4.0);
    const sp::AxisRange linear = sp::fluxRange(one, false);
    QVERIFY(linear.valid && linear.lower < 4.0 && linear.upper > 4.0);
    const sp::AxisRange x = sp::wavelengthRange(one, false);
    QVERIFY(x.valid && x.lower < 250.0 && x.upper > 250.0);
    QVERIFY(!sp::fluxRange({}, true).valid);
}

void TestSedPlotStyle::upperLimitsAndErrorsWidenTheRange()
{
    // An upper limit below every detection sets the floor, and a band left out
    // of the fit counts as much as one in it.
    const std::vector<SedPoint> points{ band(70.0, 0.05, 0.0, /*upperLimit=*/true),
                                        band(250.0, 8.0, 0.0, false, /*use=*/false),
                                        band(160.0, 4.0, 6.0) };
    const sp::AxisRange range = sp::fluxRange(points, true);
    QVERIFY(qFuzzyCompare(range.lower, 0.005));
    QVERIFY(qFuzzyCompare(range.upper, 10.0 * std::sqrt(10.0)));
    // A non-positive value cannot be drawn on a log axis and is skipped.
    const sp::AxisRange skipped = sp::fluxRange({ band(70.0, 0.0), band(160.0, 1.0) }, true);
    QVERIFY(qFuzzyCompare(skipped.lower, 0.1));
}

void TestSedPlotStyle::wavelengthRangeCoversTheBands()
{
    const std::vector<SedPoint> points{ band(1.235, 0.01), band(500.0, 1.0) };
    const sp::AxisRange log = sp::wavelengthRange(points, true);
    QVERIFY(log.lower < 1.235 && log.lower > 0.5);
    QVERIFY(log.upper > 500.0 && log.upper < 1000.0);
}

void TestSedPlotStyle::treeLinksFollowTheBranches()
{
    // One source, two branches (two rows of the SED view): the 160 and 350 µm
    // detections are on both, the 250 µm band has a counterpart on each. The
    // tree forks at 160 µm and joins again at 350 µm; the two 250 µm points are
    // not joined to each other.
    SedPoint p160 = band(160.0, 30.0);
    p160.branches = { 7, 8 };
    SedPoint p250a = band(250.0, 25.0);
    p250a.branches = { 7 };
    SedPoint p250b = band(250.0, 9.0, 0.0, false, /*use=*/false);
    p250b.branches = { 8 };
    SedPoint p350 = band(350.0, 12.0);
    p350.branches = { 7, 8 };
    const std::vector<SedPoint> points{ p160, p250a, p250b, p350 };

    const std::vector<sp::Edge> edges = sp::treeEdges(points);
    const std::vector<sp::Edge> expected{ { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 } };
    QCOMPARE(edges.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        QVERIFY(edges[i] == expected[i]);

    // A branch missing a band links over it; points with no branch have no links.
    SedPoint p500 = band(500.0, 4.0);
    p500.branches = { 8 };
    const std::vector<SedPoint> skipping{ p160, p250a, p350, p500 };
    const std::vector<sp::Edge> skip = sp::treeEdges(skipping);
    const std::vector<sp::Edge> skipExpected{ { 0, 1 }, { 0, 2 }, { 1, 2 }, { 2, 3 } };
    QCOMPARE(skip.size(), skipExpected.size());
    for (std::size_t i = 0; i < skipExpected.size(); ++i)
        QVERIFY(skip[i] == skipExpected[i]);
    QVERIFY(sp::treeEdges({ band(160.0, 1.0), band(250.0, 2.0) }).empty());
}

void TestSedPlotStyle::collapsedSedIsOneLine()
{
    // One point per band: the ticked counterpart at 250 µm, not the brighter
    // unticked one; upper limits are not on the line.
    const std::vector<SedPoint> points{
        band(160.0, 30.0), band(250.0, 25.0), band(250.0, 40.0, 0.0, false, /*use=*/false),
        band(350.0, 12.0), band(500.0, 6.0, 0.0, /*upperLimit=*/true)
    };
    QCOMPARE(sp::singleLine(points), std::vector<int>({ 0, 1, 3 }));
    // With no band ticked the brightest stands for it.
    const std::vector<SedPoint> none{ band(250.0, 5.0, 0.0, false, false),
                                      band(250.0, 8.0, 0.0, false, false) };
    QCOMPARE(sp::singleLine(none), std::vector<int>({ 1 }));
}

void TestSedPlotStyle::logErrorBarsStopAtTheFloor()
{
    // A 4 ± 1 Jy point keeps its arm; a 4 ± 6 Jy point would reach below zero
    // and stops at the floor, flagged as cut. Linear axes never cut.
    const sp::LowerArm kept = sp::lowerErrorArm(4.0, 1.0, /*logScale=*/true, 0.1);
    QCOMPARE(kept.length, 1.0);
    QVERIFY(!kept.clipped);
    const sp::LowerArm cut = sp::lowerErrorArm(4.0, 6.0, true, 0.1);
    QVERIFY(cut.clipped);
    QVERIFY(qFuzzyCompare(cut.length, 3.9));
    const sp::LowerArm linear = sp::lowerErrorArm(4.0, 6.0, false, -3.0);
    QCOMPARE(linear.length, 6.0);
    QVERIFY(!linear.clipped);
    QCOMPARE(sp::lowerErrorArm(4.0, 0.0, true, 0.1).length, 0.0);
}

void TestSedPlotStyle::theTreeIsRebuiltFromTheViewRows()
{
    // The VLKB SED view returns one row per branch. Rows 3 and 4 share the
    // 350 µm detection and the 22 µm one (with its WISE photometry); each has
    // its own 250 µm counterpart. Built into a SED, the 250 µm band forks.
    const auto entry = [](const QString &band, std::vector<int> rows,
                          const QList<QPair<QString, QString>> &fields) {
        CatalogueOverlayEntry e;
        e.band = band;
        e.groupKey = QStringLiteral("HIGALBM042");
        e.extra = fields;
        e.sedRows = std::move(rows);
        return e;
    };
    const std::vector<CatalogueOverlayEntry> entries{
        entry(u"22"_s, { 3, 4 }, { { u"flux22"_s, u"0.8"_s }, { u"flux12"_s, u"0.3"_s } }),
        entry(u"250"_s, { 3 }, { { u"flux250"_s, u"20.0"_s } }),
        entry(u"250"_s, { 4 }, { { u"flux250"_s, u"7.0"_s } }),
        entry(u"350"_s, { 3, 4 }, { { u"flux350"_s, u"9.0"_s } }),
    };
    const visivo::SedBuildResult sed =
            visivo::SedBuilder::buildSed(entries, { 0, 1, 2, 3 });
    QVERIFY2(sed.valid, qPrintable(sed.errorMessage));
    // 12 (WISE W3, riding on the 22 µm entry), 22, 250 bright, 250 faint, 350.
    QCOMPARE(sed.points.size(), std::size_t(5));
    QCOMPARE(sed.points[0].wavelengthUm, 12.0);
    QCOMPARE(sed.points[0].branches, std::vector<int>({ 3, 4 }));

    const std::vector<sp::Edge> edges = sp::treeEdges(sed.points);
    const std::vector<sp::Edge> expected{ { 0, 1 }, { 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 } };
    QCOMPARE(edges.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        QVERIFY(edges[i] == expected[i]);

    // Collapsed into one point per band it is a single line again.
    QCOMPARE(sp::singleLine(sed.points), std::vector<int>({ 0, 1, 2, 4 }));
}

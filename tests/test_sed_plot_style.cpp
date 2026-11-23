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
            QVERIFY2(faded >= 1.8, qPrintable(u"%1 %2 faded: %3"_s
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

void TestSedPlotStyle::fitLineSkipsDuplicatedBands()
{
    // Two ticked counterparts at 250 µm: the line goes from 160 to 350 µm
    // instead of running up and down between them. Unticked bands and upper
    // limits are not on it.
    const std::vector<SedPoint> points{
        band(350.0, 5.0), band(250.0, 9.0), band(160.0, 12.0), band(250.0, 3.0),
        band(500.0, 2.0, 0.0, false, /*use=*/false), band(70.0, 0.4, 0.0, /*upperLimit=*/true)
    };
    const auto line = sp::fitLine(points);
    QCOMPARE(line.size(), std::size_t(2));
    QCOMPARE(line[0].first, 160.0);
    QCOMPARE(line[1].first, 350.0);
}

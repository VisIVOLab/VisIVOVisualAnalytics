// test_region_measure.cpp
// See TestRegionMeasure.h.

#include "TestRegionMeasure.h"

#include "RegionMeasure.h"
#include "RegionModel.h"

#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;
using namespace visivo::measure;
using visivo::regions::Shape;

namespace {

QString valueOf(const Rows &rows, const QString &label)
{
    for (const auto &r : rows)
        if (r.first == label)
            return r.second;
    return QStringLiteral("<missing>");
}

bool has(const Rows &rows, const QString &label)
{
    return valueOf(rows, label) != QStringLiteral("<missing>");
}

Numbers someNumbers()
{
    Numbers n;
    n.validCount = 77;
    n.totalCount = 80;
    n.blankedCount = 3;
    n.minValue = -0.5;
    n.maxValue = 9.0;
    n.sum = 196.5;
    n.mean = 2.55;
    n.median = 2.4;
    n.stddev = 1.25;
    n.rmsUnweighted = 2.84;
    n.madSigma = 1.1;
    n.hasPeak = true;
    n.peakX = 21;
    n.peakY = 40;
    return n;
}

BackendRegionProfileResult profile()
{
    BackendRegionProfileResult p;
    p.valid = true;
    p.offsetUnit = u"arcsec"_s;
    p.bunit = u"Jy/beam"_s;
    p.widthPix = 1;
    p.lengthPix = 3.0;
    p.offsets = { 0.0, 1.5, 3.0 };
    p.values = { 1.0, std::numeric_limits<double>::quiet_NaN(), 3.0 };
    p.x = { 10.0, 11.5, 13.0 };
    p.y = { 20.0, 20.0, 20.0 };
    p.validSamples = 2;
    return p;
}

}   // namespace

void TestRegionMeasure::rmsAndStdAreNamedApart()
{
    QVERIFY(rmsLabel() != stdLabel());
    QVERIFY(rmsLabel().contains(u"RMS"_s));
    QVERIFY(rmsLabel().contains(QChar(0x221a)));          // the root of <x^2>
    QCOMPARE(stdLabel(), u"Std dev"_s);
}

void TestRegionMeasure::unweightedRmsFromTheSumOfSquares()
{
    // {3, 3, 3}: mean 3, standard deviation 0, but RMS 3.
    QCOMPARE(rmsFromSumSq(27.0, 3), 3.0);
    // {-1, 1}: mean 0, so RMS and standard deviation are one number.
    QCOMPARE(rmsFromSumSq(2.0, 2), 1.0);
    QCOMPARE(rmsFromSumSq(5.0, 0), 0.0);
}

void TestRegionMeasure::statisticsRowsCarryTheSumInTheImageUnit()
{
    const Rows rows = statisticsRows(someNumbers(), u"Jy/beam"_s);
    QCOMPARE(valueOf(rows, u"Sum"_s), u"196.5 Jy/beam"_s);
    QCOMPARE(valueOf(rows, rmsLabel()), u"2.84"_s);
    QCOMPARE(valueOf(rows, stdLabel()), u"1.25"_s);
    QCOMPARE(valueOf(rows, u"Valid pixels"_s), u"77 / 80"_s);
    // A backend that predates the unweighted RMS: no row, not a zero.
    Numbers old = someNumbers();
    old.hasRmsUnweighted = false;
    QVERIFY(!has(statisticsRows(old, u"Jy/beam"_s), rmsLabel()));
    QVERIFY(has(statisticsRows(old, u"Jy/beam"_s), stdLabel()));
    // No unit: the bare number.
    QCOMPARE(valueOf(statisticsRows(someNumbers(), QString()), u"Sum"_s), u"196.5"_s);
}

void TestRegionMeasure::peakRowsGivePixelAndWorld()
{
    const Rows rows = peakRows(someNumbers(), u"Jy/beam"_s, u"RA 13:25:27.6  Dec -43:01:09"_s);
    QCOMPARE(valueOf(rows, u"Peak"_s), u"9 Jy/beam"_s);
    QVERIFY(valueOf(rows, u"Peak position"_s).startsWith(u"pixel (21, 40)"_s));
    QVERIFY(valueOf(rows, u"Peak position"_s).contains(u"13:25:27.6"_s));
    // Without a sky position the pixel stands alone.
    QCOMPARE(valueOf(peakRows(someNumbers(), QString(), QString()), u"Peak position"_s),
             u"pixel (21, 40)"_s);
    // A backend that sent no peak pixel: the value only.
    Numbers n = someNumbers();
    n.hasPeak = false;
    QVERIFY(!has(peakRows(n, QString(), QString()), u"Peak position"_s));
}

void TestRegionMeasure::pointRowsSayBlank()
{
    const Rows rows = pointRows(0.00123, 12.0, 30.0, u"RA 1 Dec 2"_s, u"Jy/beam"_s, false);
    QCOMPARE(valueOf(rows, u"Value"_s), u"0.00123 Jy/beam"_s);
    QCOMPARE(valueOf(rows, u"Position"_s), u"pixel (12, 30)  ·  RA 1 Dec 2"_s);
    const Rows blank = pointRows(0.0, 1.0, 2.0, QString(), u"Jy/beam"_s, true);
    QCOMPARE(valueOf(blank, u"Value"_s), u"blank (NaN)"_s);
}

void TestRegionMeasure::pointAndLineRegionsAreRegionsOfTheList()
{
    const auto point = pointRegion(10.4, 20.6);
    QCOMPARE(point.shape, Shape::Point);
    QCOMPARE(point.center[0], 10.4);
    // The canonical form the backend takes, and what a region file writes.
    const QJsonObject pc = visivo::regions::toCanonical(point, 0.0);
    QCOMPARE(pc.value(u"shape"_s).toString(), u"point"_s);
    QCOMPARE(pc.value(u"center"_s).toArray().at(1).toDouble(), 20.6);

    const auto line = lineRegion(1.0, 2.0, 31.0, 42.0);
    QVERIFY(line.has_value());
    QCOMPARE(line->shape, Shape::Line);
    QCOMPARE(int(line->points.size()), 2);
    const QJsonObject lc = visivo::regions::toCanonical(*line, 0.0);
    QCOMPARE(lc.value(u"shape"_s).toString(), u"line"_s);
    QCOMPARE(lc.value(u"points"_s).toArray().size(), 2);

    // Both come back from their canonical form unchanged.
    visivo::regions::Region back;
    QVERIFY(visivo::regions::fromCanonical(lc, 0.0, &back));
    QCOMPARE(back.shape, Shape::Line);
    QCOMPARE(back.points.back()[1], 42.0);
    visivo::regions::Region pback;
    QVERIFY(visivo::regions::fromCanonical(pc, 0.0, &pback));
    QCOMPARE(pback.shape, Shape::Point);
    // A line has no pixels of its own: the area tools skip it.
    QVERIFY(!visivo::regions::hasArea(Shape::Line));
    QVERIFY(visivo::regions::hasArea(Shape::Point));
}

void TestRegionMeasure::aLineThatWentNowhereIsNotARegion()
{
    QVERIFY(!lineRegion(5.0, 5.0, 5.0, 5.0).has_value());
    QVERIFY(!lineRegion(5.0, 5.0, 5.2, 5.2).has_value());
    QVERIFY(lineRegion(5.0, 5.0, 6.0, 5.0).has_value());
}

void TestRegionMeasure::profileRowsSummariseTheLine()
{
    const Rows rows = profileRows(profile());
    QCOMPARE(valueOf(rows, u"Length"_s), u"3.00\""_s);
    QVERIFY(valueOf(rows, u"Samples"_s).startsWith(u"2 valid of 3"_s));
    QCOMPARE(valueOf(rows, u"Min"_s), u"1 Jy/beam"_s);
    QCOMPARE(valueOf(rows, u"Max"_s), u"3 Jy/beam"_s);
    BackendRegionProfileResult p = profile();
    p.offsetUnit = u"pix"_s;
    QCOMPARE(valueOf(profileRows(p), u"Length"_s), u"3.00 px"_s);
}

void TestRegionMeasure::noiseRowsOfAClippedImage()
{
    BackendImageNoiseResult r;
    r.valid = true;
    r.mode = u"auto"_s;
    r.bunit = u"Jy/beam"_s;
    r.rms = 3.1e-5;
    r.madSigma = 2.7e-5;
    r.beamAreaPix = 42.24;
    r.rmsPerPixel = r.rms / r.beamAreaPix;
    r.madSigmaPerPixel = r.madSigma / r.beamAreaPix;
    r.validCount = 64436274;
    r.usedCount = 62745125;
    r.clipSigma = 3.0;
    r.exact = false;
    r.sampleCount = 4194304;
    const Rows rows = noiseRows(r);
    QCOMPARE(valueOf(rows, rmsLabel()), u"3.1e-05 Jy/beam"_s);
    QCOMPARE(valueOf(rows, madLabel()), u"2.7e-05 Jy/beam"_s);
    QCOMPARE(valueOf(rows, u"Beam area"_s), u"42.24 pixels"_s);
    QVERIFY(valueOf(rows, u"RMS per pixel"_s).endsWith(u" Jy/pixel"_s));
    QVERIFY(valueOf(rows, u"RMS per pixel"_s).startsWith(u"7.3"_s));
    const QString from = valueOf(rows, u"From"_s);
    QVERIFY(from.contains(u"3σ-clipped"_s));
    QVERIFY(from.contains(u"62,745,125 of 64,436,274"_s));
    QVERIFY(from.contains(u"random sample"_s));
    // An exact clip (every pixel) does not talk about a sample.
    r.exact = true;
    QVERIFY(!valueOf(noiseRows(r), u"From"_s).contains(u"sample"_s));
}

void TestRegionMeasure::noiseRowsOfBackgroundRegions()
{
    BackendImageNoiseResult r;
    r.valid = true;
    r.mode = u"regions"_s;
    r.bunit = u"Jy/beam"_s;
    r.rms = 2.0e-5;
    r.madSigma = 1.9e-5;
    r.rmsScatter = 1.0e-6;
    r.validCount = 20000;
    BackendImageNoiseResult::PerRegion a, b, c;
    a.name = u"north"_s;  a.validCount = 10000; a.measured = true; a.rms = 1.95e-5; a.madSigma = 1.85e-5;
    b.name = u"south"_s;  b.validCount = 10000; b.measured = true; b.rms = 2.05e-5; b.madSigma = 1.95e-5;
    c.name = u"empty"_s;  c.validCount = 1;
    r.regions = { a, b, c };
    const Rows rows = noiseRows(r);
    QVERIFY(valueOf(rows, u"From"_s).contains(u"average of 3 background regions"_s));
    QVERIFY(valueOf(rows, u"Scatter between regions"_s).contains(u"1e-06"_s));
    QVERIFY(valueOf(rows, u"north"_s).contains(u"RMS 1.95e-05"_s));
    QVERIFY(valueOf(rows, u"empty"_s).contains(u"not used"_s));
    // Each region has its own row, in order.
    int first = -1, last = -1;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].first == u"north"_s) first = i;
        if (rows[i].first == u"empty"_s) last = i;
    }
    QVERIFY(first >= 0 && last > first);
}

void TestRegionMeasure::noiseWithoutABeamHasNoConversion()
{
    BackendImageNoiseResult r;
    r.valid = true;
    r.mode = u"auto"_s;
    r.bunit = u"K"_s;
    r.rms = 0.5;
    r.madSigma = 0.45;
    r.clipSigma = 3.0;
    r.exact = true;
    r.validCount = 100;
    r.usedCount = 99;
    const Rows rows = noiseRows(r);
    QVERIFY(!has(rows, u"Beam area"_s));
    QVERIFY(!has(rows, u"RMS per pixel"_s));
    QCOMPARE(valueOf(rows, rmsLabel()), u"0.5 K"_s);
}

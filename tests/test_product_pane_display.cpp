#include "TestProductPaneDisplay.h"

#include "gui/ProductPaneDisplay.h"

#include <QtTest>

#include <vector>

using namespace Qt::Literals::StringLiterals;
using LutScale::Mode;

void TestProductPaneDisplay::segmentsRoundTrip()
{
    // The control lists Linear, Log, Sqrt in that order, on the slice and on
    // every map pane alike.
    for (const Mode m : { Mode::Linear, Mode::Log, Mode::Sqrt })
        QCOMPARE(visivo::scaleForSegment(visivo::scaleSegment(m)), m);
    QCOMPARE(visivo::scaleSegment(Mode::Log), 1);
    QCOMPARE(visivo::scaleSegment(Mode::Sqrt), 2);
    QCOMPARE(visivo::scaleForSegment(-1), Mode::Linear);   // nothing selected
}

void TestProductPaneDisplay::editorOnlyStretchesShowAsLinear()
{
    // Square and Power come from the colour-map editor and have no segment.
    QCOMPARE(visivo::scaleSegment(Mode::Square), 0);
    QCOMPARE(visivo::scaleSegment(Mode::Power), 0);
}

void TestProductPaneDisplay::linearBarIsEven()
{
    QCOMPARE(LutScale::valueAt(Mode::Linear, 0.0, -2.0, 8.0), -2.0);
    QCOMPARE(LutScale::valueAt(Mode::Linear, 0.5, -2.0, 8.0), 3.0);
    QCOMPARE(LutScale::valueAt(Mode::Linear, 1.0, -2.0, 8.0), 8.0);
    // Outside the bar is the bar's end.
    QCOMPARE(LutScale::valueAt(Mode::Linear, 1.5, -2.0, 8.0), 8.0);
}

void TestProductPaneDisplay::logBarIsLogSpaced()
{
    // A column-density map over four decades: the middle of the bar is the
    // geometric mean, a decade a quarter of the way up.
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Log, 0.0, 1e18, 1e22), 1e18));
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Log, 0.25, 1e18, 1e22), 1e19));
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Log, 0.5, 1e18, 1e22), 1e20));
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Log, 1.0, 1e18, 1e22), 1e22));
}

void TestProductPaneDisplay::logBarOnSignedDataStartsAtTheFloor()
{
    // Noise below zero cannot be on a log bar: it starts where the table does
    // (rangeFor), six decades under the peak when no floor was sampled.
    const auto r = LutScale::rangeFor(Mode::Log, -0.5, 100.0);
    QVERIFY(qFuzzyCompare(r[0], 1e-4));
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Log, 0.0, -0.5, 100.0), 1e-4));
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Log, 1.0, -0.5, 100.0), 100.0));
    // A range with nothing positive: the bar still reads what the table holds,
    // the fallback 1e-12 .. 1e-6 (the views refuse Log before it comes to this).
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Log, 0.5, -3.0, -1.0), 1e-9));
}

void TestProductPaneDisplay::sqrtBarInvertsTheStretch()
{
    // Under Sqrt a value at normalised t takes the colour at sqrt(t): the
    // colour half-way up the map belongs to a quarter of the range.
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Sqrt, 0.5, 0.0, 100.0), 25.0));
    QVERIFY(qFuzzyCompare(LutScale::valueAt(Mode::Sqrt, 1.0, 0.0, 100.0), 100.0));
    QCOMPARE(LutScale::exponent(Mode::Sqrt, 2.0), 0.5);
    QVERIFY(LutScale::isResampled(Mode::Sqrt));
    QVERIFY(!LutScale::isResampled(Mode::Log));
}

void TestProductPaneDisplay::percentileFloorTakesTheFaintEnd()
{
    std::vector<double> v;
    for (int i = 1000; i >= 1; --i)   // unsorted on purpose
        v.push_back(i);
    QCOMPARE(LutScale::percentileFloor(v), 10.0);   // 1st percentile of 1..1000
    QCOMPARE(LutScale::percentileFloor({}), 0.0);   // nothing positive: no log
    QCOMPARE(LutScale::percentileFloor({ 0.5 }), 0.5);
}

void TestProductPaneDisplay::momentSummary()
{
    // The recipe a moment registers with (vtkWindowCube, Provenance params).
    const QVariantMap params{
        { u"Order"_s, u"Moment 0 (Integrated intensity)"_s },
        { u"Channels"_s, u"1–512"_s },
        { u"Mask"_s, u"none"_s },
        { u"Scope"_s, u"Full resolution"_s },
        { u"Unit"_s, u"Jy/beam km/s"_s },
    };
    QCOMPARE(visivo::productSummary(u"Moment 0"_s, params),
             u"Moment 0 · channels 1–512 · mask off"_s);
}

void TestProductPaneDisplay::maskedMomentSummary()
{
    QCOMPARE(visivo::productSummary(
                     u"Moment 1"_s,
                     { { u"Channels"_s, u"10..40"_s }, { u"Mask"_s, u"auto (≈ 3σ)"_s } }),
             u"Moment 1 · channels 10–40 · mask auto (≈ 3σ)"_s);
    QCOMPARE(visivo::productSummary(
                     u"Moment 0"_s,
                     { { u"Channels"_s, u"5–9"_s }, { u"Mask"_s, u">= 0.0012 (3σ estimate)"_s } }),
             u"Moment 0 · channels 5–9 · mask ≥ 0.0012 (3σ estimate)"_s);
}

void TestProductPaneDisplay::linewidthSummaryNamesTheMethodOnce()
{
    const QVariantMap fwhm{ { u"Method"_s, u"FWHM"_s }, { u"Channels"_s, u"3–60"_s },
                            { u"Mask"_s, u"none"_s }, { u"Type"_s, u"FWHM"_s } };
    QCOMPARE(visivo::productSummary(u"FWHM (km/s)"_s, fwhm),
             u"FWHM (km/s) · channels 3–60 · mask off"_s);
    QVariantMap ew = fwhm;
    ew.insert(u"Method"_s, u"Equivalent width"_s);
    QCOMPARE(visivo::productSummary(u"Line width"_s, ew),
             u"Line width · Equivalent width · channels 3–60 · mask off"_s);
}

void TestProductPaneDisplay::linewidthSummaryIsShort()
{
    // The labels and methods LinewidthDialog really registers: the unit and the
    // repeated "FWHM, " go, the method keeps a word and its first note.
    const QVariantMap fast{ { u"Method"_s, u"FWHM, moment-2 dispersion (fast)"_s },
                            { u"Channels"_s, u"1–158"_s }, { u"Mask"_s, u"none"_s },
                            { u"Unit"_s, u"m.s**-1"_s } };
    QCOMPARE(visivo::productSummary(u"FWHM [m.s**-1]"_s, fast),
             u"FWHM · moment-2 (fast) · channels 1–158 · mask off"_s);
    QVariantMap fit = fast;
    fit.insert(u"Method"_s, u"FWHM, Gaussian fit (slow, exact)"_s);
    QCOMPARE(visivo::productSummary(u"FWHM [km/s]"_s, fit),
             u"FWHM · Gaussian (slow) · channels 1–158 · mask off"_s);
    QVariantMap ew = fast;
    ew.insert(u"Method"_s, u"Equivalent width (fast, no fit)"_s);
    QCOMPARE(visivo::productSummary(u"EW [m.s**-1]"_s, ew),
             u"EW · Equivalent width · channels 1–158 · mask off"_s);
}

void TestProductPaneDisplay::pvAndRmSummaries()
{
    QCOMPARE(visivo::productSummary(u"PV diagram"_s,
                                    { { u"Path"_s, u"4 points"_s },
                                      { u"Width"_s, u"3 px"_s },
                                      { u"From"_s, u"(1.0, 2.0)"_s } }),
             u"PV diagram · path 4 points · width 3 px"_s);
    QCOMPARE(visivo::productSummary(u"Peak |F(φ)|"_s,
                                    { { u"φ range"_s, u"-50 … 50 rad m⁻²"_s },
                                      { u"φ step"_s, 1.0 },
                                      { u"Region"_s, u"x 10–20, y 5–9"_s } }),
             u"Peak |F(φ)| · φ -50 … 50 rad m⁻² · region x 10–20, y 5–9"_s);
}

void TestProductPaneDisplay::bareLabelWithoutRecipe()
{
    QCOMPARE(visivo::productSummary(u" Velocity field "_s, {}), u"Velocity field"_s);
    QCOMPARE(visivo::productSummary(QString(), {}), QString());
}

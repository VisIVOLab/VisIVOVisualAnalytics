// test_region_readout.cpp
// See TestRegionReadout.h.

#include "TestRegionReadout.h"

#include "RegionReadout.h"

#include <QTest>

using visivo::kRegionReadoutBudget;
using visivo::regionReadoutText;
using visivo::regionSampleStride;

void TestRegionReadout::aSmallRegionIsReadWhole()
{
    // Anything inside the budget is exact: no stride, no "≈".
    QCOMPARE(regionSampleStride(1), 1);
    QCOMPARE(regionSampleStride(kRegionReadoutBudget), 1);
}

void TestRegionReadout::aLargeRegionIsSampledWithinTheBudget()
{
    // The readout runs on every mouse move, so the cost has to be bounded: a
    // megapixel region must not be read pixel by pixel while the user drags.
    for (long long pixels : { 400000LL, 1000000LL, 16000000LL, 250000000LL }) {
        const int stride = regionSampleStride(pixels);
        QVERIFY(stride > 1);
        const long long touched = pixels / (static_cast<long long>(stride) * stride);
        QVERIFY2(touched <= kRegionReadoutBudget,
                 qPrintable(QStringLiteral("%1 pixels, stride %2 → %3 touched")
                                    .arg(pixels).arg(stride).arg(touched)));
    }
}

void TestRegionReadout::theStrideIsNeverZero()
{
    // A zero stride is an infinite loop in the sampler.
    QCOMPARE(regionSampleStride(0), 1);
    QCOMPARE(regionSampleStride(-5), 1);
    QCOMPARE(regionSampleStride(1000000, 0), 1);
}

void TestRegionReadout::theTextCarriesBothNumbers()
{
    const QString text = regionReadoutText(QStringLiteral("Circle"), 3410.0, 1.88, 1813, false);
    QVERIFY(text.contains(QStringLiteral("Circle")));
    QVERIFY(text.contains(QStringLiteral("Σ")));
    QVERIFY(text.contains(QStringLiteral("μ")));
    QVERIFY(!text.contains(QStringLiteral("≈")));
}

void TestRegionReadout::sampledFiguresAreMarkedAsEstimates()
{
    // A sampled sum is scaled up from a fraction of the pixels. Printed bare it
    // reads as the integrated flux, which it is not.
    const QString text = regionReadoutText(QStringLiteral("Box"), 3410.0, 1.88, 1000000, true);
    QVERIFY(text.contains(QStringLiteral("≈")));
}

void TestRegionReadout::anEmptyRegionSaysSoInsteadOfShowingZero()
{
    // Σ 0 over a region of blanked pixels is a measurement; "no valid pixels"
    // is the truth.
    const QString text = regionReadoutText(QStringLiteral("Box"), 0.0, 0.0, 0, false);
    QVERIFY(text.contains(QStringLiteral("no valid pixels")));
    QVERIFY(!text.contains(QStringLiteral("Σ")));
}

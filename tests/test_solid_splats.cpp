#include "TestSolidSplats.h"

#include "gui/SolidSplats.h"
#include "gui/ParticleSplatDefaults.h"

#include <QtTest/QtTest>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

// The calls SolidSplats makes on vtkDoubleArray and vtkLookupTable.
struct FakeArray {
    std::vector<double> v;
    int modified = 0;
    long long GetNumberOfTuples() const { return static_cast<long long>(v.size()); }
    void SetValue(long long i, double x) { v[static_cast<std::size_t>(i)] = x; }
    void Modified() { ++modified; }
};

struct FakeLut {
    std::vector<std::array<double, 4>> table;
    std::array<double, 2> range{ 0.0, 0.0 };
    std::array<double, 4> nan{ 0.5, 0.0, 0.0, 1.0 };   // VTK's default: dark red
    void SetNumberOfTableValues(int n) { table.assign(static_cast<std::size_t>(n), { 0, 0, 0, 0 }); }
    void SetTableRange(double a, double b) { range = { a, b }; }
    void SetTableValue(int i, double r, double g, double b, double a)
    {
        table[static_cast<std::size_t>(i)] = { r, g, b, a };
    }
    void SetNanColor(double r, double g, double b, double a) { nan = { r, g, b, a }; }
};

} // namespace

void TestSolidSplats::staleNaNScalarsAreClearedWhenGoingSolid()
{
    // A column with NaN coloured the splats; then "none": the scalars the
    // mapper keeps must not hold NaN (undefined opacity, NaN colour).
    FakeArray scalars;
    scalars.v = { 1.0, std::numeric_limits<double>::quiet_NaN(), 3.5, -2.0,
                  std::numeric_limits<double>::infinity() };
    SolidSplats::clearScalars(&scalars);
    for (double x : scalars.v)
        QCOMPARE(x, 0.0);
    QCOMPARE(scalars.modified, 1);
}

void TestSolidSplats::theTableMapsEverythingNaNIncludedToTheSolidColour()
{
    FakeLut lut;
    SolidSplats::configureTable(&lut);
    QCOMPARE(int(lut.table.size()), 2);
    for (const auto &c : lut.table) {
        QCOMPARE(c[0], SolidSplats::kRed);
        QCOMPARE(c[1], SolidSplats::kGreen);
        QCOMPARE(c[2], SolidSplats::kBlue);
        QCOMPARE(c[3], 1.0);
    }
    QCOMPARE(lut.nan[0], SolidSplats::kRed);
    QCOMPARE(lut.nan[1], SolidSplats::kGreen);
    QCOMPARE(lut.nan[2], SolidSplats::kBlue);
    QVERIFY(lut.range[0] <= -1.0e15 && lut.range[1] >= 1.0e15);
}

void TestSolidSplats::gaussianDefaultsAvoidImmediateSaturation()
{
    QCOMPARE(ParticleSplatDefaults::initialSliderValue(), 25);
    QCOMPARE(3.0 * ParticleSplatDefaults::initialSliderValue() / 100.0, 0.75);
}

void TestSolidSplats::gaussianOpacityAdaptsToSightlineDepth()
{
    using namespace ParticleSplatDefaults;
    // Whole 8-bit steps of 1.5 / depth, half up, from 8 to 64.
    QCOMPARE(opacityForDepth(1.0), 64.0 / 255.0);
    QCOMPARE(opacityForDepth(30.0), 13.0 / 255.0);    // 12.75 steps
    QCOMPARE(opacityForDepth(0.0), 64.0 / 255.0);
    QCOMPARE(opacityForDepth(std::numeric_limits<double>::quiet_NaN()), 64.0 / 255.0);
    // A million-row cube (depth 100, 3.8 steps) and a deep one (0.4 steps)
    // keep at least eight steps: their dark colours still add something.
    QCOMPARE(alphaStepsForDepth(100.0), kMinAlphaSteps);
    QCOMPARE(alphaStepsForDepth(1000.0), kMinAlphaSteps);
    QCOMPARE(opacityForDepth(1000.0), 8.0 / 255.0);
}

void TestSolidSplats::gaussianOpacityIsStoredOnItsStep()
{
    using namespace ParticleSplatDefaults;
    // Every step survives VTK's truncating 8-bit store, also through float
    // (its opacity table is float), and so does rounding.
    for (int k = kMinAlphaSteps; k <= kMaxAlphaSteps; ++k) {
        const double value = vtkOpacityValue(k / 255.0);
        QCOMPARE(storedAlphaSteps(value), k);
        QCOMPARE(storedAlphaSteps(double(float(value))), k);
        QCOMPARE(int(std::lround(value * 255.0)), k);
    }
    // The former floor, 0.005, was stored as one step: 22 % low.
    QCOMPARE(storedAlphaSteps(0.005), 1);
    // No depth gives an effective alpha below eight steps.
    for (double depth = 1.0; depth < 1.0e6; depth *= 1.37)
        QVERIFY(storedAlphaSteps(vtkOpacityValue(opacityForDepth(depth))) >= kMinAlphaSteps);
}

void TestSolidSplats::splatSizeDefaultDoesNotCarryOverToPoints()
{
    using namespace ParticleSplatDefaults;
    // The automatic Gaussian size leaves with the splats and comes back with them.
    QCOMPARE(automaticSliderValue(false, initialSliderValue()), kDefaultSliderValue);
    QCOMPARE(automaticSliderValue(true, kDefaultSliderValue), initialSliderValue());
    // Already right for the shape, or another value: kept.
    QCOMPARE(automaticSliderValue(true, initialSliderValue()), initialSliderValue());
    QCOMPARE(automaticSliderValue(false, kDefaultSliderValue), kDefaultSliderValue);
    QCOMPARE(automaticSliderValue(false, 60), 60);
    QCOMPARE(automaticSliderValue(true, 60), 60);
}

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

void TestSolidSplats::theVr8BitRuleKeepsItsSteps()
{
    using namespace ParticleSplatDefaults;
    // Whole 8-bit steps of 1.5 / depth, half up, from 8 to 64.
    QCOMPARE(alphaStepsForDepth(1.0), kMaxAlphaSteps);
    QCOMPARE(alphaStepsForDepth(30.0), 13);    // 12.75 steps
    QCOMPARE(alphaStepsForDepth(0.0), kMaxAlphaSteps);
    QCOMPARE(alphaStepsForDepth(std::numeric_limits<double>::quiet_NaN()), kMaxAlphaSteps);
    QCOMPARE(alphaStepsForDepth(100.0), kMinAlphaSteps);
    QCOMPARE(alphaStepsForDepth(1000.0), kMinAlphaSteps);
    // A hair from a step's boundary: the same steps as point_splat.py
    // (test_point_splat.py) and the VR page (layers.test.mjs).
    QCOMPARE(alphaStepsForDepth(12.966), 30);
    QCOMPARE(alphaStepsForDepth(6.4286), 59);
    QCOMPARE(alphaStepsForDepth(6.3223), 61);
    QCOMPARE(alphaStepsForDepth(6.0236), 64);
    QCOMPARE(alphaStepsForDepth(40.0, 70.0), 19);   // 19.125 steps
}

void TestSolidSplats::gaussianOpacityIsUnquantisedWithNoFloor()
{
    using namespace ParticleSplatDefaults;
    // point_splat.splat_opacity(depth, intensity, steps=False).
    QCOMPARE(opacityForDepth(30.0), 1.5 / 30.0);
    QCOMPARE(opacityForDepth(1000.0), 1.5 / 1000.0);
    QCOMPARE(opacityForDepth(1.0e6), 1.5 / 1.0e6);   // the former floors were 8 / 255 and 0.005
    QCOMPARE(opacityForDepth(1.0), kMaxOpacity);
    QCOMPARE(opacityForDepth(0.0), kMaxOpacity);
    QCOMPARE(opacityForDepth(std::numeric_limits<double>::quiet_NaN()), kMaxOpacity);
    QCOMPARE(opacityForDepth(40.0, 70.0), 1.5 * 70.0 / (35.0 * 40.0));
    QCOMPARE(kIntensity, 35.0);
    QCOMPARE(kMaxOpacity, 0.25);
}

void TestSolidSplats::theSplatShaderMatchesThePointSession()
{
    using namespace ParticleSplatDefaults;
    // The literal the backend pins for point_splat.vtk_splat_shader(1.5 / 35 /
    // 1000) in test_point_render.py (issue #457).
    const std::string code = splatShaderCode(1.5 / 35.0 / 1000.0);
    QCOMPARE(QString::fromStdString(code),
             QStringLiteral("//VTK::Color::Impl\n"
                            "  float dist2 = dot(offsetVCVSOutput.xy, offsetVCVSOutput.xy);\n"
                            "  opacity = opacity * exp(-0.5 * dist2) * 4.285714286e-05;\n"));
    QVERIFY(splatShaderCode(0.25).find("* 2.500000000e-01;") != std::string::npos);
    QVERIFY(splatShaderCode(0.0).find("* 0.000000000e+00;") != std::string::npos);
    for (double bad : { -1.0e-3, std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::infinity() })
        QVERIFY(splatShaderCode(bad).empty());
}

void TestSolidSplats::theRenderStrideKeepsTheBudget()
{
    using namespace ParticleSplatDefaults;
    QCOMPARE(renderStride(999, 1000), 1LL);
    QCOMPARE(renderStride(1000, 1000), 1LL);
    QCOMPARE(renderStride(1001, 1000), 2LL);
    QCOMPARE(renderStride(3660000, 1000000), 4LL);
    QCOMPARE(renderStride(30000000, 1000000), 30LL);
    QCOMPARE(renderStride(30000000, 0), 1LL);
    for (long long n : { 1500000LL, 3660000LL, 10000000LL, 29999999LL }) {
        const long long stride = renderStride(n, 1000000);
        QVERIFY((n + stride - 1) / stride <= 1000000);
    }
}

void TestSolidSplats::splatLightDoesNotDependOnTheRows()
{
    using namespace ParticleSplatDefaults;
    // The light of a cloud is rows x gain x radius^2 (the kernel's integral):
    // for one box it must not change with the rows, nor with a level of
    // detail that draws table / stride of them, while no splat reaches the
    // opacity cap.
    const double box[6] = { -10.0, 10.0, -10.0, 10.0, -10.0, 10.0 };
    const auto light = [&](double rows) {
        const SplatStyle s = splatStyle(box, rows, 0.75);
        return rows * s.gain * s.radius * s.radius;
    };
    const double reference = light(1.0e5);
    for (double rows : { 1.0e6, 3.66e6, 1.0e7, 3.0e7, 3.0e7 / 30.0, 3.66e6 / 4.0, 1.0e9 })
        QVERIFY2(std::abs(light(rows) / reference - 1.0) < 1.0e-9,
                 qPrintable(QString::number(rows)));
    // The point session's rule: spacing cbrt(volume / rows), depth longest / spacing.
    const SplatStyle s = splatStyle(box, 1.0e6, 0.75);
    QVERIFY(std::abs(s.depth - 100.0) < 1.0e-9);
    QVERIFY(std::abs(s.radius - 0.2 * 0.75 / 8.0) < 1.0e-12);
    QCOMPARE(s.gain, 1.5 / s.depth);
    // A flat cloud counts its thin axis as a thousandth of the longest.
    const double flat[6] = { 0.0, 10.0, 0.0, 10.0, 0.0, 0.0 };
    QVERIFY(std::abs(splatStyle(flat, 1.0e3, 8.0).radius - std::cbrt(1.0e-2 * 1.0e2 / 1.0e3))
            < 1.0e-12);
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

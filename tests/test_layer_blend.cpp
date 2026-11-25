// test_layer_blend.cpp
// See TestLayerBlend.h.

#include "TestLayerBlend.h"

#include "LayerBlend.h"

#include <QtTest>

#include <cmath>
#include <limits>
#include <vector>

using namespace visivo::blend;

namespace {

Rgba8 sum(std::initializer_list<std::array<std::uint8_t, 4>> colours,
          std::initializer_list<double> weights)
{
    Accum acc;
    auto w = weights.begin();
    for (const auto &c : colours)
        accumulate(acc, c.data(), *w++);
    return finish(acc);
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

}   // namespace

void TestLayerBlend::threeBlackToHueLayersSumToTheirMix()
{
    // Halfway up three single-hue maps: each layer's own colour, mixed.
    const Rgba8 p = sum({ { 128, 0, 0, 255 }, { 0, 64, 0, 255 }, { 0, 0, 255, 255 } }, { 1, 1, 1 });
    QCOMPARE(int(p.r), 128);
    QCOMPARE(int(p.g), 64);
    QCOMPARE(int(p.b), 255);
    QCOMPARE(int(p.a), 255);
}

void TestLayerBlend::equalFullLayersReachWhite()
{
    // The reason for the mode: normal blending never gets here.
    const Rgba8 p = sum({ { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } }, { 1, 1, 1 });
    QCOMPARE(int(p.r), 255);
    QCOMPARE(int(p.g), 255);
    QCOMPARE(int(p.b), 255);
}

void TestLayerBlend::aSumPastOneClips()
{
    const Rgba8 p = sum({ { 200, 10, 0, 255 }, { 200, 10, 0, 255 } }, { 1, 1 });
    QCOMPARE(int(p.r), 255);   // 400/255 clipped, not wrapped
    QCOMPARE(int(p.g), 20);
    QCOMPARE(int(p.b), 0);
}

void TestLayerBlend::opacityIsTheWeightOfTheLayer()
{
    const Rgba8 half = sum({ { 255, 0, 0, 255 }, { 0, 200, 0, 255 } }, { 0.5, 1.0 });
    QCOMPARE(int(half.r), 128);   // 127.5 rounds to 128
    QCOMPARE(int(half.g), 200);
    // The colour's own alpha scales it too.
    const Rgba8 faint = sum({ { 255, 0, 0, 128 } }, { 1.0 });
    QCOMPARE(int(faint.r), 128);
}

void TestLayerBlend::aZeroWeightLayerAddsNothing()
{
    const Rgba8 p = sum({ { 255, 0, 0, 255 }, { 0, 255, 0, 255 } }, { 1.0, 0.0 });
    QCOMPARE(int(p.g), 0);
    const Rgba8 negative = sum({ { 0, 255, 0, 255 } }, { -1.0 });   // never subtracts
    QCOMPARE(int(negative.g), 0);
}

void TestLayerBlend::aLayerWithoutDataLeavesThePixelTransparent()
{
    // Nothing accumulated: the image underneath shows through.
    const Rgba8 p = finish(Accum{});
    QCOMPARE(int(p.a), 0);
}

void TestLayerBlend::aBlackPixelWithDataIsOpaque()
{
    // A layer at its minimum is black, and covers what is below it.
    const Rgba8 p = sum({ { 0, 0, 0, 255 } }, { 1.0 });
    QCOMPARE(int(p.a), 255);
    QCOMPARE(int(p.r) + int(p.g) + int(p.b), 0);
}

void TestLayerBlend::threeLayersAreRedGreenBlue()
{
    QCOMPARE(QString(hueMapName(0, 3)), QStringLiteral("PureRed"));
    QCOMPARE(QString(hueMapName(1, 3)), QStringLiteral("PureGreen"));
    QCOMPARE(QString(hueMapName(2, 3)), QStringLiteral("PureBlue"));
    QCOMPARE(QString(hueMapName(0, 2)), QStringLiteral("PureRed"));
    QCOMPARE(QString(hueMapName(1, 2)), QStringLiteral("PureBlue"));
}

void TestLayerBlend::fourAndFiveLayersRunRedToBlue()
{
    // Four: the first and last keep red and blue, the middle two sit between.
    QCOMPARE(QString(hueMapName(0, 4)), QStringLiteral("PureRed"));
    QCOMPARE(QString(hueMapName(1, 4)), QStringLiteral("PureYellow"));
    QCOMPARE(QString(hueMapName(2, 4)), QStringLiteral("PureCyan"));
    QCOMPARE(QString(hueMapName(3, 4)), QStringLiteral("PureBlue"));
    const char *five[] = { "PureRed", "PureYellow", "PureGreen", "PureCyan", "PureBlue" };
    for (int i = 0; i < 5; ++i)
        QCOMPARE(QString(hueMapName(i, 5)), QString(five[i]));
}

void TestLayerBlend::moreThanFiveLayersCycle()
{
    QCOMPARE(QString(hueMapName(0, 8)), QStringLiteral("PureRed"));
    QCOMPARE(QString(hueMapName(5, 8)), QStringLiteral("PureMagenta"));
    QCOMPARE(QString(hueMapName(6, 8)), QStringLiteral("PureRed"));
    // Out-of-range input is clamped, never undefined.
    QCOMPARE(QString(hueMapName(-3, 3)), QStringLiteral("PureRed"));
    QCOMPARE(QString(hueMapName(9, 3)), QStringLiteral("PureBlue"));
    QCOMPARE(QString(hueMapName(0, 1)), QStringLiteral("PureRed"));
}

void TestLayerBlend::sameGridResamplesUnchanged()
{
    const std::vector<float> src = { 1, 2, 3, 4, 5, 6 };   // 3 x 2
    std::vector<float> out(6);
    resampleRows({ src.data(), 3, 2 }, Affine2D{}, 1, 3, 0, 2, out.data());
    for (std::size_t i = 0; i < src.size(); ++i)
        QCOMPARE(out[i], src[i]);
    // A band of rows reads the same pixels as the whole.
    std::vector<float> band(3);
    resampleRows({ src.data(), 3, 2 }, Affine2D{}, 1, 3, 1, 2, band.data());
    QCOMPARE(band[0], 4.f);
    QCOMPARE(band[2], 6.f);
}

void TestLayerBlend::aShiftedLayerLandsOnTheBaseGrid()
{
    // The layer's pixel (0,0) sits at base pixel (2,1).
    const std::vector<float> src = { 10, 20, 30, 40 };   // 2 x 2
    Affine2D toBase;
    toBase.e = 2;
    toBase.f = 1;
    std::vector<float> out(5 * 4);
    resampleRows({ src.data(), 2, 2 }, toBase, 1, 5, 0, 4, out.data());
    QCOMPARE(out[1 * 5 + 2], 10.f);
    QCOMPARE(out[1 * 5 + 3], 20.f);
    QCOMPARE(out[2 * 5 + 2], 30.f);
    QCOMPARE(out[2 * 5 + 3], 40.f);
    QVERIFY(std::isnan(out[0]));
    QVERIFY(std::isnan(out[1 * 5 + 1]));
    QVERIFY(std::isnan(out[3 * 5 + 3]));
}

void TestLayerBlend::aCoarserLayerIsScaledToTheBaseGrid()
{
    // A layer whose pixels are twice the base's: one layer pixel covers 2 x 2
    // base pixels, so base pixel 2 reads layer pixel 1 (a map x' = 2x).
    const std::vector<float> src = { 1, 2, 3, 4 };   // 2 x 2
    Affine2D toBase;
    toBase.a = 2;
    toBase.d = 2;
    std::vector<float> out(4 * 4);
    resampleRows({ src.data(), 2, 2 }, toBase, 1, 4, 0, 4, out.data());
    QCOMPARE(out[0], 1.f);   // base (0,0) is layer (0,0)
    QCOMPARE(out[2], 2.f);   // base (2,0) is layer (1,0)
    QCOMPARE(out[2 * 4 + 2], 4.f);
    // Between layer pixel centres the value is interpolated.
    QVERIFY(out[1] > 1.f && out[1] < 2.f);
}

void TestLayerBlend::aRotatedLayerIsSampledWhereItLies()
{
    // Turned 90 degrees counter-clockwise about its corner: (x, y) -> (-y, x),
    // then shifted by +3 in x so it stays on the grid.
    const std::vector<float> src = { 1, 2, 3, 4, 5, 6 };   // 3 wide, 2 high
    Affine2D toBase;
    toBase.a = 0;
    toBase.b = -1;
    toBase.c = 1;
    toBase.d = 0;
    toBase.e = 3;
    std::vector<float> out(4 * 4, -1.f);
    resampleRows({ src.data(), 3, 2 }, toBase, 1, 4, 0, 4, out.data());
    // Layer pixel (2,1) = 6 goes to (-1+3, 2) = (2,2); (0,0) = 1 to (3,0).
    QCOMPARE(out[2 * 4 + 2], 6.f);
    QCOMPARE(out[0 * 4 + 3], 1.f);
    QCOMPARE(out[1 * 4 + 3], 2.f);
    QVERIFY(std::isnan(out[0]));
}

void TestLayerBlend::outsideTheLayerIsBlankAndBlanksDoNotBleed()
{
    const std::vector<float> src = { 1, kNaN, 1, 1 };   // 2 x 2, one blank
    const FloatImage img{ src.data(), 2, 2 };
    QVERIFY(std::isnan(sampleBilinear(img, -0.6, 0)));
    QVERIFY(std::isnan(sampleBilinear(img, 0, 1.6)));
    // Between a blank and a value, the value is not pulled towards zero.
    QCOMPARE(sampleBilinear(img, 0.5, 0.0), 1.f);
    const std::vector<float> allBlank = { kNaN, kNaN };
    QVERIFY(std::isnan(sampleBilinear({ allBlank.data(), 2, 1 }, 0.5, 0)));
}

void TestLayerBlend::aSingularMapGivesBlank()
{
    const std::vector<float> src = { 1, 2, 3, 4 };
    Affine2D flat;
    flat.a = 0;
    flat.d = 0;
    std::vector<float> out(4, 0.f);
    resampleRows({ src.data(), 2, 2 }, flat, 1, 2, 0, 2, out.data());
    for (float v : out)
        QVERIFY(std::isnan(v));
}

void TestLayerBlend::reducedDisplayAveragesTheBlock()
{
    // 4 x 4 of a checkerboard shown at step 4: one pixel, near the mean.
    std::vector<float> src(16);
    for (int i = 0; i < 16; ++i)
        src[static_cast<std::size_t>(i)] = ((i % 4) + (i / 4)) % 2 ? 10.f : 0.f;
    float out = -1.f;
    resampleRows({ src.data(), 4, 4 }, Affine2D{}, 4, 1, 0, 1, &out);
    QVERIFY(std::fabs(out - 5.f) < 1.5f);
}

void TestLayerBlend::displayStepFitsTheBudget()
{
    QCOMPARE(displayStep(1000, 1000, 2'000'000), 1);
    const int step = displayStep(10000, 10000, 2'500'000);
    QVERIFY(step > 1);
    QVERIFY(static_cast<long long>((10000 + step - 1) / step) * ((10000 + step - 1) / step)
            <= 2'500'000);
    QCOMPARE(displayStep(0, 0, 100), 1);
}

void TestLayerBlend::affineInverseRoundTrips()
{
    Affine2D m;
    m.a = 0.6;
    m.b = -0.8;
    m.c = 0.8;
    m.d = 0.6;
    m.e = 5;
    m.f = -2;
    bool ok = false;
    const Affine2D inv = m.inverse(&ok);
    QVERIFY(ok);
    const auto p = m.apply(3.5, 7.25);
    const auto q = inv.apply(p[0], p[1]);
    QVERIFY(std::fabs(q[0] - 3.5) < 1e-9 && std::fabs(q[1] - 7.25) < 1e-9);
    // after(): first, then this.
    const auto r = inv.after(m).apply(1.0, 2.0);
    QVERIFY(std::fabs(r[0] - 1.0) < 1e-9 && std::fabs(r[1] - 2.0) < 1e-9);
}

void TestLayerBlend::clampToRangeKeepsNanAndTakesEitherOrder()
{
    const float v[] = { -5.f, 0.5f, 9.f, std::nanf("") };
    float out[4];
    clampToRange(v, 4, 0.0, 1.0, out);
    QCOMPARE(out[0], 0.f);
    QCOMPARE(out[1], 0.5f);
    QCOMPARE(out[2], 1.f);
    QVERIFY(std::isnan(out[3]));
    clampToRange(v, 4, 1.0, 0.0, out);   // a reversed range clamps the same way
    QCOMPARE(out[2], 1.f);
}

void TestLayerBlend::noDataIsTransparentWhateverTheTableGaveNan()
{
    const float v[] = { 1.f, std::nanf("") };
    std::uint8_t rgba[8] = { 10, 20, 30, 255, 1, 2, 3, 255 };
    clearNoData(v, 2, rgba);
    QCOMPARE(int(rgba[3]), 255);   // data keeps its colour
    QCOMPARE(int(rgba[0]), 10);
    QCOMPARE(int(rgba[4]) + rgba[5] + rgba[6] + rgba[7], 0);
}

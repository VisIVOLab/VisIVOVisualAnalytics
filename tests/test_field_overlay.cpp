// test_field_overlay.cpp
// See TestFieldOverlay.h.

#include "TestFieldOverlay.h"

#include "FieldOverlay.h"

#include <QtTest>

#include <array>
#include <cmath>
#include <vector>

using namespace visivo::fields;
using visivo::blend::Rgba8;

namespace {

using Px = std::array<std::uint8_t, 4>;

Rgba8 over(std::initializer_list<std::pair<Px, double>> layers, Layout layout = Layout::Layered)
{
    std::vector<Px> store;
    for (const auto &l : layers)
        store.push_back(l.first);
    std::vector<ColourLayer> v;
    std::size_t i = 0;
    for (const auto &l : layers)
        v.push_back({ store[i++].data(), l.second });
    Rgba8 out;
    compose(layout, v, 1, &out);
    return out;
}

bool same(const Rgba8 &c, int r, int g, int b, int a, int tol = 1)
{
    return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol
            && std::abs(c.a - a) <= tol;
}

}   // namespace

void TestFieldOverlay::aPositiveFieldOverThreeDecadesPrefersLog()
{
    QVERIFY(prefersLog(1e-12, 1e-3));
    QVERIFY(prefersLog(2.0, 2000.0));
    QVERIFY(!prefersLog(1.0, 999.0));   // under three decades
}

void TestFieldOverlay::aSignedOrNarrowFieldStaysLinear()
{
    QVERIFY(!prefersLog(-1.0, 1e6));    // noise around zero: a radio cube
    QVERIFY(!prefersLog(0.0, 1e6));     // zero is not positive
    QVERIFY(!prefersLog(1.0, std::nan("")));
}

void TestFieldOverlay::quantileIgnoresBlanksAndClamps()
{
    std::vector<float> v{ 5, NAN, 1, 3, INFINITY, 2, 4 };
    QCOMPARE(quantile(v, 0.0), 1.0);
    std::vector<float> w{ 5, NAN, 1, 3, INFINITY, 2, 4 };
    QCOMPARE(quantile(w, 1.0), 5.0);
    std::vector<float> m{ 5, 1, 3, 2, 4 };
    QCOMPARE(quantile(m, 0.5), 3.0);
    std::vector<float> high{ 5, 1, 3 };
    QCOMPARE(quantile(high, 7.0), 5.0);     // q clamped to 1
    std::vector<float> none{ NAN };
    QVERIFY(std::isnan(quantile(none, 0.5)));
}

void TestFieldOverlay::layeredOpaqueTopHidesTheBottom()
{
    const Rgba8 c = over({ { { 255, 0, 0, 255 }, 1.0 }, { { 0, 0, 255, 255 }, 1.0 } });
    QVERIFY(same(c, 0, 0, 255, 255));
}

void TestFieldOverlay::layeredOpacityMixesTheLayers()
{
    // Half-opaque blue over opaque red: an even mix, fully opaque.
    const Rgba8 c = over({ { { 255, 0, 0, 255 }, 1.0 }, { { 0, 0, 255, 255 }, 0.5 } });
    QVERIFY(same(c, 128, 0, 128, 255));
    // The same over nothing: the colour of the layer at half alpha.
    const Rgba8 d = over({ { { 0, 0, 255, 255 }, 0.5 } });
    QVERIFY(same(d, 0, 0, 255, 128));
}

void TestFieldOverlay::layeredBlankTopLeavesTheBottom()
{
    const Rgba8 c = over({ { { 255, 0, 0, 255 }, 1.0 }, { { 0, 0, 255, 0 }, 1.0 } });
    QVERIFY(same(c, 255, 0, 0, 255));
}

void TestFieldOverlay::layeredNothingIsTransparent()
{
    QVERIFY(same(over({ { { 9, 9, 9, 0 }, 1.0 } }), 0, 0, 0, 0));
    QVERIFY(same(over({ { { 9, 9, 9, 255 }, 0.0 } }), 0, 0, 0, 0));
}

void TestFieldOverlay::additiveSumsAndClips()
{
    QVERIFY(same(over({ { { 255, 0, 0, 255 }, 1.0 }, { { 0, 255, 0, 255 }, 1.0 } },
                      Layout::Additive),
                 255, 255, 0, 255));
    QVERIFY(same(over({ { { 200, 0, 0, 255 }, 1.0 }, { { 200, 0, 0, 255 }, 1.0 } },
                      Layout::Additive),
                 255, 0, 0, 255));    // 400 clips at 255
    // Opacity is the weight in the sum.
    QVERIFY(same(over({ { { 200, 0, 0, 255 }, 0.5 }, { { 100, 0, 0, 255 }, 1.0 } },
                      Layout::Additive),
                 200, 0, 0, 255));
}

void TestFieldOverlay::additiveBlankLayerAddsNothing()
{
    QVERIFY(same(over({ { { 255, 0, 0, 255 }, 1.0 }, { { 0, 255, 0, 0 }, 1.0 } },
                      Layout::Additive),
                 255, 0, 0, 255));
    // Blank everywhere: transparent, not an opaque black.
    QVERIFY(same(over({ { { 0, 255, 0, 0 }, 1.0 } }, Layout::Additive), 0, 0, 0, 0));
}

void TestFieldOverlay::additiveMatchesTheImageViewerBlend()
{
    // One rule for the image viewer's Additive and this one.
    const std::uint8_t a[4] = { 120, 30, 10, 255 }, b[4] = { 40, 200, 90, 255 };
    visivo::blend::Accum acc;
    visivo::blend::accumulate(acc, a, 0.7);
    visivo::blend::accumulate(acc, b, 1.0);
    const Rgba8 viewer = visivo::blend::finish(acc);
    const Rgba8 ours = over({ { { 120, 30, 10, 255 }, 0.7 }, { { 40, 200, 90, 255 }, 1.0 } },
                            Layout::Additive);
    QVERIFY(same(ours, viewer.r, viewer.g, viewer.b, viewer.a, 0));
}

void TestFieldOverlay::splitTakesLeftFromAAndRightFromB()
{
    const SplitLayout s = splitLayout(10, 4, false);
    QCOMPARE(s.outWidth, 10);
    QCOMPARE(s.indexOffset, 0);
    for (int x = 0; x < 10; ++x) {
        const SplitSource src = splitSource(s, x);
        QCOMPARE(src.field, x < 4 ? 0 : 1);
        QCOMPARE(src.column, x);
    }
    QCOMPARE(splitSource(s, 10).column, -1);
    QCOMPARE(splitSource(s, -1).column, -1);
}

void TestFieldOverlay::splitSeamIsClampedToTheSlice()
{
    QCOMPARE(splitLayout(10, -5, false).seam, 0);
    QCOMPARE(splitLayout(10, 99, false).seam, 10);
    // All of the slice on one side: every column of A.
    const SplitLayout all = splitLayout(10, 99, false);
    for (int x = 0; x < 10; ++x)
        QCOMPARE(splitSource(all, x).field, 0);
}

void TestFieldOverlay::mirroredReflectsAAndKeepsB()
{
    // Axis at column 3 of 10: seven columns to its right.
    const SplitLayout s = splitLayout(10, 3, true);
    QCOMPARE(s.outWidth, 14);
    // Right half: B, from the axis outward, as it is.
    QCOMPARE(splitSource(s, 7).field, 1);
    QCOMPARE(splitSource(s, 7).column, 3);
    QCOMPARE(splitSource(s, 13).column, 9);
    // Left half: A, reflected: the column next to the axis is the axis' own.
    QCOMPARE(splitSource(s, 6).field, 0);
    QCOMPARE(splitSource(s, 6).column, 3);
    QCOMPARE(splitSource(s, 0).column, 9);   // the farthest
    // Mirror symmetry: output columns k and 13 - k read the same slice column.
    for (int k = 0; k < 7; ++k)
        QCOMPARE(splitSource(s, k).column, splitSource(s, 13 - k).column);
}

void TestFieldOverlay::mirroredAxisAtTheEdgeDoublesTheWidth()
{
    // An axisymmetric run: the grid starts on the axis.
    const SplitLayout s = splitLayout(8, 0, true);
    QCOMPARE(s.outWidth, 16);
    QCOMPARE(s.indexOffset, -8);
    // An axis at the far edge leaves nothing to draw.
    QCOMPARE(splitLayout(8, 8, true).outWidth, 0);
}

void TestFieldOverlay::mirroredRightHalfKeepsItsPlace()
{
    // Output column k sits at slice column k + indexOffset: the right half's
    // columns coincide with the slice's own.
    const SplitLayout s = splitLayout(10, 3, true);
    for (int k = s.seam; k < s.outWidth; ++k)
        QCOMPARE(k + s.indexOffset, splitSource(s, k).column);
}

void TestFieldOverlay::composeSplitPaintsEachHalfWithItsOwnColours()
{
    // Two 4 x 1 slices: A all red, B all blue.
    std::vector<std::uint8_t> a, b;
    for (int i = 0; i < 4; ++i) {
        a.insert(a.end(), { 255, 0, 0, 255 });
        b.insert(b.end(), { 0, 0, 255, 255 });
    }
    std::vector<Rgba8> out(4);
    composeSplit(splitLayout(4, 2, false), a.data(), b.data(), 1.0, 1.0, 1, out.data());
    QVERIFY(same(out[0], 255, 0, 0, 255));
    QVERIFY(same(out[1], 255, 0, 0, 255));
    QVERIFY(same(out[2], 0, 0, 255, 255));
    QVERIFY(same(out[3], 0, 0, 255, 255));

    // Mirrored about column 2: A reflected to the left, B to the right.
    std::vector<Rgba8> mirror(4);
    composeSplit(splitLayout(4, 2, true), a.data(), b.data(), 1.0, 0.5, 1, mirror.data());
    QVERIFY(same(mirror[0], 255, 0, 0, 255));
    QVERIFY(same(mirror[1], 255, 0, 0, 255));
    QVERIFY(same(mirror[2], 0, 0, 255, 128));   // B at its own opacity
    QVERIFY(same(mirror[3], 0, 0, 255, 128));
}

void TestFieldOverlay::seamColumnFollowsStrideAndOrigin()
{
    QCOMPARE(seamColumnFromIndex(32.0, 0.0, 1.0), 32);
    QCOMPARE(seamColumnFromIndex(31.5, 0.0, 1.0), 32);   // between 31 and 32
    // A slice read every 4th pixel, starting at 2: column i at 2 + 4 i.
    QCOMPARE(seamColumnFromIndex(18.0, 2.0, 4.0), 4);
    QCOMPARE(seamColumnFromIndex(10.0, 0.0, 0.0), 0);    // no stride: nothing
}

void TestFieldOverlay::opacityNodesScale()
{
    std::vector<std::pair<double, double>> nodes{ { 0.0, 0.0 }, { 1.0, 0.6 }, { 2.0, 1.0 } };
    scaleOpacityNodes(nodes, 0.5);
    QCOMPARE(nodes[0].second, 0.0);
    QVERIFY(std::abs(nodes[1].second - 0.3) < 1e-12);
    QVERIFY(std::abs(nodes[2].second - 0.5) < 1e-12);
    scaleOpacityNodes(nodes, 9.0);          // clamped to 1: nothing grows past 1
    QVERIFY(std::abs(nodes[2].second - 0.5) < 1e-12);
}

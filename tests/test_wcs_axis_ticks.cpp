#include "TestWcsAxisTicks.h"

#include "gui/WcsAxisTicks.h"

#include <QtTest>

#include <cmath>

using visivo::AxisVoxel;

void TestWcsAxisTicks::theXAxisIsReadAlongTheBottomEdge()
{
    for (double t : { 0.0, 0.25, 0.5, 1.0 }) {
        const AxisVoxel v = visivo::xAxisTickVoxel(t, 10.0, 690.0, 5.0);
        QCOMPARE(v.x, 10.0 + t * 680.0);
        QCOMPARE(v.y, 5.0);   // the axis' own height, whatever t
    }
}

void TestWcsAxisTicks::theYAxisIsReadAlongTheLeftEdge()
{
    for (double t : { 0.0, 0.25, 0.5, 1.0 }) {
        const AxisVoxel v = visivo::yAxisTickVoxel(t, 5.0, 695.0, 10.0);
        QCOMPARE(v.x, 10.0);   // the axis' own column, whatever t
        QCOMPARE(v.y, 5.0 + t * 690.0);
    }
}

void TestWcsAxisTicks::aRotatedFrameLabelsTheLeftEdgeNotTheDiagonal()
{
    // HCG 44 in Galactic coordinates, to first order: b falls by ~1.5° across
    // the 700 columns and rises by ~0.45° up the 700 rows (astropy:
    // b(0,0) = 55.58, b(699,0) = 54.10, b(0,699) = 56.02).
    const auto latitude = [](double x, double y) { return 55.58 - 0.00212 * x + 0.00063 * y; };
    const double xMin = 0.0, xMax = 699.0, yMin = 0.0, yMax = 699.0;
    // The top label: read at the top-left corner, as the hover readout there says.
    const AxisVoxel top = visivo::yAxisTickVoxel(1.0, yMin, yMax, xMin);
    QVERIFY(std::abs(latitude(top.x, top.y) - 56.02) < 0.01);
    // Read on the diagonal it was the top-RIGHT corner's latitude, ~1.5° away:
    // the axis ran the wrong way against the readout.
    QVERIFY(std::abs(latitude(xMax, yMax) - latitude(top.x, top.y)) > 1.0);
    // Labels grow upwards, as b does along the left edge.
    double previous = -1e9;
    for (double t : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
        const AxisVoxel v = visivo::yAxisTickVoxel(t, yMin, yMax, xMin);
        const double b = latitude(v.x, v.y);
        QVERIFY(b > previous);
        previous = b;
    }
}

void TestWcsAxisTicks::aSmallFieldKeepsEveryTickLabelDistinct()
{
    // A 2 arcsec field of the Galactic continuum: five ticks 0.5 arcsec apart at
    // l = 230.7512. Two decimals printed "230.75" on every one of them.
    const double span = 2.0 / 3600.0;
    QVERIFY(visivo::degreeTickDecimals(span) >= 5);
    QSet<QString> labels;
    for (int i = 0; i < 5; ++i)
        labels.insert(visivo::formatDegreeTick(230.7512 + i * span / 4.0, span));
    QCOMPARE(labels.size(), 5);
    // A wide field and an unknown span (the hover readout) keep two decimals.
    QCOMPARE(visivo::degreeTickDecimals(2.0), 2);
    QCOMPARE(visivo::degreeTickDecimals(-1.0), 2);
    QCOMPARE(visivo::formatDegreeTick(29.5312, -1.0), QStringLiteral("29.53"));
    // The origin never reads "-0.000".
    QCOMPARE(visivo::formatDegreeTick(-1e-9, 0.01), QStringLiteral("0.0000"));
}

void TestWcsAxisTicks::aSpanAcrossZeroIsTheShortWayRound()
{
    QVERIFY(std::abs(visivo::angularAxisSpan(359.99, 0.01) - 0.02) < 1e-9);
    QCOMPARE(visivo::angularAxisSpan(10.0, 30.0), 20.0);
}

void TestWcsAxisTicks::theYTitleClearsLongLabels()
{
    // Short labels leave the title where it was.
    QCOMPARE(visivo::yAxisTitleX(136.0, 56.0, 8.0, 5, 7.5, 10.0), 56.0);
    // "230.751234" (10 characters) would run under it: it moves left, clear of them.
    const double x = visivo::yAxisTitleX(136.0, 56.0, 8.0, 10, 7.5, 10.0);
    QVERIFY(x < 56.0);
    QVERIFY(x + 9.0 <= 136.0 - 10.0 - 10 * 7.5);
    // ...but never off the window.
    QCOMPARE(visivo::yAxisTitleX(136.0, 56.0, 8.0, 40, 7.5, 10.0), 8.0);
}

void TestWcsAxisTicks::aLinearAxisPrintsZeroNotRoundingNoise()
{
    // x = 0 of a 0..3 AU axis came out of the pixel-to-world map as 1.63e-16.
    QCOMPARE(visivo::formatLinearTick(1.63e-16, 3.0), QStringLiteral("0"));
    QCOMPARE(visivo::formatLinearTick(-1.63e-16, 3.0), QStringLiteral("0"));
    QCOMPARE(visivo::formatLinearTick(1.5, 3.0), QStringLiteral("1.5"));
    // A genuinely small axis keeps its small values.
    QCOMPARE(visivo::formatLinearTick(2e-24, 4e-24), QStringLiteral("2e-24"));
    // Span unknown (the hover readout): printed as is.
    QCOMPARE(visivo::formatLinearTick(1.63e-16, -1.0), QStringLiteral("1.63e-16"));
}

void TestWcsAxisTicks::aLinearWcsIsNotNamedAfterASkyFrame()
{
    QCOMPARE(visivo::linearFrameLabel(QStringLiteral("X"), QStringLiteral("Z")),
             QStringLiteral("linear (X, Z)"));
    QCOMPARE(visivo::linearFrameLabel(QString(), QString()), QStringLiteral("pixel"));
}

void TestWcsAxisTicks::anAxisFollowsTheImageOnScreenAndReadsItsEnds()
{
    // A 400-pixel-wide image centred in an 1100-pixel viewport at 1.4 px per
    // voxel: it spans screen 270..830, not the 136..1066 the axis used to cover.
    const double px = 1.4;
    const visivo::AxisFit fit = visivo::fitAxisToImage(1100.0, 200.0, px, 0.0, 400.0, 136.0, 34.0);
    QVERIFY(fit.valid);
    QVERIFY(std::abs(fit.screenLo - 270.0) < 1e-9);
    QVERIFY(std::abs(fit.screenHi - 830.0) < 1e-9);
    // The axis ends read the image's own ends.
    QVERIFY(std::abs(fit.worldLo - 0.0) < 1e-9);
    QVERIFY(std::abs(fit.worldHi - 400.0) < 1e-9);
    // Zoomed in until the image overflows the viewport: the viewport's room
    // wins, and the ends read what is under them.
    const visivo::AxisFit zoomed = visivo::fitAxisToImage(1100.0, 200.0, 10.0, 0.0, 400.0, 136.0, 34.0);
    QCOMPARE(zoomed.screenLo, 136.0);
    QCOMPARE(zoomed.screenHi, 1066.0);
    QVERIFY(std::abs(zoomed.worldLo - (200.0 + (136.0 - 550.0) / 10.0)) < 1e-9);
    // Too little of the image on screen to label: no axis fit, the caller keeps its default.
    QVERIFY(!visivo::fitAxisToImage(1100.0, 5000.0, 1.0, 0.0, 400.0, 136.0, 34.0).valid);
    QVERIFY(!visivo::fitAxisToImage(1100.0, 200.0, 0.0, 0.0, 400.0, 136.0, 34.0).valid);
    // A panned camera moves the axis with the image; a small pane may label a
    // shorter axis than the default 60 px.
    const visivo::AxisFit panned = visivo::fitAxisToImage(1100.0, 100.0, px, 0.0, 400.0, 136.0, 34.0);
    QVERIFY(std::abs(panned.screenLo - (550.0 - 100.0 * px)) < 1e-9);
    QVERIFY(std::abs(panned.worldLo - 0.0) < 1e-9);
    const visivo::AxisFit narrow = visivo::fitAxisToImage(200.0, 20.0, 1.0, 0.0, 40.0, 80.0, 70.0);
    QVERIFY(!narrow.valid);
    QVERIFY(visivo::fitAxisToImage(200.0, 20.0, 1.0, 0.0, 40.0, 80.0, 70.0, 16.0).valid);
}

namespace {
// What the label at a full-resolution pixel reads for a linear FITS axis.
double headerValue(double crval, double crpix, double cdelt, double fullResPixel)
{
    return crval + (fullResPixel + 1.0 - crpix) * cdelt;
}

// The world extent a VTK image with this grid covers (vtkImageData::GetBounds on
// one axis): origin to origin + (samples - 1) * spacing.
struct GridAxis
{
    int samples;
    double origin;
    double spacing;
    double lo() const { return origin; }
    double hi() const { return origin + (samples - 1) * spacing; }
};

// The label span an axis laid on this grid prints, over the full-resolution
// pixels it shows: the ratio to CDELT x those pixels is 1 when the labels are right.
double labelSpanRatio(const GridAxis &grid, double trueStride)
{
    const double cdelt = -1.027777777717e-06;   // SDP9 continuum
    const double viewportPx = 1100.0;
    const double pxPerUnit = 0.07;   // the whole image fits
    const double focal = 0.5 * (grid.lo() + grid.hi());
    const visivo::AxisFit fit = visivo::fitAxisToImage(viewportPx, focal, pxPerUnit, grid.lo(),
                                                       grid.hi(), 136.0, 34.0);
    const double span = std::fabs(headerValue(136.9, 5185.0, cdelt, fit.worldHi)
                                  - headerValue(136.9, 5185.0, cdelt, fit.worldLo));
    const double fullResPixelsShown = trueStride * (fit.screenHi - fit.screenLo) / pxPerUnit
            / grid.spacing;
    return span / (std::fabs(cdelt) * fullResPixelsShown);
}
}   // namespace

void TestWcsAxisTicks::aDecimatedImageLabelsItsAxisInFullResolutionPixels()
{
    // Level 0, then the three ways an image reaches the screen reduced by 2
    // (issue #539): a tile level, a block-mean preview (origin half a cell in)
    // and a cube slice read 1 in 2 from a region's corner.
    QVERIFY(std::abs(labelSpanRatio({ 10368, 0.0, 1.0 }, 1.0) - 1.0) < 1e-9);
    QVERIFY(std::abs(labelSpanRatio({ 5184, 0.0, 2.0 }, 2.0) - 1.0) < 1e-9);
    QVERIFY(std::abs(labelSpanRatio({ 5184, 0.5, 2.0 }, 2.0) - 1.0) < 1e-9);
    QVERIFY(std::abs(labelSpanRatio({ 2000, 3000.0, 2.0 }, 2.0) - 1.0) < 1e-9);
    // The wrong mapping: the displayed voxel read as the pixel (spacing 1 on a
    // grid that is really 2 apart) halves the span.
    QVERIFY(std::abs(labelSpanRatio({ 5184, 0.0, 1.0 }, 2.0) - 0.5) < 1e-9);
}

void TestWcsAxisTicks::tickLabelsAreThinnedToTheRoomTheyHave()
{
    // Five ticks, four intervals, labels 60 px wide with 10 px between them.
    // Room for all five: every label.
    QCOMPARE(visivo::tickLabelStride(400.0, 60.0, 10.0, 4), 1);
    // About 250 px wide: 62 px per interval, too little next to the right-
    // justified last label (1.5 × 60 + 10 = 100), so every other label.
    QCOMPARE(visivo::tickLabelStride(250.0, 60.0, 10.0, 4), 2);
    // About 175 px wide (issue #544): the ends only.
    QCOMPARE(visivo::tickLabelStride(175.0, 60.0, 10.0, 4), 4);
    // 300 px: 75 px per interval would fit centred labels, but the last pair
    // (centred, then right-justified) would overlap by 15 px: every other label.
    QCOMPARE(visivo::tickLabelStride(300.0, 60.0, 10.0, 4), 2);
    // Wider labels (more digits) need more room for the same axis.
    QCOMPARE(visivo::tickLabelStride(400.0, 95.0, 10.0, 4), 2);
    QCOMPARE(visivo::tickLabelStride(250.0, 95.0, 10.0, 4), 4);
    // The chosen stride always leaves the gap, unless only the ends are left.
    for (double axisPx = 120.0; axisPx < 900.0; axisPx += 7.0) {
        const int k = visivo::tickLabelStride(axisPx, 70.0, 10.0, 4);
        QVERIFY(k == 1 || k == 2 || k == 4);
        if (k < 4)
            QVERIFY(k * axisPx / 4.0 >= 1.5 * 70.0 + 10.0);
    }
    // Unusable input keeps the ends.
    QCOMPARE(visivo::tickLabelStride(0.0, 60.0, 10.0, 4), 4);
    QCOMPARE(visivo::tickLabelStride(400.0, 60.0, 10.0, 1), 1);
}

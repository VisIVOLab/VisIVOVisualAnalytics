#include "TestRadioDisplay.h"

#include "app/BackendWording.h"
#include "app/DatasetBeam.h"
#include "gui/ChannelMapsLayout.h"
#include "gui/PvPathGeometry.h"

#include <QtTest>

using namespace Qt::Literals::StringLiterals;

namespace {
constexpr double kMas = 1.0 / 3600.0 / 1000.0;   // degrees in a milliarcsecond
constexpr double kArcsec = 1.0 / 3600.0;
}

void TestRadioDisplay::aMilliarcsecondBeamIsNotZero()
{
    // The SDP9 ALMA beam, 22.8 x 18.4 mas, read 0.03" x 0.02" and 0.0" x 0.0".
    QCOMPARE(visivo::beam::formatBeamSize(22.8 * kMas, 18.4 * kMas), u"22.8 mas × 18.4 mas"_s);
    QCOMPARE(visivo::beam::formatAngularSize(22.8 * kMas), u"22.8 mas"_s);
    // 5 mas keeps its digits too.
    QCOMPARE(visivo::beam::formatAngularSize(5.0 * kMas), u"5.00 mas"_s);
}

void TestRadioDisplay::aBeamPairSharesOneUnit()
{
    // Both axes in the unit of the larger one: never "1.20″ × 800 mas".
    QCOMPARE(visivo::beam::formatBeamSize(1.2 * kArcsec, 0.8 * kArcsec), u"1.20″ × 0.800″"_s);
    QCOMPARE(visivo::beam::formatBeamSize(15.0 * kArcsec, 15.0 * kArcsec), u"15.0″ × 15.0″"_s);
    QCOMPARE(visivo::beam::formatBeamSize(90.0 * kArcsec, 45.0 * kArcsec), u"1.50′ × 0.750′"_s);
    // Minor axis written first still takes the larger one's unit.
    QCOMPARE(visivo::beam::formatBeamSize(0.5 * kArcsec, 0.9 * kArcsec), u"500 mas × 900 mas"_s);
}

void TestRadioDisplay::sizesPickTheirUnit()
{
    QCOMPARE(visivo::beam::formatAngularSize(0.5 * kArcsec), u"500 mas"_s);
    QCOMPARE(visivo::beam::formatAngularSize(1.0 * kArcsec), u"1.00″"_s);
    QCOMPARE(visivo::beam::formatAngularSize(30.0 * kArcsec), u"30.0″"_s);
    QCOMPARE(visivo::beam::formatAngularSize(120.0 * kArcsec), u"2.00′"_s);
    QCOMPARE(visivo::beam::formatAngularSize(2.5), u"2.50°"_s);
    QCOMPARE(visivo::beam::formatAngularSize(0.0), u"0″"_s);
    QCOMPARE(visivo::beam::formatAngularSize(std::nan("")), u"0″"_s);
}

void TestRadioDisplay::threeSignificantDigitsSurviveRounding()
{
    QCOMPARE(visivo::beam::threeSignificant(9.996), u"10.0"_s);   // not "10.00"
    QCOMPARE(visivo::beam::threeSignificant(0.0228), u"0.0228"_s);
    QCOMPARE(visivo::beam::threeSignificant(123.456), u"123"_s);
    QCOMPARE(visivo::beam::threeSignificant(1.0), u"1.00"_s);
}

void TestRadioDisplay::beamSummaryUsesTheFormatter()
{
    QCOMPARE(visivo::beam::summary(22.8 * kMas, 18.4 * kMas, 85.0, u"header"_s),
             u"22.8 mas × 18.4 mas, PA 85° (header)"_s);
    // The older rendering of an arcsecond beam is unchanged.
    QCOMPARE(visivo::beam::summary(15.0 * kArcsec, 15.0 * kArcsec, 0.0, u"user"_s),
             u"15.0″ × 15.0″, PA 0° (set by user)"_s);
}

void TestRadioDisplay::aLocalBackendIsNotCalledRemote()
{
    QCOMPARE(visivo::fullResolutionTag(true), u"local · full-res"_s);
    QCOMPARE(visivo::fullResolutionTag(false), u"remote · full-res"_s);
    QVERIFY(!visivo::momentScopeLabel(true).contains(u"emote"_s));
    QCOMPARE(visivo::momentScopeLabel(false), u"Remote full dataset"_s);
    // A Recent entry stores "remote" for every file a backend opened by path:
    // it says local while no other backend was recorded.
    QCOMPARE(visivo::recentSourceLabel(u"remote"_s, QString()), u"local"_s);
    QCOMPARE(visivo::recentSourceLabel(QString(), QString()), u"local"_s);
    QCOMPARE(visivo::recentSourceLabel(u"remote"_s, u"pleiadi"_s), u"remote"_s);
    QCOMPARE(visivo::recentSourceLabel(u"skava"_s, QString()), u"skava"_s);
    QCOMPARE(visivo::recentSourceLabel(u"soda"_s, u"node"_s), u"soda"_s);
}

void TestRadioDisplay::columnsAskedForFitTheViewport()
{
    namespace cm = visivo::cmlayout;
    for (int cols : { 1, 3, 8, 12 }) {
        for (int viewport : { 900, 1200, 1900 }) {
            const int imageW = cm::imageWidth(viewport, cols);
            // The whole row: first panel (with its y axis) and the others.
            int row = 2 * cm::kGridMargin + cm::kGridSpacing * (cols - 1);
            for (int c = 0; c < cols; ++c)
                row += cm::panelSize(c, 0, 2, imageW, imageW, 20).width;
            if (imageW > cm::kMinImageWidth)
                QVERIFY2(row <= viewport, qPrintable(u"cols %1 viewport %2 row %3"_s
                                                             .arg(cols).arg(viewport).arg(row)));
        }
    }
    // 8 columns in a 1200 px window used to be 8 x 160 = 1280 px of panels.
    QVERIFY(cm::imageWidth(1200, 8) >= 100);
    // The window is sized for what was asked.
    QCOMPARE(cm::imageWidth(cm::viewportWidthFor(8, 140), 8), 140);
    // A very narrow window stops at the floor and scrolls instead of vanishing.
    QCOMPARE(cm::imageWidth(200, 8), cm::kMinImageWidth);
}

void TestRadioDisplay::aPanelKeepsTheImageAspect()
{
    namespace cm = visivo::cmlayout;
    // A 196 x 280 cube is taller than wide, and stays so.
    QCOMPARE(cm::imageHeight(140, 196, 280), 200);
    QCOMPARE(cm::imageHeight(140, 280, 196), 98);
    QCOMPARE(cm::imageHeight(140, 100, 100), 140);
    QCOMPARE(cm::imageHeight(140, 0, 0), 140);   // unknown size: square
}

void TestRadioDisplay::panelsOfAColumnShareTheirMargins()
{
    namespace cm = visivo::cmlayout;
    const auto a = cm::panelSize(2, 0, 3, 100, 100, 26);
    const auto b = cm::panelSize(2, 1, 3, 100, 100, 26);
    QCOMPARE(a.width, b.width);
    QCOMPARE(a.marginLeft, b.marginLeft);
    // Only the first column carries the y tick labels, and only the last row
    // the x ones; the image area is 100 x 100 everywhere.
    const auto first = cm::panelSize(0, 0, 3, 100, 100, 26);
    QCOMPARE(first.width - first.marginLeft - first.marginRight, 100);
    QVERIFY(first.marginLeft > a.marginLeft);
    const auto last = cm::panelSize(2, 2, 3, 100, 100, 26);
    QCOMPARE(last.height - last.marginBottom - 26, 100);
    QVERIFY(last.marginBottom > a.marginBottom);
}

void TestRadioDisplay::theDrawingHintSaysHowToFinish()
{
    for (std::size_t n : { std::size_t(0), std::size_t(1), std::size_t(4) }) {
        const QString hint = pvpath::drawingHint(n);
        QVERIFY(hint.contains(u"double-click"_s));
        QVERIFY(hint.contains(u"click"_s));
    }
    QVERIFY(pvpath::drawingHint(4).contains(u"4 points"_s));
}

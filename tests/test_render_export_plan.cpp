#include "TestRenderExportPlan.h"

#include "gui/RenderExportPlan.h"

#include <QtTest>

using namespace Qt::Literals::StringLiterals;
using namespace visivo::renderexport;

void TestRenderExportPlan::theCurrentSizeIsTheViews()
{
    Request r;
    QCOMPARE(outputSize(QSize(1200, 800), r), QSize(1200, 800));
}

void TestRenderExportPlan::twiceAndFourfoldScaleBothSides()
{
    Request r;
    r.size = SizeChoice::Twice;
    QCOMPARE(outputSize(QSize(1200, 800), r), QSize(2400, 1600));
    r.size = SizeChoice::Fourfold;
    QCOMPARE(outputSize(QSize(1200, 800), r), QSize(4800, 3200));
}

void TestRenderExportPlan::aCustomWidthKeepsTheShapeOfTheView()
{
    Request r;
    r.size = SizeChoice::Custom;
    r.customWidth = 3000;
    QCOMPARE(outputSize(QSize(1200, 800), r), QSize(3000, 2000));
    // The height rounds, and is never zero.
    r.customWidth = 1001;
    QCOMPARE(outputSize(QSize(1200, 800), r), QSize(1001, 667));
    r.customWidth = 16;
    QCOMPARE(outputSize(QSize(4000, 10), r).height(), 1);
    r.customWidth = 0;
    QVERIFY(outputSize(QSize(1200, 800), r).isEmpty());
}

void TestRenderExportPlan::anEmptyViewHasNoPicture()
{
    QVERIFY(outputSize(QSize(0, 0), Request{}).isEmpty());
    QVERIFY(outputSize(QSize(800, 0), Request{}).isEmpty());
    QCOMPARE(tileScale(QSize(0, 0), QSize(100, 100)), 1);
    QCOMPARE(heightForWidth(QSize(0, 0), 100), 0);
}

void TestRenderExportPlan::tooLargeASizeIsRefusedWithAReason()
{
    QVERIFY(sizeProblem(QSize(4000, 3000)).isEmpty());
    QVERIFY(sizeProblem(QSize(kMaxSide, 100)).isEmpty());
    QVERIFY(sizeProblem(QSize(kMaxSide + 1, 100)).contains(u"side"));
    // Both sides within the limit but too many pixels together.
    QVERIFY(sizeProblem(QSize(12000, 9000)).contains(u"million"));
    QVERIFY(!sizeProblem(QSize()).isEmpty());
}

void TestRenderExportPlan::theTileScaleCoversTheSize()
{
    QCOMPARE(tileScale(QSize(1000, 600), QSize(1000, 600)), 1);
    QCOMPARE(tileScale(QSize(1000, 600), QSize(500, 300)), 1);   // smaller: one tile, scaled down
    QCOMPARE(tileScale(QSize(1000, 600), QSize(2000, 1200)), 2);
    QCOMPARE(tileScale(QSize(1000, 600), QSize(2001, 1201)), 3);
    QCOMPARE(tileScale(QSize(1000, 600), QSize(4000, 2400)), 4);
}

void TestRenderExportPlan::aNameWithoutOrWithAnotherSuffixGetsTheFormats()
{
    QCOMPARE(formatOfName(u"a/b/fig.PDF"_s, Format::Png), Format::Pdf);
    QCOMPARE(formatOfName(u"fig.svg"_s, Format::Png), Format::Svg);
    QCOMPARE(formatOfName(u"fig.png"_s, Format::Pdf), Format::Png);
    QCOMPARE(formatOfName(u"fig"_s, Format::Svg), Format::Svg);
    QCOMPARE(formatOfName(u"fig.tiff"_s, Format::Png), Format::Png);
    QCOMPARE(withSuffix(u"fig"_s, Format::Png), u"fig.png"_s);
    QCOMPARE(withSuffix(u"fig.PNG"_s, Format::Png), u"fig.PNG"_s);
    QCOMPARE(withSuffix(u"fig.tiff"_s, Format::Svg), u"fig.tiff.svg"_s);
    QVERIFY(filterFor(Format::Pdf).contains(u"*.pdf"));
}

void TestRenderExportPlan::theSuggestedNameIsSafeForAFileSystem()
{
    QCOMPARE(suggestedName(u"NGC 3137: cube/1"_s, Format::Pdf), u"NGC_3137__cube_1_view.pdf"_s);
    QCOMPARE(suggestedName(QString(), Format::Png), u"view3d_view.png"_s);
}

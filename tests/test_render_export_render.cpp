// Save View As… (issue #528): the render half, with a real GL context.
//
// A small scene (a sphere on a coloured background) is rendered offscreen, read
// back by renderScene() and written by writePicture(). What is checked is what
// a user relies on: a transparent PNG is transparent in the corners and opaque
// where the scene is, a translucent surface keeps its colour and its alpha, a
// larger size is rendered at that size, and a PDF or an SVG holds the render
// as an image with the title and the axis names as text.
//
// Skipped, with a message, when no GL context can be made (a CI box with no
// display).

#include "vtk/RenderExport.h"

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCubeAxesActor.h>
#include <vtkNew.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSphereSource.h>

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::Literals::StringLiterals;
using namespace visivo::renderexport;

namespace {

struct Scene
{
    vtkNew<vtkRenderWindow> window;
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkActor> sphere;
    vtkNew<vtkCubeAxesActor> axes;
    bool ok = false;

    explicit Scene(double opacity)
    {
        window->SetOffScreenRendering(1);
        window->SetMultiSamples(0);
        window->SetAlphaBitPlanes(1);
        window->SetSize(200, 150);
        vtkNew<vtkSphereSource> source;
        source->SetRadius(0.5);
        source->SetThetaResolution(48);
        source->SetPhiResolution(48);
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(source->GetOutputPort());
        sphere->SetMapper(mapper);
        // Flat colour: no lighting, so the colour read back is the colour set.
        sphere->GetProperty()->SetColor(1.0, 0.0, 0.0);
        sphere->GetProperty()->LightingOff();
        sphere->GetProperty()->SetOpacity(opacity);
        sphere->GetProperty()->BackfaceCullingOn();   // one layer, so alpha = opacity
        renderer->AddActor(sphere);
        renderer->SetBackground(0.1, 0.2, 0.4);
        window->AddRenderer(renderer);
        renderer->ResetCamera();
        axes->SetCamera(renderer->GetActiveCamera());
        axes->SetBounds(-0.5, 0.5, -0.5, 0.5, -0.5, 0.5);
        axes->SetXTitle("X (test)");
        axes->SetYTitle("Y (test)");
        axes->SetZTitle("Z (test)");
        axes->SetXAxisRange(10.0, 20.0);
        renderer->AddViewProp(axes);
        axes->VisibilityOff();   // drawn by the export, not in these renders
        ok = window->SupportsOpenGL() != 0;
    }
};

QRgb unpremultiplied(const QImage &image, int x, int y)
{
    return image.pixel(x, y);
}

} // namespace

class TestRenderExportRender : public QObject
{
    Q_OBJECT

private slots:
    void aTransparentRenderIsTransparentAroundTheScene();
    void aTranslucentSurfaceKeepsItsColourAndItsAlpha();
    void anOpaqueRenderKeepsTheBackground();
    void aLargerSizeIsRenderedAtThatSize();
    void theWindowIsLeftAsItWas();
    void thePngKeepsItsAlphaOnDisk();
    void aPdfAndAnSvgHoldTheRenderAndVectorText();
};

void TestRenderExportRender::aTransparentRenderIsTransparentAroundTheScene()
{
    Scene s(1.0);
    if (!s.ok)
        QSKIP("No GL context for an offscreen render window here.");
    QString error;
    const QImage image = renderScene(s.window, s.renderer, QSize(200, 150), true, {}, &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    QCOMPARE(image.size(), QSize(200, 150));
    QVERIFY(image.hasAlphaChannel());
    for (const QPoint &c : { QPoint(0, 0), QPoint(199, 0), QPoint(0, 149), QPoint(199, 149) })
        QCOMPARE(qAlpha(unpremultiplied(image, c.x(), c.y())), 0);
    const QRgb centre = unpremultiplied(image, 100, 75);
    QCOMPARE(qAlpha(centre), 255);
    QVERIFY2(qRed(centre) > 240 && qGreen(centre) < 16 && qBlue(centre) < 16,
             qPrintable(QString::number(centre, 16)));
}

void TestRenderExportRender::aTranslucentSurfaceKeepsItsColourAndItsAlpha()
{
    Scene s(0.5);
    if (!s.ok)
        QSKIP("No GL context for an offscreen render window here.");
    QString error;
    const QImage image = renderScene(s.window, s.renderer, QSize(200, 150), true, {}, &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    const QRgb centre = unpremultiplied(image, 100, 75);
    // Half-opaque red over nothing: red at half alpha, not a dark red at full alpha.
    QVERIFY2(qAlpha(centre) > 110 && qAlpha(centre) < 146, qPrintable(QString::number(centre, 16)));
    QVERIFY2(qRed(centre) > 235 && qGreen(centre) < 20 && qBlue(centre) < 20,
             qPrintable(QString::number(centre, 16)));
    QCOMPARE(qAlpha(unpremultiplied(image, 0, 0)), 0);
}

void TestRenderExportRender::anOpaqueRenderKeepsTheBackground()
{
    Scene s(1.0);
    if (!s.ok)
        QSKIP("No GL context for an offscreen render window here.");
    QString error;
    const QImage image = renderScene(s.window, s.renderer, QSize(200, 150), false, {}, &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    QVERIFY(!image.hasAlphaChannel());
    const QRgb corner = image.pixel(0, 0);
    QVERIFY2(qAbs(qRed(corner) - 26) < 4 && qAbs(qGreen(corner) - 51) < 4
                     && qAbs(qBlue(corner) - 102) < 4,
             qPrintable(QString::number(corner, 16)));
}

void TestRenderExportRender::aLargerSizeIsRenderedAtThatSize()
{
    Scene s(1.0);
    if (!s.ok)
        QSKIP("No GL context for an offscreen render window here.");
    QString error;
    // A whole multiple (two tiles per side), and one that is not (scaled down from three).
    const QImage twice = renderScene(s.window, s.renderer, QSize(400, 300), true, {}, &error);
    QVERIFY2(!twice.isNull(), qPrintable(error));
    QCOMPARE(twice.size(), QSize(400, 300));
    QCOMPARE(qAlpha(twice.pixel(0, 0)), 0);
    QCOMPARE(qAlpha(twice.pixel(399, 299)), 0);
    QCOMPARE(qAlpha(twice.pixel(200, 150)), 255);
    const QImage odd = renderScene(s.window, s.renderer, QSize(500, 375), true, {}, &error);
    QVERIFY2(!odd.isNull(), qPrintable(error));
    QCOMPARE(odd.size(), QSize(500, 375));
    QCOMPARE(qAlpha(odd.pixel(250, 187)), 255);
    QCOMPARE(qAlpha(odd.pixel(0, 374)), 0);
    // The sphere keeps its share of the picture at any size.
    int filled = 0;
    for (int y = 0; y < twice.height(); ++y)
        for (int x = 0; x < twice.width(); ++x)
            filled += qAlpha(twice.pixel(x, y)) > 128;
    const double share = double(filled) / (twice.width() * twice.height());
    int filledOdd = 0;
    for (int y = 0; y < odd.height(); ++y)
        for (int x = 0; x < odd.width(); ++x)
            filledOdd += qAlpha(odd.pixel(x, y)) > 128;
    QVERIFY(share > 0.05);
    QVERIFY(qAbs(share - double(filledOdd) / (odd.width() * odd.height())) < 0.02);

    QVERIFY(renderScene(s.window, s.renderer, QSize(20000, 100), false, {}, &error).isNull());
    QVERIFY(!error.isEmpty());
}

void TestRenderExportRender::theWindowIsLeftAsItWas()
{
    Scene s(1.0);
    if (!s.ok)
        QSKIP("No GL context for an offscreen render window here.");
    s.renderer->SetBackgroundAlpha(0.7);
    bool hidden = false;
    int calls = 0;
    QString error;
    const QImage image = renderScene(
            s.window, s.renderer, QSize(200, 150), true,
            [&](bool hide) {
                hidden = hide;
                ++calls;
            },
            &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    QCOMPARE(calls, 2);
    QVERIFY(!hidden);
    double bg[3];
    s.renderer->GetBackground(bg);
    QCOMPARE(bg[0], 0.1);
    QCOMPARE(bg[2], 0.4);
    QCOMPARE(s.renderer->GetBackgroundAlpha(), 0.7);
    const int *size = s.window->GetSize();
    QCOMPARE(size[0], 200);
    QCOMPARE(size[1], 150);
}

void TestRenderExportRender::thePngKeepsItsAlphaOnDisk()
{
    Scene s(1.0);
    if (!s.ok)
        QSKIP("No GL context for an offscreen render window here.");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString error;
    const QImage image = renderScene(s.window, s.renderer, QSize(200, 150), true, {}, &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    Annotations notes;
    notes.logicalSize = QSize(200, 150);
    notes.deviceSize = QSize(200, 150);
    const QString path = dir.filePath(u"view.png"_s);
    QVERIFY2(writePicture(path, Format::Png, image, notes, &error), qPrintable(error));
    const QImage back(path);
    QVERIFY(back.hasAlphaChannel());
    QCOMPARE(back.size(), QSize(200, 150));
    QCOMPARE(qAlpha(back.pixel(0, 0)), 0);
    QCOMPARE(qAlpha(back.pixel(199, 149)), 0);
    QCOMPARE(qAlpha(back.pixel(100, 75)), 255);
}

void TestRenderExportRender::aPdfAndAnSvgHoldTheRenderAndVectorText()
{
    Scene s(1.0);
    if (!s.ok)
        QSKIP("No GL context for an offscreen render window here.");
    s.axes->VisibilityOn();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString error;
    const QImage image = renderScene(s.window, s.renderer, QSize(400, 300), false,
                                     sceneHider({ s.axes.Get() }), &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    QVERIFY(s.axes->GetVisibility());   // put back

    Annotations notes;
    notes.title = u"Test cube"_s;
    notes.box = axesBoxOf(s.axes);
    QVERIFY(notes.box.valid);
    QCOMPARE(notes.box.lo[0], 10.0);
    QCOMPARE(notes.box.hi[0], 20.0);
    QCOMPARE(notes.box.lo[1], -0.5);   // no range set: the bounds
    notes.renderer = s.renderer;
    notes.logicalSize = QSize(200, 150);
    notes.deviceSize = QSize(200, 150);
    notes.colourbar = [](QPainter &painter, const QSize &) {
        painter.fillRect(QRect(170, 20, 10, 100), Qt::green);
    };

    // For a look at the result: VISIVO_KEEP_EXPORT=<dir> keeps a PNG of the annotated picture.
    if (const QString keep = qEnvironmentVariable("VISIVO_KEEP_EXPORT"); !keep.isEmpty())
        QVERIFY2(writePicture(keep + u"/annotated.png"_s, Format::Png, image, notes, &error),
                 qPrintable(error));

    const QString svgPath = dir.filePath(u"view.svg"_s);
    QVERIFY2(writePicture(svgPath, Format::Svg, image, notes, &error), qPrintable(error));
    QFile svg(svgPath);
    QVERIFY(svg.open(QIODevice::ReadOnly));
    const QByteArray text = svg.readAll();
    QVERIFY(text.contains("<svg"));
    QVERIFY(text.contains("<image"));       // the render
    QVERIFY(text.contains("Test cube"));    // the title, as text
    QVERIFY(text.contains("X (test)"));     // an axis name, as text

    const QString pdfPath = dir.filePath(u"view.pdf"_s);
    QVERIFY2(writePicture(pdfPath, Format::Pdf, image, notes, &error), qPrintable(error));
    QFile pdf(pdfPath);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    QVERIFY(pdf.readAll().startsWith("%PDF"));
    QVERIFY(pdf.size() > 1000);
}

QTEST_MAIN(TestRenderExportRender)
#include "test_render_export_render.moc"

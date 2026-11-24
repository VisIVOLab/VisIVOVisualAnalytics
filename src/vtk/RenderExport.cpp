#include "RenderExport.h"

#include <vtkCubeAxesActor.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkType.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkProp.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>

#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QMarginsF>
#include <QPageSize>
#include <QPdfWriter>
#include <QScopeGuard>
#include <QSvgGenerator>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using namespace Qt::StringLiterals;

namespace visivo::renderexport {

AxesBox axesBoxOf(vtkCubeAxesActor *actor)
{
    AxesBox box;
    if (!actor || !actor->GetVisibility())
        return box;
    const double *b = actor->GetBounds();
    if (!b || !(b[1] > b[0] || b[3] > b[2] || b[5] > b[4]))
        return box;
    for (int i = 0; i < 6; ++i)
        box.bounds[i] = b[i];
    const char *titles[3] = { actor->GetXTitle(), actor->GetYTitle(), actor->GetZTitle() };
    const double *ranges[3] = { actor->GetXAxisRange(), actor->GetYAxisRange(),
                                actor->GetZAxisRange() };
    for (int a = 0; a < 3; ++a) {
        box.title[a] = QString::fromUtf8(titles[a] ? titles[a] : "");
        // The actor's default range is "none set": its labels then read the
        // bounds, and so do ours.
        const bool set = ranges[a][0] < VTK_DOUBLE_MAX && ranges[a][1] < VTK_DOUBLE_MAX
                && std::isfinite(ranges[a][0]) && std::isfinite(ranges[a][1]);
        box.lo[a] = set ? ranges[a][0] : b[2 * a];
        box.hi[a] = set ? ranges[a][1] : b[2 * a + 1];
    }
    box.valid = true;
    return box;
}

std::function<void(bool)> sceneHider(std::vector<vtkProp *> props, vtkOrientationMarkerWidget *marker)
{
    struct Saved
    {
        std::vector<int> visible;
        bool marker = false;
    };
    auto saved = std::make_shared<Saved>();
    return [props = std::move(props), marker, saved](bool hide) {
        if (hide) {
            saved->visible.clear();
            for (vtkProp *prop : props) {
                saved->visible.push_back(prop ? prop->GetVisibility() : 0);
                if (prop)
                    prop->VisibilityOff();
            }
            saved->marker = marker && marker->GetEnabled();
            if (saved->marker)
                marker->EnabledOff();
        } else {
            for (size_t i = 0; i < props.size(); ++i)
                if (props[i])
                    props[i]->SetVisibility(saved->visible.size() > i ? saved->visible[i] : 1);
            if (saved->marker)
                marker->EnabledOn();
        }
    };
}

QImage renderScene(vtkRenderWindow *window, vtkRenderer *renderer, const QSize &out,
                   bool transparent, const std::function<void(bool)> &hideAnnotations,
                   QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return QImage();
    };
    if (!window || !renderer)
        return fail(u"There is no 3-D view to save."_s);
    const int *winSize = window->GetSize();
    const QSize view(winSize[0], winSize[1]);
    if (view.width() <= 0 || view.height() <= 0)
        return fail(u"The 3-D view has no size yet."_s);
    if (const QString problem = sizeProblem(out); !problem.isEmpty())
        return fail(problem);
    const int scale = tileScale(view, out);
    // The tiles add up to more than `out` when it is not a whole multiple of
    // the view: it is the read-back that has to fit in memory.
    if (const QString problem = sizeProblem(view * scale); !problem.isEmpty())
        return fail(problem);

    // What the export changes in the scene is put back at the end whatever
    // happens: the window goes on showing what the user had.
    double background[3];
    renderer->GetBackground(background);
    const double backgroundAlpha = renderer->GetBackgroundAlpha();
    const bool gradient = renderer->GetGradientBackground();
    const bool textured = renderer->GetTexturedBackground();
    const auto restore = qScopeGuard([&] {
        renderer->SetBackground(background);
        renderer->SetBackgroundAlpha(backgroundAlpha);
        renderer->SetGradientBackground(gradient);
        renderer->SetTexturedBackground(textured);
        if (hideAnnotations)
            hideAnnotations(false);
        window->Render();
    });
    if (hideAnnotations)
        hideAnnotations(true);
    if (transparent) {
        // Black, so that what the GPU leaves where the render is not opaque is
        // premultiplied by its alpha, which is how the result is read.
        renderer->SetBackground(0.0, 0.0, 0.0);
        renderer->SetBackgroundAlpha(0.0);
        renderer->GradientBackgroundOff();
        renderer->TexturedBackgroundOff();
    }

    vtkNew<vtkWindowToImageFilter> grab;
    grab->SetInput(window);
    grab->SetScale(scale);
    grab->SetInputBufferTypeToRGBA();
    grab->ReadFrontBufferOff();
    grab->ShouldRerenderOn();
    grab->Update();

    vtkImageData *image = grab->GetOutput();
    int dims[3];
    image->GetDimensions(dims);
    auto *pixels = vtkUnsignedCharArray::SafeDownCast(image->GetPointData()->GetScalars());
    if (!pixels || dims[0] <= 0 || dims[1] <= 0 || pixels->GetNumberOfComponents() != 4)
        return fail(u"The 3-D view could not be read back."_s);

    // VTK's rows run from the bottom up. A transparent render is read as
    // premultiplied (see above); an opaque one has the final colour in RGB and
    // an alpha that means nothing (VTK's background alpha is 0 by default), so
    // it is read as colour only.
    QImage grabbed(dims[0], dims[1],
                   transparent ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBX8888);
    const size_t row = size_t(dims[0]) * 4;
    if (grabbed.isNull())
        return fail(u"There is not enough memory for a picture of this size."_s);
    for (int y = 0; y < dims[1]; ++y)
        std::memcpy(grabbed.scanLine(dims[1] - 1 - y), pixels->GetPointer(0) + size_t(y) * row, row);

    const QImage scaled = (grabbed.size() == out)
            ? grabbed
            : grabbed.scaled(out, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (transparent)
        return scaled.convertToFormat(QImage::Format_ARGB32);
    return scaled.convertToFormat(QImage::Format_RGB32);
}

namespace {

/// A label with a dark halo, so it reads on the render's dark background and
/// on the white page of a PDF alike (the look of the colour bar).
void haloText(QPainter &painter, const QPointF &at, const QString &text)
{
    painter.setPen(QColor(0, 0, 0, 190));
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx != 0 || dy != 0)
                painter.drawText(at + QPointF(dx, dy), text);
    painter.setPen(QColor(235, 235, 235));
    painter.drawText(at, text);
}

void haloLine(QPainter &painter, const QPointF &a, const QPointF &b)
{
    painter.setPen(QPen(QColor(0, 0, 0, 190), 3.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(a, b);
    painter.setPen(QPen(QColor(235, 235, 235, 220), 1.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(a, b);
}

/// The corner `index` of the bounds (bit 0: x, bit 1: y, bit 2: z; set is high).
std::array<double, 3> corner(const std::array<double, 6> &b, int index)
{
    return { b[(index & 1) ? 1 : 0], b[(index & 2) ? 3 : 2], b[(index & 4) ? 5 : 4] };
}

void paintBox(QPainter &painter, const Annotations &a)
{
    if (!a.box.valid || !a.renderer || a.deviceSize.width() <= 0 || a.deviceSize.height() <= 0)
        return;
    const double sx = double(a.logicalSize.width()) / a.deviceSize.width();
    const double sy = double(a.logicalSize.height()) / a.deviceSize.height();
    std::array<QPointF, 8> p;
    for (int i = 0; i < 8; ++i) {
        const auto c = corner(a.box.bounds, i);
        a.renderer->SetWorldPoint(c[0], c[1], c[2], 1.0);
        a.renderer->WorldToDisplay();
        double d[3];
        a.renderer->GetDisplayPoint(d);
        // Display rows run from the bottom up.
        p[i] = QPointF(d[0] * sx, (a.deviceSize.height() - d[1]) * sy);
    }
    QPointF centre;
    for (const QPointF &q : p)
        centre += q / 8.0;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < 8; ++i)
        for (int bit = 0; bit < 3; ++bit)
            if (!(i & (1 << bit)))
                haloLine(painter, p[i], p[i | (1 << bit)]);

    // Each axis is named on the one of its four edges that lies farthest from
    // the centre of the box on screen, with the range at its two ends.
    QFont font(u"Helvetica"_s);
    font.setPixelSize(12);
    painter.setFont(font);
    const QFontMetricsF fm(font);
    for (int axis = 0; axis < 3; ++axis) {
        const int bit = 1 << axis;
        int best = -1;
        double bestDistance = -1.0;
        for (int i = 0; i < 8; ++i) {
            if (i & bit)
                continue;
            const QPointF mid = (p[i] + p[i | bit]) / 2.0;
            const double distance = std::hypot(mid.x() - centre.x(), mid.y() - centre.y());
            if (distance > bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        if (best < 0)
            continue;
        const QPointF lo = p[best], hi = p[best | bit];
        const QPointF mid = (lo + hi) / 2.0;
        QPointF outward = mid - centre;
        const double length = std::hypot(outward.x(), outward.y());
        outward = length > 1e-6 ? outward / length : QPointF(0, 1);
        const auto place = [&](const QPointF &anchor, const QString &text) {
            const QPointF at = anchor + outward * 12.0;
            haloText(painter, at + QPointF(-fm.horizontalAdvance(text) / 2.0, fm.ascent() / 2.0 - 1.0),
                     text);
        };
        place(mid + outward * 10.0,
              a.box.title[axis].isEmpty() ? u"Axis %1"_s.arg(axis + 1) : a.box.title[axis]);
        place(lo, QString::number(a.box.lo[axis], 'g', 5));
        place(hi, QString::number(a.box.hi[axis], 'g', 5));
    }
    painter.restore();
}

} // namespace

void paintAnnotations(QPainter &painter, const Annotations &a)
{
    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing);
    paintBox(painter, a);
    if (a.colourbar)
        a.colourbar(painter, a.logicalSize);
    if (!a.title.isEmpty()) {
        QFont font(u"Helvetica"_s);
        font.setPixelSize(16);
        font.setBold(true);
        painter.setFont(font);
        haloText(painter, QPointF(12.0, 12.0 + QFontMetricsF(font).ascent()), a.title);
    }
    painter.restore();
}

bool writePicture(const QString &path, Format format, const QImage &scene,
                  const Annotations &annotations, QString *error)
{
    const auto fail = [error, &path](const QString &why = {}) {
        if (error)
            *error = why.isEmpty() ? u"%1 could not be written."_s.arg(path) : why;
        return false;
    };
    if (scene.isNull())
        return fail(u"There is no picture to save."_s);
    Annotations placed = annotations;
    if (placed.logicalSize.isEmpty())
        placed.logicalSize = scene.size();
    const QSize logical = placed.logicalSize;

    if (format == Format::Png) {
        QImage picture = scene.convertToFormat(scene.hasAlphaChannel()
                                                       ? QImage::Format_ARGB32_Premultiplied
                                                       : QImage::Format_RGB32);
        {
            QPainter painter(&picture);
            painter.scale(double(picture.width()) / logical.width(),
                          double(picture.height()) / logical.height());
            paintAnnotations(painter, placed);
        }
        return picture.save(path, "PNG") ? true : fail();
    }

    const QRectF page(0.0, 0.0, logical.width(), logical.height());
    if (format == Format::Pdf) {
        QPdfWriter writer(path);
        writer.setResolution(72);   // one unit is one point, one pixel of the view
        writer.setPageSize(QPageSize(page.size(), QPageSize::Point, QString(), QPageSize::ExactMatch));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(placed.title);
        QPainter painter(&writer);
        if (!painter.isActive())
            return fail();
        painter.drawImage(page, scene);
        paintAnnotations(painter, placed);
    } else {
        QSvgGenerator svg;
        svg.setFileName(path);
        svg.setSize(logical);
        svg.setViewBox(page);
        svg.setTitle(placed.title);
        QPainter painter(&svg);
        if (!painter.isActive())
            return fail();
        painter.drawImage(page, scene);
        paintAnnotations(painter, placed);
    }
    return QFile(path).size() > 0 ? true : fail();
}

} // namespace visivo::renderexport

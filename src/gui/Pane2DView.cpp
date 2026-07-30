#include "Pane2DView.h"

#include "CubeViewportGeometry.h"
#include "CubeWcsUtil.h"

#include <QDebug>
#include <QVTKInteractor.h>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkAxisActor2D.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkImageMapToColors.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkProperty2D.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkTextRenderer.h>
#include <vtkTrivialProducer.h>

#include <algorithm>
#include <cmath>

namespace {

// Same as vtkWindowCube's WCS overlay: five evenly-spaced tick labels per axis.
constexpr int kOverlayTickCount = 5;

// A 1x1 placeholder so the display pipeline always has a valid input, exactly like
// createPlaceholderImageData() but without depending on it — keeps the class
// self-contained (no vtkWindowCube coupling).
vtkSmartPointer<vtkImageData> makePlaceholderImage()
{
    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetDimensions(1, 1, 1);
    img->AllocateScalars(VTK_DOUBLE, 1);
    img->SetScalarComponentFromDouble(0, 0, 0, 0, 0.);
    return img;
}

// Colorbar-column measurement, replicated from vtkWindowCube::gutter2dFor(): measure
// the LUT range labels with VTK's own text engine at the exact property the colorbar
// uses, then hand CubeGeom::imageGutter() the measured right-gutter width (device px).
CubeGeom::ImageGutter computeGutter(vtkGenericOpenGLRenderWindow *win, vtkLookupTable *lut,
                                    bool showWcsAxes, double dpr)
{
    double range[2] = { 0., 0. };
    if (lut)
        lut->GetTableRange(range);

    // Never measure below the DPR-scaled DPI, or the label column comes out
    // half-width on a retina panel (§3.0b).
    const int dpi = std::max(win ? win->GetDPI() : 72, static_cast<int>(72. * dpr));
    vtkNew<vtkTextProperty> tp;
    tp->SetFontSize(CubeGeom::kColorbarLabelFontSize);
    tp->BoldOff();
    tp->ShadowOff();
    double labelW = 0.;
    if (auto *tr = vtkTextRenderer::GetInstance()) {
        for (double v : { range[0], range[1] }) {
            int bbox[4] = { 0, 0, 0, 0 };
            const std::string s = QString::asprintf("%.3g", v).toStdString();
            if (tr->GetBoundingBox(tp, s, bbox, dpi))
                labelW = std::max<double>(labelW, bbox[1] - bbox[0] + 1);
        }
    }
    if (labelW <= 0.)               // headless / no text engine
        labelW = 7. * 7. * dpr;     // 7 chars of a %.3g exponent, device px

    const double rightGutter = CubeGeom::kColorbarGap * dpr        // data area → bar
            + CubeGeom::kColorbarBarWidth * dpr                    // the swatch
            + CubeGeom::kColorbarTextPad * dpr                     // bar → label
            + labelW                                               // already device px
            + CubeGeom::kColorbarMargin * dpr;                     // label → pane edge
    return CubeGeom::imageGutter(showWcsAxes, rightGutter, dpr);
}

}  // namespace

Pane2DView::Pane2DView(QObject *parent)
    : QObject(parent)
{
    // --- widget + render window -------------------------------------------------
    m_window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    m_widget = new QVTKOpenGLNativeWidget();
    m_widget->setRenderWindow(m_window);
    // THE fix for the macOS trackpad phantom-click: QVTKOpenGLNativeWidget synthesizes
    // trackpad touch events into mouse button PRESSES, so merely moving over the pane
    // fired LeftButtonPress → window/level with no real click. The 2D Slice already does
    // this (vtkWindowCube_Setup.cpp) — match it exactly (§5.1).
    m_widget->setEnableTouchEventProcessing(false);
    // §5.1: activate only on click; no hover focus/interaction. The container swallows
    // the activating press and flips setInteractive() — panes start non-interactive.
    m_widget->setFocusPolicy(Qt::ClickFocus);

    // --- renderer + parallel-projection camera (mirror setupMomentRenderer) -----
    m_renderer = vtkSmartPointer<vtkRenderer>::New();
    m_renderer->SetBackground(0.0, 0.0, 0.0157);   // #000004 — scientific colormaps need black
    m_renderer->GetActiveCamera()->ParallelProjectionOn();
    m_window->AddRenderer(m_renderer);

    // --- interactor: EXACTLY the 2D-Slice pattern ------------------------------
    // Use the interactor the QVTKOpenGLNativeWidget created for us in setRenderWindow()
    // (as the slice does with sliceWin->GetInteractor()). Do NOT create a SECOND
    // QVTKInteractor and SetInteractor() it — the legacy setupMomentRenderer did that,
    // and a manually-installed interactor layered over the widget's own is what made this
    // pane window/level on hover while the slice (same style) did not. With the widget's
    // own interactor + plain vtkInteractorStyleImage, LEFT-drag windows the contrast (its
    // OWN m_lut), hover does nothing — idle panes are gated by the Qt-level swallow in
    // vtkWindowCube::eventFilter (§5.1). Do NOT Disable() it (QVTK renders through it).
    vtkNew<vtkInteractorStyleImage> style;
    if (auto *iren = m_window->GetInteractor())
        iren->SetInteractorStyle(style);

    // --- display pipeline: source → colors(LUT) → slice mapper → slice actor ----
    m_source = vtkSmartPointer<vtkTrivialProducer>::New();
    m_source->SetOutput(makePlaceholderImage());

    m_lut = vtkSmartPointer<vtkLookupTable>::New();
    m_lut->SetTableRange(0., 0.);
    m_lut->SetNanColor(0., 0., 0., 0.);   // masked / NaN pixels read as ABSENT (transparent)
    m_lut->Build();

    m_colors = vtkSmartPointer<vtkImageMapToColors>::New();
    m_colors->SetInputConnection(m_source->GetOutputPort());
    m_colors->SetLookupTable(m_lut);

    m_mapper = vtkSmartPointer<vtkImageSliceMapper>::New();
    m_mapper->SetInputConnection(m_colors->GetOutputPort());
    m_mapper->BorderOn();

    m_actor = vtkSmartPointer<vtkImageSlice>::New();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetInterpolationTypeToNearest();
    m_renderer->AddViewProp(m_actor);

    // --- colorbar (parked in the right gutter, laid out by fit()) ---------------
    m_colorbar = vtkSmartPointer<vtkScalarBarActor>::New();
    m_colorbar->SetLookupTable(m_lut);
    CubeWcs::applyOverlayFont(m_colorbar->GetLabelTextProperty());
    m_renderer->AddViewProp(m_colorbar);
    CubeGeom::layoutColorbarInGutter(m_colorbar, m_renderer,
                                     computeGutter(m_window, m_lut, m_showWcsAxes, dpr()),
                                     CubeGeom::ImageRect{}, dpr());

    // --- WCS overlay actors (4 props + tick vectors) ----------------------------
    m_overlayXAxis = vtkSmartPointer<vtkAxisActor2D>::New();
    m_overlayYAxis = vtkSmartPointer<vtkAxisActor2D>::New();
    m_overlayXTitle = vtkSmartPointer<vtkTextActor>::New();
    m_overlayYTitle = vtkSmartPointer<vtkTextActor>::New();

    m_overlayXAxis->GetTitleTextProperty()->SetFontSize(16);
    m_overlayXAxis->GetTitleTextProperty()->SetBold(false);
    m_overlayYAxis->GetTitleTextProperty()->SetFontSize(16);
    m_overlayYAxis->GetTitleTextProperty()->SetOrientation(90.);
    m_overlayYAxis->GetTitleTextProperty()->SetBold(false);
    m_overlayXTitle->GetTextProperty()->SetColor(1., 1., 1.);
    m_overlayXTitle->GetTextProperty()->SetFontSize(14);
    m_overlayXTitle->GetTextProperty()->SetJustificationToCentered();
    m_overlayXTitle->GetTextProperty()->SetVerticalJustificationToCentered();
    m_overlayYTitle->GetTextProperty()->SetColor(1., 1., 1.);
    m_overlayYTitle->GetTextProperty()->SetFontSize(14);
    m_overlayYTitle->GetTextProperty()->SetOrientation(90.);
    m_overlayYTitle->GetTextProperty()->SetJustificationToCentered();
    m_overlayYTitle->GetTextProperty()->SetVerticalJustificationToCentered();
    CubeWcs::applyOverlayFont(m_overlayXTitle->GetTextProperty());
    CubeWcs::applyOverlayFont(m_overlayYTitle->GetTextProperty());

    m_renderer->AddViewProp(m_overlayXAxis);
    m_renderer->AddViewProp(m_overlayYAxis);
    m_renderer->AddViewProp(m_overlayXTitle);
    m_renderer->AddViewProp(m_overlayYTitle);
    this->ensureOverlayTickActors();

    m_overlayInitialized = true;
    this->setOverlayVisible(m_showWcsAxes);

    // --- EndEvent observer → updateWcsOverlay (clientData = this) ---------------
    m_endObserver = vtkSmartPointer<vtkCallbackCommand>::New();
    m_endObserver->SetClientData(this);
    m_endObserver->SetCallback([](vtkObject *, unsigned long, void *clientData, void *) {
        if (auto *self = static_cast<Pane2DView *>(clientData))
            self->updateWcsOverlay();
    });
    m_window->AddObserver(vtkCommand::EndEvent, m_endObserver);

    m_renderer->ResetCamera();     // sane camera distance; fit() letterboxes into the gutter
    this->fit(true);
}

Pane2DView::~Pane2DView()
{
    if (m_window && m_endObserver)
        m_window->RemoveObserver(m_endObserver);
    // m_widget is owned by its Qt parent (whoever mounts it); if never mounted it
    // is parentless — delete it so the render window is released cleanly.
    if (m_widget && !m_widget->parent())
        delete m_widget;
}

QVTKOpenGLNativeWidget *Pane2DView::widget() const
{
    return m_widget;
}

double Pane2DView::dpr() const
{
    return (m_widget && m_widget->devicePixelRatioF() > 0.) ? m_widget->devicePixelRatioF() : 1.;
}

void Pane2DView::ensureOverlayTickActors()
{
    if (!m_renderer)
        return;
    if (m_overlayXTicks.empty()) {
        for (int i = 0; i < kOverlayTickCount; ++i) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            CubeWcs::configureTickLabelActor(actor, /*rightAligned=*/false);
            m_renderer->AddViewProp(actor);
            m_overlayXTicks.push_back(actor);
        }
    }
    if (m_overlayYTicks.empty()) {
        for (int i = 0; i < kOverlayTickCount; ++i) {
            auto actor = vtkSmartPointer<vtkTextActor>::New();
            CubeWcs::configureTickLabelActor(actor, /*rightAligned=*/true);
            m_renderer->AddViewProp(actor);
            m_overlayYTicks.push_back(actor);
        }
    }
}

void Pane2DView::setOverlayVisible(bool visible)
{
    if (m_overlayXAxis)
        m_overlayXAxis->SetVisibility(visible);
    if (m_overlayYAxis)
        m_overlayYAxis->SetVisibility(visible);
    if (m_overlayXTitle)
        m_overlayXTitle->SetVisibility(visible);
    if (m_overlayYTitle)
        m_overlayYTitle->SetVisibility(visible);
    for (const auto &a : m_overlayXTicks)
        if (a)
            a->SetVisibility(visible);
    for (const auto &a : m_overlayYTicks)
        if (a)
            a->SetVisibility(visible);
}

void Pane2DView::setImageData(vtkImageData *img, double lo, double hi)
{
    // Point the pipeline at the caller's image; a null image keeps the placeholder
    // so nothing downstream ever sees a null input.
    if (img)
        m_source->SetOutput(img);
    else
        m_source->SetOutput(makePlaceholderImage());
    m_colors->SetInputConnection(m_source->GetOutputPort());
    m_lut->SetTableRange(lo, hi);
    m_lut->Modified();
    // Do NOT fit here — the caller calls fit() once it has set the colormap.
}

void Pane2DView::setColorMap(const QString &name, LutScale::Mode mode, double gamma,
                             bool clampOutliersTransparent)
{
    // §5.3: remember the state on the pane (authoritative), don't discard it.
    m_colorMapName = name;
    m_scale = mode;
    m_gamma = gamma;
    m_clampOutliers = clampOutliersTransparent;
    m_lutInitialized = true;
    rebuildLut();
}

void Pane2DView::rebuildLut()
{
    double range[2] = { 0., 0. };
    m_lut->GetTableRange(range);   // build against the CURRENT table range
    LutScale::build(m_lut, m_colorMapName.toStdString(), m_scale, range[0], range[1], m_gamma);
    if (m_invert) {
        // Reverse the colour ramp in place (ColorMaps has no reverse; flip the table).
        const vtkIdType n = m_lut->GetNumberOfTableValues();
        std::vector<std::array<double, 4>> tmp(static_cast<size_t>(n));
        for (vtkIdType i = 0; i < n; ++i)
            m_lut->GetTableValue(i, tmp[static_cast<size_t>(i)].data());
        for (vtkIdType i = 0; i < n; ++i)
            m_lut->SetTableValue(n - 1 - i, tmp[static_cast<size_t>(i)].data());
    }
    if (m_clampOutliers) {
        m_lut->SetUseAboveRangeColor(1);
        m_lut->SetAboveRangeColor(0., 0., 0., 0.);
        m_lut->SetUseBelowRangeColor(1);
        m_lut->SetBelowRangeColor(0., 0., 0., 0.);
    } else {
        m_lut->SetUseAboveRangeColor(0);
        m_lut->SetUseBelowRangeColor(0);
    }
    m_lut->SetNanColor(0., 0., 0., 0.);   // masked pixels stay transparent through rebuilds
    m_lut->Modified();
}

vtkImageData *Pane2DView::imageData() const
{
    return m_source ? vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0)) : nullptr;
}

double Pane2DView::rangeMin() const
{
    double r[2] = { 0., 0. };
    if (m_lut)
        m_lut->GetTableRange(r);
    return r[0];
}

double Pane2DView::rangeMax() const
{
    double r[2] = { 0., 0. };
    if (m_lut)
        m_lut->GetTableRange(r);
    return r[1];
}

void Pane2DView::setColorMapName(const QString &name)
{
    if (name.isEmpty() || name == m_colorMapName)
        return;
    m_colorMapName = name;
    m_lutInitialized = true;
    rebuildLut();
    this->fit(true);   // range→colorbar gutter is re-measured in fit()
    emit lutChanged();
}

void Pane2DView::setScale(LutScale::Mode mode)
{
    if (mode == m_scale)
        return;
    m_scale = mode;
    rebuildLut();
    this->fit(true);
    emit lutChanged();
}

void Pane2DView::setInvert(bool on)
{
    if (on == m_invert)
        return;
    m_invert = on;
    rebuildLut();
    this->fit(true);
    emit lutChanged();
}

void Pane2DView::setRange(double lo, double hi)
{
    if (!(hi > lo))
        return;   // ignore degenerate/inverted input
    double cur[2] = { 0., 0. };
    m_lut->GetTableRange(cur);
    if (std::fabs(cur[0] - lo) < 1e-12 && std::fabs(cur[1] - hi) < 1e-12)
        return;
    m_lut->SetTableRange(lo, hi);   // authoritative range — the mapper + colorbar honor THIS
    m_lut->Modified();
    this->fit(true);
    emit rangeChanged(lo, hi);
}

void Pane2DView::setWcsContext(WcsContext ctx)
{
    m_wcs = std::move(ctx);
    // Force the overlay cache to recompute on the next render.
    m_overlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN() };
    m_overlayViewportSize = { -1, -1 };
}

void Pane2DView::setWcsAxesVisible(bool on)
{
    m_showWcsAxes = on;
    this->setOverlayVisible(on);
    this->fit(true);   // gutter collapses / expands with the axes, so re-fit + refresh
}

void Pane2DView::setInteractive(bool on)
{
    // §5.1: gating is done by the container swallowing non-active mouse events at the Qt
    // level — the interactor must stay ENABLED or QVTKOpenGLNativeWidget paints black.
    // Kept as a no-op hook so the container has a single call site if a future VTK build
    // lets us disable input without killing the render.
    Q_UNUSED(on);
}

void Pane2DView::fit(bool force)
{
    if (!m_window || !m_renderer)
        return;
    const int *size = m_renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0)
        return;

    // The gutter (hence the data-area size) depends on the LUT range via the measured
    // colorbar column, so a range change must re-fit even at the same viewport (§3.0b).
    const double d = dpr();
    const auto gutter = computeGutter(m_window, m_lut, m_showWcsAxes, d);
    const std::array<int, 2> viewport = { size[0], size[1] };
    if (!force && viewport == m_fitSize && gutter.right == m_fitRight)
        return;   // same geometry — do NOT touch the camera (the user may have zoomed)

    auto *image = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0));
    CubeGeom::ImageRect dataArea;
    const bool fitted = CubeGeom::fitImageInGutter(m_renderer, image, gutter, &dataArea);
    if (fitted) {
        CubeGeom::layoutColorbarInGutter(m_colorbar, m_renderer, gutter, dataArea, d);
        m_fitSize = viewport;
        m_fitRight = gutter.right;
    }

    // Invalidate the overlay cache so the next EndEvent recomputes.
    m_overlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN() };
    m_overlayViewportSize = { -1, -1 };
    m_window->Render();
}

void Pane2DView::updateWcsOverlay()
{
    if (!m_overlayInitialized || !m_window || !m_renderer)
        return;
    if (!m_overlayXAxis || !m_overlayYAxis || !m_overlayXTitle || !m_overlayYTitle)
        return;

    auto *imageData = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0));

    this->setOverlayVisible(m_showWcsAxes);
    if (!m_showWcsAxes || !imageData)
        return;
    // If the caller never supplied WCS closures, skip the overlay gracefully — but
    // hide the actors first so no stale axes/ticks linger from a previous context.
    if (!m_wcs.voxelToWcs || !m_wcs.axisTitle || !m_wcs.formatCoord) {
        this->setOverlayVisible(false);
        return;
    }

    this->ensureOverlayTickActors();
    if (m_overlayXTicks.size() < static_cast<std::size_t>(kOverlayTickCount)
        || m_overlayYTicks.size() < static_cast<std::size_t>(kOverlayTickCount))
        return;

    const auto visible = CubeGeom::computeVisibleImageBounds2D(m_renderer, imageData);
    if (!visible.valid) {
        // Nothing of the image is on screen: hide every overlay actor (axes, titles
        // AND ticks) so none linger from the last valid frame.
        this->setOverlayVisible(false);
        return;
    }

    const int *size = m_renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0)
        return;

    const std::array<double, 4> visibleBounds = { visible.xmin, visible.xmax, visible.ymin,
                                                  visible.ymax };
    const std::array<int, 2> viewportSize = { size[0], size[1] };
    // Camera MTime is part of the key: a pan/zoom that keeps the full image visible
    // leaves visibleBounds unchanged but moves the projected axis rect.
    const unsigned long camMTime = m_renderer->GetActiveCamera()->GetMTime();
    if (visibleBounds == m_overlayVisibleBounds && viewportSize == m_overlayViewportSize
        && camMTime == m_overlayCameraMTime)
        return;
    m_overlayVisibleBounds = visibleBounds;
    m_overlayViewportSize = viewportSize;
    m_overlayCameraMTime = camMTime;

    const double d = dpr();
    const auto gutter = computeGutter(m_window, m_lut, m_showWcsAxes, d);
    // Derive the data area at the CURRENT viewport size — never a cached member (§2.7).
    const auto data = CubeGeom::dataAreaFor(size, imageData, gutter);
    const double axisX = data.valid ? data.x : gutter.left;
    const double xRight = data.valid ? data.x + data.width : size[0] - gutter.right;
    const double yTop = data.valid ? data.y + data.height : size[1] - gutter.top;
    const double bottomMargin = data.valid ? data.y : gutter.bottom;

    CubeWcs::configureAxisActor(m_overlayXAxis, axisX, bottomMargin, xRight, bottomMargin);
    CubeWcs::configureAxisActor(m_overlayYAxis, axisX, yTop, axisX, bottomMargin);
    m_overlayXAxis->SetTitle("");
    m_overlayXTitle->SetInput(m_wcs.axisTitle(0).toStdString().c_str());
    // Titles travel with their axis (§2.7).
    m_overlayXTitle->SetDisplayPosition(
            static_cast<int>((axisX + xRight) / 2.),
            static_cast<int>(std::max(bottomMargin - 38. * d, 10. * d)));
    m_overlayYAxis->SetTitle("");
    m_overlayYTitle->SetInput(m_wcs.axisTitle(1).toStdString().c_str());
    m_overlayYTitle->SetDisplayPosition(static_cast<int>(std::max(axisX - 74. * d, 12. * d)),
                                        static_cast<int>((bottomMargin + yTop) / 2.));

    // The image exactly fills the data area — range and ticks from the visible bounds.
    const double rxMin = visible.xmin;
    const double rxMax = visible.xmax;
    const double ryMin = visible.ymin;
    const double ryMax = visible.ymax;
    bool xOk = false;
    bool yOk = false;
    const double xMin = m_wcs.voxelToWcs(0, rxMin, &xOk);
    const double xMax = m_wcs.voxelToWcs(0, rxMax, &xOk);
    const double yMin = m_wcs.voxelToWcs(1, ryMin, &yOk);
    const double yMax = m_wcs.voxelToWcs(1, ryMax, &yOk);
    m_overlayXAxis->SetRange(xOk ? xMin : rxMin, xOk ? xMax : rxMax);
    m_overlayYAxis->SetRange(yOk ? yMax : ryMax, yOk ? yMin : ryMin);
    m_overlayXAxis->LabelVisibilityOff();
    m_overlayYAxis->LabelVisibilityOff();
    m_overlayXAxis->VisibilityOn();
    m_overlayYAxis->VisibilityOn();
    m_overlayXTitle->VisibilityOn();
    m_overlayYTitle->VisibilityOn();

    const bool hasCelestial = m_wcs.hasCelestial && m_wcs.hasCelestial();
    for (int i = 0; i < kOverlayTickCount; ++i) {
        const double t = kOverlayTickCount == 1
                ? 0.0
                : static_cast<double>(i) / static_cast<double>(kOverlayTickCount - 1);
        const double voxelX = rxMin + t * (rxMax - rxMin);
        const double voxelY = ryMin + t * (ryMax - ryMin);
        bool tickXOk = false;
        bool tickYOk = false;
        const double frameX = m_wcs.voxelToWcs(0, voxelX, &tickXOk);
        const double frameY = m_wcs.voxelToWcs(1, voxelY, &tickYOk);
        // NOTE on frames: the original moment overlay does a PAIRED celestial
        // conversion (convertRemoteCelestialCoordinates(x,y,...)) because e.g.
        // FK5<->Galactic mixes both axes. This class's required API is per-axis
        // (voxelToWcs/formatCoord take one axis), so the caller MUST supply closures
        // that already return values in the final display frame — this view cannot
        // re-project across axes. hasCelestial is exposed for callers/formatters that
        // want to branch on it; the tick loop itself needs no cross-axis mixing.
        (void)hasCelestial;
        const auto x = static_cast<std::size_t>(i);
        m_overlayXTicks[x]->SetInput(m_wcs.formatCoord(0, frameX).toStdString().c_str());
        m_overlayXTicks[x]->SetDisplayPosition(static_cast<int>(axisX + t * (xRight - axisX)),
                                               static_cast<int>(bottomMargin - 18. * d));
        // Drop the leftmost x label — corner collision with the bottom y label.
        m_overlayXTicks[x]->SetVisibility(i != 0);
        m_overlayYTicks[x]->SetInput(m_wcs.formatCoord(1, frameY).toStdString().c_str());
        m_overlayYTicks[x]->SetDisplayPosition(
                static_cast<int>(axisX - 10. * d),
                static_cast<int>(bottomMargin + t * (yTop - bottomMargin)));
        m_overlayYTicks[x]->VisibilityOn();
    }
}

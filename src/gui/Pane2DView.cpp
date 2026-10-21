#include "Pane2DView.h"

#include "ColorMaps.h"
#include "CubeViewportGeometry.h"
#include "CubeWcsUtil.h"

#include <QDebug>
#include <QScopedValueRollback>
#include <QVTKInteractor.h>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkActor.h>
#include <vtkAxisActor2D.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
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
#include <vtkObjectFactory.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <cmath>

namespace {

// §5.2: window/level drag redirected to the LUT TABLE RANGE (not the vtkImageProperty).
// vtkInteractorStyleImage's default windows the image property's ColorWindow/ColorLevel,
// a post-scale of the already-coloured RGBA that the colorbar + mapper DON'T see — the
// "washout". Here a LEFT-drag rewrites the pane's LUT range instead (horizontal = window
// width, vertical = level/centre), so drag, colorbar and the dock Range fields are ONE
// state. Pan (middle) + zoom (wheel/right) fall through to the base class.
class PaneWindowLevelStyle : public vtkInteractorStyleImage
{
public:
    static PaneWindowLevelStyle *New();
    vtkTypeMacro(PaneWindowLevelStyle, vtkInteractorStyleImage);
    std::function<double()> getLo, getHi;
    std::function<void(double, double)> setRange;

    // §5.2: window/level is the LEFT-drag (the natural gesture; the macOS trackpad
    // phantom-press bug is fixed via setEnableTouchEventProcessing(false), so a deliberate
    // drag is safe). We DON'T call the base OnLeftButtonDown, which would window the image
    // PROPERTY (the RGBA washout) — we redirect to the LUT range instead. Wheel = zoom,
    // middle = pan from the base class.
    void OnLeftButtonDown() override
    {
        if (!setRange || !this->GetInteractor())
            return;
        const int *p = this->GetInteractor()->GetEventPosition();
        m_x0 = p[0];
        m_y0 = p[1];
        m_lo0 = getLo ? getLo() : 0.;
        m_hi0 = getHi ? getHi() : 1.;
        m_dragging = true;
    }
    void OnLeftButtonUp() override { m_dragging = false; }
    void OnMouseMove() override
    {
        if (!m_dragging) {
            Superclass::OnMouseMove();   // pan/zoom in-progress, hover — base behaviour
            return;
        }
        const int *p = this->GetInteractor()->GetEventPosition();
        int size[2] = { 1, 1 };
        if (auto *rw = this->GetInteractor()->GetRenderWindow())
            if (const int *s = rw->GetSize()) {
                size[0] = s[0];
                size[1] = s[1];
            }
        const double W = std::max(1, size[0]);
        const double H = std::max(1, size[1]);
        double window = m_hi0 - m_lo0;
        if (!(window > 0.))
            window = 1.;
        const double level = 0.5 * (m_hi0 + m_lo0);
        const double dx = p[0] - m_x0;
        const double dy = p[1] - m_y0;
        const double newWindow = window * std::exp(2.0 * dx / W);   // exp → always positive
        const double newLevel = level - (dy / H) * window;
        setRange(newLevel - 0.5 * newWindow, newLevel + 0.5 * newWindow);
    }

private:
    int m_x0{ 0 }, m_y0{ 0 };
    double m_lo0{ 0. }, m_hi0{ 1. };
    bool m_dragging{ false };
};
vtkStandardNewMacro(PaneWindowLevelStyle);


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
                                    bool showWcsAxes, double dpr, int bottomReserve = 0)
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
    return CubeGeom::fitGutterWithBottomReserve(CubeGeom::imageGutter(showWcsAxes, rightGutter, dpr),
                                                win ? win->GetSize() : nullptr, bottomReserve * dpr);
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
    vtkNew<PaneWindowLevelStyle> style;
    style->getLo = [this]() { return this->rangeMin(); };
    style->getHi = [this]() { return this->rangeMax(); };
    style->setRange = [this](double lo, double hi) { this->setRange(lo, hi); };
    if (auto *iren = m_window->GetInteractor()) {
        iren->SetInteractorStyle(style);
        // Linked 2D: pan/zoom (InteractionEvent) → emit cameraChanged so the viewer can sync
        // same-WCS peers. Guarded so an applied (synced) camera doesn't echo back.
        m_cameraObserver = vtkSmartPointer<vtkCallbackCommand>::New();
        m_cameraObserver->SetClientData(this);
        m_cameraObserver->SetCallback([](vtkObject *, unsigned long, void *cd, void *) {
            if (auto *self = static_cast<Pane2DView *>(cd))
                if (!self->m_applyingCamera)
                    emit self->cameraChanged();
        });
        // InteractionEvent follows live pan/dolly; EndInteractionEvent catches mouse-WHEEL
        // zoom, which does NOT emit InteractionEvent (same as the 3-D linked-camera path).
        m_camTagInteract = iren->AddObserver(vtkCommand::InteractionEvent, m_cameraObserver);
        m_camTagEnd = iren->AddObserver(vtkCommand::EndInteractionEvent, m_cameraObserver);
    }

    // --- display pipeline: source → colors(LUT) → slice mapper → slice actor ----
    m_source = vtkSmartPointer<vtkTrivialProducer>::New();
    m_source->SetOutput(makePlaceholderImage());

    m_lut = vtkSmartPointer<vtkLookupTable>::New();
    m_lut->SetTableRange(0., 0.);
    m_lut->SetNanColor(0., 0., 0., 0.);   // masked / NaN pixels read as ABSENT (transparent)
    // Seed a real colormap, NOT vtkLookupTable::Build()'s raw HSV rainbow: a pane rendered
    // before its by-kind setColorMap() lands (or with an empty name) must never flash the
    // rainbow — it reads as a broken "MinMax" default (§5.4b).
    m_colorMapName = QString::fromStdString(ColorMaps::DefaultColorMap);
    ColorMaps::SetColorMap(m_lut, ColorMaps::DefaultColorMap);
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

    // --- an overlaid path (the PV cut, same colour as on the slice) -------------
    m_pathPoints = vtkSmartPointer<vtkPoints>::New();
    m_pathCells = vtkSmartPointer<vtkCellArray>::New();
    m_pathData = vtkSmartPointer<vtkPolyData>::New();
    m_pathData->SetPoints(m_pathPoints);
    m_pathData->SetLines(m_pathCells);
    {
        vtkNew<vtkPolyDataMapper> pathMapper;
        pathMapper->SetInputData(m_pathData);
        m_pathActor = vtkSmartPointer<vtkActor>::New();
        m_pathActor->SetMapper(pathMapper);
        m_pathActor->GetProperty()->SetColor(1.0, 0.4, 0.1);
        m_pathActor->GetProperty()->SetLineWidth(2.5);
        m_pathActor->VisibilityOff();
        m_renderer->AddActor(m_pathActor);
    }

    // --- colorbar (parked in the right gutter, laid out by fit()) ---------------
    m_colorbar = vtkSmartPointer<vtkScalarBarActor>::New();
    m_colorbar->SetLookupTable(m_lut);
    CubeWcs::applyOverlayFont(m_colorbar->GetLabelTextProperty());
    m_renderer->AddViewProp(m_colorbar);
    CubeGeom::layoutColorbarInGutter(m_colorbar, m_renderer,
                                     computeGutter(m_window, m_lut, m_showWcsAxes, dpr(), m_bottomReserve),
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
    // §5.2: detach the window/level style FIRST — its getLo/getHi/setRange closures
    // capture `this`, so it must not survive us and fire on a late interactor event.
    if (m_window && m_window->GetInteractor()) {
        auto *iren = m_window->GetInteractor();
        if (m_camTagInteract)
            iren->RemoveObserver(m_camTagInteract);
        if (m_camTagEnd)
            iren->RemoveObserver(m_camTagEnd);
        iren->SetInteractorStyle(nullptr);
    }
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

void Pane2DView::setImageData(vtkImageData *img, double lo, double hi,
                              std::optional<LutScale::Mode> destScale)
{
    // Point the pipeline at the caller's image; a null image keeps the placeholder
    // so nothing downstream ever sees a null input.
    if (img)
        m_source->SetOutput(img);
    else
        m_source->SetOutput(makePlaceholderImage());
    m_colors->SetInputConnection(m_source->GetOutputPort());
    // The range is installed here and read back by rebuildLut(), so a log
    // destination has to be resolved NOW rather than later, when the transition
    // guard may decide nothing changed.
    //
    // `destScale` also says WHICH kind of mount this is, and the two want
    // opposite things. Given (a fresh product adopting its saved stretch) the
    // floor should come from the data. Omitted, the caller is re-mounting the
    // same product — a recompute, or a layout change — and deliberately passing
    // this pane's own range back in; anchoring there would throw away a lower
    // bound the user set by hand every time the layout changed.
    const LutScale::Mode dest = destScale.value_or(m_scale);
    double floorValue = 0.;
    if (dest != LutScale::Mode::Log) {
        LutScale::setRange(m_lut, dest, lo, hi);
    } else if (!logFloor(floorValue)) {
        // Log is impossible for these pixels. Commit the demotion here: the
        // same-product path never reaches setColorMap(), so nothing downstream
        // would, and the pane would sit with m_scale saying Log over a linear
        // table — controls offering a switch that returns immediately, and the
        // next colormap edit quietly rebuilding a logarithmic one.
        //
        // And the range has to come from the NEW image, not from `lo`/`hi`: on
        // the same-product path those are the pane's existing range, which under
        // log is entirely positive. Keeping it while demoting an image that has
        // gone all-negative would put every pixel below the bottom of the table
        // — one flat colour, or nothing at all with outlier clamping on.
        double raw[2] = { lo, hi };
        if (auto *newImg = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0))) {
            double sr[2] = { 0., 0. };
            newImg->GetScalarRange(sr);
            if (std::isfinite(sr[0]) && std::isfinite(sr[1]) && sr[1] > sr[0]) {
                raw[0] = sr[0];
                raw[1] = sr[1];
            }
        }
        LutScale::setRange(m_lut, LutScale::Mode::Linear, raw[0], raw[1]);
        if (m_scale == LutScale::Mode::Log) {
            m_scale = LutScale::Mode::Linear;
            emit logScaleUnavailable();
        }
    } else if (destScale.has_value()) {
        LutScale::setRange(m_lut, dest, floorValue,
                           std::max(hi, floorValue * (1.0 + 1e-6)));
    } else {
        LutScale::setRange(m_lut, dest, lo, hi);   // the pane's own range, kept
    }
    m_lut->Modified();
    // Do NOT fit here — the caller calls fit() once it has set the colormap.
}

void Pane2DView::setColorMap(const QString &name, LutScale::Mode mode, double gamma,
                             bool clampOutliersTransparent)
{
    // force: both callers are a slot adopting a product's saved state, so the
    // image is new even when the mode is not.
    if (!anchorLogFloor(mode, /*force=*/true)) {
        // Log is not viable for this image. Adopt everything else rather than
        // bailing out — returning here left the pane holding the previous
        // product's colormap over the new pixels, and (because setImageData had
        // already switched the table) a logarithmic LUT under a linear m_scale.
        mode = (m_scale == LutScale::Mode::Log) ? LutScale::Mode::Linear : m_scale;
    }
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
    // Empty name → ColorMaps::SetColorMap is a no-op → the raw rainbow would survive; fall
    // back to the default so the table is always a real colormap AND the stored name matches
    // what's drawn (so colorMapName()/the strip aren't blank) (§5.4b).
    if (m_colorMapName.isEmpty())
        m_colorMapName = QString::fromStdString(ColorMaps::DefaultColorMap);
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

// Entering log: take the floor from the data rather than leaving LutScale to
// fall back on max x 1e-6, six fixed decades that spend most of the ramp below
// anything the image contains. rebuildLut() builds at the CURRENT table range,
// so the floor has to be in place before it runs.
//
// Returns false when log was asked for and the image has no positive values —
// the caller must then leave the scale alone.
//
// Called from the paths that can put the pane INTO log carrying a mode but no
// range: setScale(), applyLut() (how a linked pane adopts a stretch) and
// setColorMap() (how a slot adopts a newly mounted product). Not from
// adoptLutState(), which only records what LUTCustomizerDialog has already
// built — it derives its own positive floor, and re-anchoring would overwrite
// the range the user just chose in it.
//
// Only on the TRANSITION into log. Anchoring on every applyLut() would reset a
// lower bound the user had adjusted by hand whenever an unrelated colormap,
// gamma or invert change propagated in from a linked window.
bool Pane2DView::logFloor(double &floorOut) const
{
    auto *img = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0));
    floorOut = LutScale::sampledPositiveFloor(img);
    return floorOut > 0.0;
}

bool Pane2DView::anchorLogFloor(LutScale::Mode mode, bool force)
{
    if (mode != LutScale::Mode::Log)
        return true;
    // `force` is for the paths where the DATA changed rather than the mode: a
    // slot mounting a different product into a pane that is already logarithmic
    // needs a floor for the new image, and the transition guard alone would skip
    // it, leaving the blind fallback setImageData had to install.
    if (!force && m_scale == LutScale::Mode::Log)
        return true;
    double floorValue = 0.;
    if (!logFloor(floorValue)) {
        emit logScaleUnavailable();
        return false;
    }
    double cur[2] = { 0., 0. };
    m_lut->GetTableRange(cur);
    m_lut->SetTableRange(floorValue, std::max(cur[1], floorValue * (1.0 + 1e-6)));
    return true;
}

void Pane2DView::setScale(LutScale::Mode mode)
{
    if (mode == m_scale)
        return;
    if (!anchorLogFloor(mode))
        return;
    m_scale = mode;
    rebuildLut();
    this->fit(true);
    emit lutChanged();
}

void Pane2DView::applyLut(const QString &name, LutScale::Mode scale, double gamma, bool invert)
{
    if (!anchorLogFloor(scale))
        return;
    if (!name.isEmpty())
        m_colorMapName = name;
    m_scale = scale;
    m_gamma = gamma;
    m_invert = invert;
    m_lutInitialized = true;
    rebuildLut();
    this->fit(true);
    emit lutChanged();
}

void Pane2DView::adoptLutState(const QString &name, LutScale::Mode scale, double gamma)
{
    if (!name.isEmpty())
        m_colorMapName = name;
    m_scale = scale;
    m_gamma = gamma;
    m_lutInitialized = true;
    this->fit(true);   // range/colorbar may have changed in the dialog
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
    if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo))
        return;   // ignore non-finite / degenerate / inverted input (drag can overshoot)
    // Compare — and emit — the range that will actually be installed. Under a log
    // stretch of signed data the requested low end is floored, so testing the
    // request against the table would never match: the guard would never fire and
    // two linked panes would echo the same change at each other forever.
    const std::array<double, 2> eff = LutScale::rangeFor(m_scale, lo, hi);
    double cur[2] = { 0., 0. };
    m_lut->GetTableRange(cur);
    if (std::fabs(cur[0] - eff[0]) < 1e-12 && std::fabs(cur[1] - eff[1]) < 1e-12)
        return;
    LutScale::setRange(m_lut, m_scale, lo, hi);   // authoritative — mapper + colorbar honor THIS
    m_lut->Modified();
    this->fit(true);
    emit rangeChanged(eff[0], eff[1]);
}

void Pane2DView::cameraState(double focalXY[2], double &parallelScale) const
{
    auto *cam = m_renderer->GetActiveCamera();
    double f[3] = { 0., 0., 0. };
    cam->GetFocalPoint(f);
    focalXY[0] = f[0];
    focalXY[1] = f[1];
    parallelScale = cam->GetParallelScale();
}

void Pane2DView::setCameraState(const double focalXY[2], double parallelScale)
{
    if (!std::isfinite(focalXY[0]) || !std::isfinite(focalXY[1]) || !(parallelScale > 0.))
        return;
    // E-07/M7: RAII guard — don't echo cameraChanged back out (Linked-2D loop
    // guard); a throw during Render() must not wedge the flag true and silence
    // all future camera-change signals from this pane.
    QScopedValueRollback<bool> cameraGuard(m_applyingCamera, true);
    auto *cam = m_renderer->GetActiveCamera();
    double f[3] = { 0., 0., 0. }, p[3] = { 0., 0., 0. };
    cam->GetFocalPoint(f);
    cam->GetPosition(p);
    // Parallel projection looking straight down Z: pan = move focal + position x/y together;
    // zoom = parallel scale. Preserve the z components (camera distance/direction).
    f[0] = focalXY[0];
    f[1] = focalXY[1];
    p[0] = focalXY[0];
    p[1] = focalXY[1];
    cam->SetFocalPoint(f);
    cam->SetPosition(p);
    cam->SetParallelScale(parallelScale);
    if (m_window)
        m_window->Render();
    // cameraGuard restores m_applyingCamera on scope exit (RAII).
}

bool Pane2DView::cameraStateNormalized(double dataCenterXY[2], double &worldHeight) const
{
    // STEP6-P3: express the view independently of THIS pane's gutter — as the world point at the
    // data-area centre + the world height filling the data area. Cancels the colorbar-column
    // offset so a copy between different-gutter panes frames identically.
    if (!m_window || !m_renderer || !m_source)
        return false;
    const int *size = m_renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0)
        return false;
    auto *image = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0));
    const double d = dpr();
    const auto gutter = computeGutter(m_window, m_lut, m_showWcsAxes, d, m_bottomReserve);
    const auto data = CubeGeom::dataAreaFor(size, image, gutter);
    return CubeGeom::readNormalizedCamera(m_renderer, data, dataCenterXY, worldHeight);
}

void Pane2DView::setCameraStateNormalized(const double dataCenterXY[2], double worldHeight)
{
    // STEP6-P3: inverse of cameraStateNormalized — convert the gutter-independent (data-centre,
    // world-height) view back to THIS pane's own focal + parallel scale using its own gutter.
    if (!m_window || !m_renderer || !m_source)
        return;
    if (!std::isfinite(dataCenterXY[0]) || !std::isfinite(dataCenterXY[1]) || !(worldHeight > 0.))
        return;
    const int *size = m_renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0)
        return;
    auto *image = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0));
    const double d = dpr();
    const auto gutter = computeGutter(m_window, m_lut, m_showWcsAxes, d, m_bottomReserve);
    const auto data = CubeGeom::dataAreaFor(size, image, gutter);
    double focalXY[2] = { 0., 0. }, parScale = 0.;
    if (!CubeGeom::normalizedToFocalScale(size, data, dataCenterXY, worldHeight, focalXY, parScale))
        return;
    this->setCameraState(focalXY, parScale);   // reuse the echo-guarded focal/scale setter
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

void Pane2DView::setBottomReserve(int logicalPx)
{
    if (m_bottomReserve == logicalPx)
        return;
    m_bottomReserve = logicalPx;
    m_overlayViewportSize = { -1, -1 };
    this->fit(true);
}

void Pane2DView::setWcsAxesVisible(bool on)
{
    m_showWcsAxes = on;
    this->setOverlayVisible(on);
    m_overlayViewportSize = { -1, -1 };   // recompute (and re-decide each label) next frame
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
    const auto gutter = computeGutter(m_window, m_lut, m_showWcsAxes, d, m_bottomReserve);
    const std::array<int, 2> viewport = { size[0], size[1] };
    if (!force && viewport == m_fitSize && gutter.right == m_fitRight
        && gutter.bottom == m_fitBottom)
        return;   // same geometry — do NOT touch the camera (the user may have zoomed)

    auto *image = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0));
    CubeGeom::ImageRect dataArea;
    const bool didFit = CubeGeom::fitImageInGutter(m_renderer, image, gutter, &dataArea);
    if (didFit) {
        CubeGeom::layoutColorbarInGutter(m_colorbar, m_renderer, gutter, dataArea, d);
        m_fitSize = viewport;
        m_fitRight = gutter.right;
        m_fitBottom = gutter.bottom;
    }

    // Invalidate the overlay cache so the next EndEvent recomputes.
    m_overlayVisibleBounds = { std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN() };
    m_overlayViewportSize = { -1, -1 };
    m_window->Render();
    // STEP6-P2: a fit() re-letterboxes to this pane's own bounds, discarding any shared Linked-2D
    // zoom/pan. Tell the window so it can re-assert the shared camera on a linked receiver. Guarded
    // by m_applyingCamera so a re-assert's own fit (if any) can't recurse.
    if (didFit && !m_applyingCamera)
        emit this->fitted();
}

void Pane2DView::updateWcsOverlay()
{
    if (!m_overlayInitialized || !m_window || !m_renderer)
        return;
    if (!m_overlayXAxis || !m_overlayYAxis || !m_overlayXTitle || !m_overlayYTitle)
        return;

    auto *imageData = vtkImageData::SafeDownCast(m_source->GetOutputDataObject(0));

    // Visibility is decided below, per actor (the leftmost x label, crowded labels,
    // a title with no room): switching everything on here and then returning on an
    // unchanged camera undid those choices on every frame.
    if (!m_showWcsAxes || !imageData) {
        this->setOverlayVisible(false);
        return;
    }
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
    const auto gutter = computeGutter(m_window, m_lut, m_showWcsAxes, d, m_bottomReserve);
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
    // A narrow pane squeezes the axis column (fitGutterToViewport): the numbers
    // stay, the rotated title would sit on top of them.
    const bool narrowAxis = gutter.left < 0.8 * CubeGeom::ImageGutter{}.left * d;
    // A short pane above a channel bar: the title would land on the bar.
    const bool shortAxis = m_bottomReserve > 0 && bottomMargin - m_bottomReserve * d < 46. * d;
    m_overlayXTitle->SetVisibility(!shortAxis);
    m_overlayYTitle->SetVisibility(!narrowAxis);

    const bool hasCelestial = m_wcs.hasCelestial && m_wcs.hasCelestial();
    // Ticks in the display frame: paired conversion when the caller provides it.
    const auto frameAt = [&](double vx, double vy, double &fx, double &fy, bool &okX, bool &okY) {
        fx = m_wcs.voxelToWcs(0, vx, &okX);
        fy = m_wcs.voxelToWcs(1, vy, &okY);
        if (okX && okY && hasCelestial && m_wcs.voxelToFrame) {
            double gx = fx, gy = fy;
            if (m_wcs.voxelToFrame(vx, vy, gx, gy)) {
                fx = gx;
                fy = gy;
            }
        }
    };
    double spanX = std::fabs(xMax - xMin);
    double spanY = std::fabs(yMax - yMin);
    if (m_wcs.voxelToFrame && hasCelestial) {
        double fx0 = 0., fy0 = 0., fx1 = 0., fy1 = 0.;
        bool a = false, b = false, c = false, e = false;
        frameAt(rxMin, ryMin, fx0, fy0, a, b);
        frameAt(rxMax, ryMax, fx1, fy1, c, e);
        if (a && b && c && e) {
            spanX = std::fabs(fx1 - fx0);
            spanY = std::fabs(fy1 - fy0);
        }
    }
    for (int i = 0; i < kOverlayTickCount; ++i) {
        const double t = kOverlayTickCount == 1
                ? 0.0
                : static_cast<double>(i) / static_cast<double>(kOverlayTickCount - 1);
        const double voxelX = rxMin + t * (rxMax - rxMin);
        const double voxelY = ryMin + t * (ryMax - ryMin);
        bool tickXOk = false;
        bool tickYOk = false;
        double frameX = 0.0;
        double frameY = 0.0;
        frameAt(voxelX, voxelY, frameX, frameY, tickXOk, tickYOk);
        // NOTE on frames: the original moment overlay does a PAIRED celestial
        // conversion (convertRemoteCelestialCoordinates(x,y,...)) because e.g.
        // FK5<->Galactic mixes both axes. This class's required API is per-axis
        // (voxelToWcs/formatCoord take one axis), so the caller MUST supply closures
        // that already return values in the final display frame — this view cannot
        // re-project across axes. hasCelestial is exposed for callers/formatters that
        // want to branch on it; the tick loop itself needs no cross-axis mixing.
        (void)hasCelestial;
        const auto x = static_cast<std::size_t>(i);
        // The axis' WCS span (in the display frame, above) lets the formatter use enough
        // decimals for THIS zoom (a 0.007° moment field must not print "47.21" on every
        // tick) (b).
        m_overlayXTicks[x]->SetInput(m_wcs.formatCoord(0, frameX, spanX).toStdString().c_str());
        m_overlayXTicks[x]->SetDisplayPosition(static_cast<int>(axisX + t * (xRight - axisX)),
                                               static_cast<int>(bottomMargin - 18. * d));
        // Drop the leftmost x label — corner collision with the bottom y label.
        // In a narrow pane the five labels collide: keep every other one.
        const bool crowded = (xRight - axisX) / (kOverlayTickCount - 1) < 62. * d;
        m_overlayXTicks[x]->SetVisibility(i != 0 && (!crowded || i % 2 == 0));
        m_overlayYTicks[x]->SetInput(m_wcs.formatCoord(1, frameY, spanY).toStdString().c_str());
        m_overlayYTicks[x]->SetDisplayPosition(
                static_cast<int>(axisX - 10. * d),
                static_cast<int>(bottomMargin + t * (yTop - bottomMargin)));
        m_overlayYTicks[x]->VisibilityOn();
    }
}

void Pane2DView::setOverlayPath(const std::vector<std::array<double, 2>> &pixels)
{
    if (!m_pathActor || pixels == m_pathShown)
        return;
    m_pathShown = pixels;
    m_pathPoints->Reset();
    m_pathCells->Reset();
    if (pixels.size() >= 2) {
        m_pathCells->InsertNextCell(static_cast<vtkIdType>(pixels.size()));
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(pixels.size()); ++i) {
            m_pathPoints->InsertNextPoint(pixels[std::size_t(i)][0], pixels[std::size_t(i)][1], 0.0);
            m_pathCells->InsertCellPoint(i);
        }
    }
    m_pathPoints->Modified();
    m_pathData->Modified();
    m_pathActor->SetVisibility(pixels.size() >= 2);
    render();
}

void Pane2DView::render()
{
    if (m_window)
        m_window->Render();
}

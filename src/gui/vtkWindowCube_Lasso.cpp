// vtkWindowCube_Lasso.cpp
// ── Kinematic Lasso ──────────────────────────────────────────────────────────
// Interactive kinematics-aware 3-D selection: the user clicks a source they can
// SEE in the volume render; a vtkVolumePicker ray-casts the click to a seed
// voxel; the backend grows a geodesic, velocity-weighted region from that seed
// (/v1/cube/kinematic_select_bin); the returned bounding-box mask is contoured as
// a green isosurface overlay showing the selected object. No coordinates, no box.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "app/BackendClient.h"

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkContourFilter.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleUser.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkTrivialProducer.h>
#include <vtkVolumePicker.h>

#include <QApplication>
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QtConcurrentRun>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace Qt::StringLiterals;

namespace {

// Network-only worker (value copies; never touches `this`). Returns a GUI-side
// result; the VTK isosurface is built on the GUI thread (the ROI is small).
RemoteKinematicSelectResult fetchKinematicSelect(
        QString url, QString token, QString session, QString datasetId,
        int seedZ, int seedY, int seedX, double reach, double lambdaV, int roiRadius,
        QVector<std::array<int, 3>> addSeeds, QVector<std::array<int, 3>> subSeeds)
{
    BackendClient client(url, token);
    client.setSessionId(session);
    const BackendKinematicSelectResult b = client.kinematicSelect(
            datasetId, seedZ, seedY, seedX, reach, lambdaV, /*sigma=*/0.0,
            /*nsigma=*/2.5, roiRadius, addSeeds, subSeeds);
    RemoteKinematicSelectResult out;
    out.valid = b.valid;
    out.errorMessage = b.error;
    out.roiWidth = b.roiWidth; out.roiHeight = b.roiHeight; out.roiDepth = b.roiDepth;
    out.originX = b.originX; out.originY = b.originY; out.originZ = b.originZ;
    out.fullWidth = b.fullWidth; out.fullHeight = b.fullHeight; out.fullDepth = b.fullDepth;
    out.count = b.count;
    for (int i = 0; i < 6; ++i) out.bbox[i] = b.bbox[i];
    out.data = b.data;
    return out;
}

// Ray-casting point-in-polygon test over integer polygon vertices (x,y).
bool pointInPolygon(int px, int py, const std::vector<std::array<int, 2>> &poly)
{
    bool inside = false;
    const std::size_t n = poly.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = poly[i][0], yi = poly[i][1];
        const double xj = poly[j][0], yj = poly[j][1];
        const bool crosses = ((yi > py) != (yj > py))
                && (double(px) < (xj - xi) * (double(py) - yi) / (yj - yi) + xi);
        if (crosses)
            inside = !inside;
    }
    return inside;
}

}  // namespace

void vtkWindowCube::setKinematicLassoActive(bool active)
{
    this->m_lassoActive = active;
    auto *iren = ui->vtkCube && ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetInteractor() : nullptr;
    if (!iren) {
        return;
    }
    if (active) {
        // Selection is bound to the DOUBLE-click: single left-drag stays free for
        // rotating the volume to line up the source, then a double-click picks it.
        // This is unambiguous (the OS position-locks a double-click) — no fragile
        // click-vs-drag pixel heuristic that a firm tap or a HiDPI display defeats.
        if (!this->m_lassoReleaseTag) {
            this->m_lassoReleaseTag = iren->AddObserver(
                    vtkCommand::LeftButtonDoubleClickEvent, this, &vtkWindowCube::onLassoRelease, 1.0);
        }
        ui->vtkCube->setCursor(Qt::CrossCursor);
        // 0 = object-bounded: the backend grows the whole connected source from one
        // click (component of the ≥nσ emission), so a single double-click grabs the
        // entire galaxy/clump — no fixed ROI box to clip large extended objects.
        this->m_lassoRoiRadius = 0;
        // Open the panel right away so the controls are discoverable before the
        // first click, and paint an in-viewport prompt telling the user what to do.
        this->ensureLassoPanel();
        if (this->m_lassoPanel) this->m_lassoPanel->show();
        // Keep the mouse/keyboard focus on the 3-D view so the very first click
        // selects instead of being spent re-focusing the widget.
        ui->vtkCube->setFocus(Qt::OtherFocusReason);
        if (!this->m_lassoHasSeed) this->showLassoHint(true);
        this->statusBar()->showMessage(
                u"Kinematic Lasso: drag to rotate, then double-click a source to select it."_s, 0);
    } else {
        if (this->m_lassoReleaseTag) { iren->RemoveObserver(this->m_lassoReleaseTag); this->m_lassoReleaseTag = 0; }
        ui->vtkCube->setCursor(Qt::ArrowCursor);
        this->showLassoHint(false);
        this->statusBar()->clearMessage();
    }
}

void vtkWindowCube::showLassoHint(bool on)
{
    auto *renderer = ui->vtkCube && ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!on && !renderer) {
        this->m_lassoHintActor = nullptr;   // renderer already gone (teardown) → just release
        return;
    }
    if (!renderer) {
        return;
    }
    if (on) {
        if (!this->m_lassoHintActor) {
            this->m_lassoHintActor = vtkSmartPointer<vtkTextActor>::New();
            this->m_lassoHintActor->SetInput(
                    "Drag to rotate.  Double-click a source to select it.   "
                    "(Shift = add,  Alt = subtract)");
            auto *tp = this->m_lassoHintActor->GetTextProperty();
            tp->SetFontSize(13);
            tp->SetJustificationToCentered();
            tp->SetVerticalJustificationToTop();
            tp->SetColor(0.62, 0.78, 0.68);      // muted green — informative, not shouting
            tp->SetOpacity(0.75);
            tp->SetShadow(0);
            this->m_lassoHintActor->GetPositionCoordinate()
                    ->SetCoordinateSystemToNormalizedViewport();
            this->m_lassoHintActor->SetPosition(0.5, 0.985);
            renderer->AddActor2D(this->m_lassoHintActor);
        }
    } else if (this->m_lassoHintActor) {
        renderer->RemoveActor2D(this->m_lassoHintActor);
        this->m_lassoHintActor = nullptr;
    }
    if (ui->vtkCube->renderWindow()) ui->vtkCube->renderWindow()->Render();
}

// Selection handler — bound to the LEFT DOUBLE-click (single drag rotates freely).
void vtkWindowCube::onLassoRelease()
{
    if (!this->m_lassoActive || this->m_lassoSelectWatcher.isRunning()) {
        return;
    }
    auto *iren = ui->vtkCube && ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetInteractor() : nullptr;
    auto *renderer = ui->vtkCube && ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!iren || !renderer) {
        return;
    }
    int pos[2];
    iren->GetEventPosition(pos);

    // Ray-cast the click into the VOLUME: the picker stops at the visible surface
    // of the source (where the transfer function accumulates opacity) → the seed
    // voxel of whatever you clicked on.
    auto picker = vtkSmartPointer<vtkVolumePicker>::New();
    picker->SetVolumeOpacityIsovalue(0.05);
    if (picker->Pick(pos[0], pos[1], 0.0, renderer) == 0) {
        this->statusBar()->showMessage(u"Kinematic Lasso: click on visible emission."_s, 2000);
        return;
    }
    int ijk[3];
    picker->GetPointIJK(ijk);   // voxel index in the DISPLAYED cube

    // Map the displayed-cube voxel → full-resolution cube voxel (the backend
    // selection runs on the full cube). Two display conventions (same as the
    // kinematic-model overlay): a ROI subvolume has a non-zero image origin +
    // spacing 1 → world == full-res voxels, so full = origin + ijk; a preview /
    // full display has origin 0 → full = round(ijk·(full-1)/(ddim-1)).
    auto *cubeImage = vtkImageData::SafeDownCast(this->cubeDisplaySource->GetOutputDataObject(0));
    if (!cubeImage) {
        return;
    }
    int ddim[3];
    double dorg[3];
    cubeImage->GetDimensions(ddim);
    cubeImage->GetOrigin(dorg);
    const int fullW = std::max(1, this->remoteDatasetWidth);
    const int fullH = std::max(1, this->remoteDatasetHeight);
    const int fullD = std::max(1, this->remoteDatasetDepth);
    const bool roi = (dorg[0] != 0.0 || dorg[1] != 0.0 || dorg[2] != 0.0);

    const auto toFull = [roi](int dv, double dorg1, int ddim1, int full1) -> int {
        if (roi) return static_cast<int>(std::lround(dorg1)) + dv;   // spacing-1 ROI
        if (ddim1 <= 1 || full1 <= 1) return 0;
        return static_cast<int>(std::lround(double(dv) * double(full1 - 1) / double(ddim1 - 1)));
    };
    const int seedX = std::clamp(toFull(ijk[0], dorg[0], ddim[0], fullW), 0, fullW - 1);
    const int seedY = std::clamp(toFull(ijk[1], dorg[1], ddim[1], fullH), 0, fullH - 1);
    const int seedZ = std::clamp(toFull(ijk[2], dorg[2], ddim[2], fullD), 0, fullD - 1);

    // Snapshot the geometry used for THIS pick so the preview places the mask the
    // same way even if the display swaps while the request is in flight.
    this->m_lassoSnapRoi = roi;
    this->m_lassoSnapOrigin[0] = dorg[0]; this->m_lassoSnapOrigin[1] = dorg[1];
    this->m_lassoSnapOrigin[2] = dorg[2];
    this->m_lassoSnapDim[0] = ddim[0]; this->m_lassoSnapDim[1] = ddim[1];
    this->m_lassoSnapDim[2] = ddim[2];

    qDebug().noquote()
            << QStringLiteral("[lasso] pick display ijk=(%1,%2,%3) → full seed=(%4,%5,%6) "
                              "reach=%7 lambda_v=%8")
                       .arg(ijk[0]).arg(ijk[1]).arg(ijk[2])
                       .arg(seedX).arg(seedY).arg(seedZ)
                       .arg(this->m_lassoReach).arg(this->m_lassoLambdaV);

    // Instant feedback: drop a marker exactly where you clicked so it is obvious
    // the click registered while the grow computes.
    double world[3];
    picker->GetPickPosition(world);
    this->placeLassoSeedMarker(world);

    // ⇧-click ADDs another source (union); ⌥-click CARVEs a region out; a plain
    // click starts a fresh selection.
    const bool shift = iren->GetShiftKey();
    const bool alt = iren->GetAltKey();
    if (shift && this->m_lassoHasSeed) {
        this->m_lassoAddSeeds.push_back({ seedZ, seedY, seedX });
    } else if (alt && this->m_lassoHasSeed) {
        this->m_lassoSubSeeds.push_back({ seedZ, seedY, seedX });
    } else {
        this->m_lassoLastSeed[0] = seedZ;
        this->m_lassoLastSeed[1] = seedY;
        this->m_lassoLastSeed[2] = seedX;
        this->m_lassoAddSeeds.clear();
        this->m_lassoSubSeeds.clear();
        this->m_lassoHasSeed = true;
    }
    this->requestLassoSelection();
}

void vtkWindowCube::requestLassoSelection()
{
    if (!this->m_lassoHasSeed) {
        return;
    }
    if (this->m_lassoSelectWatcher.isRunning()) {
        this->m_lassoPendingRegrow = true;   // honour the latest params once the current grow ends
        return;
    }
    this->m_lassoDiscardResult = false;
    // The mask is about to be recomputed → any in-progress 2-D refinement is against
    // a stale mask; tear it down so its observers don't edit the wrong array.
    if (this->m_lassoEditActive) this->setLassoEditActive(false);
    if (this->m_lassoEditToggle) {
        QSignalBlocker b(this->m_lassoEditToggle);
        this->m_lassoEditToggle->setChecked(false);
    }
    this->setLassoApplyEnabled(false);   // block Apply on a soon-to-be-superseded surface
    // Busy feedback scoped to the 3-D widget (not a global override cursor, which
    // could stick if the window closes mid-grow) + a prominent status.
    if (ui->vtkCube) ui->vtkCube->setCursor(Qt::BusyCursor);
    this->statusBar()->showMessage(u"Kinematic Lasso: growing selection…"_s, 0);

    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    const double reach = this->m_lassoReach, lambdaV = this->m_lassoLambdaV;
    const int roiRadius = this->m_lassoRoiRadius;
    const int seedZ = this->m_lassoLastSeed[0];
    const int seedY = this->m_lassoLastSeed[1];
    const int seedX = this->m_lassoLastSeed[2];
    const QVector<std::array<int, 3>> addSeeds = this->m_lassoAddSeeds;
    const QVector<std::array<int, 3>> subSeeds = this->m_lassoSubSeeds;
    this->m_lassoSelectWatcher.setFuture(QtConcurrent::run(
            &fetchKinematicSelect, url, token, session, dataset,
            seedZ, seedY, seedX, reach, lambdaV, roiRadius, addSeeds, subSeeds));
}

void vtkWindowCube::placeLassoSeedMarker(const double world[3])
{
    auto *renderer = ui->vtkCube && ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!renderer) {
        return;
    }
    double b[6];
    renderer->ComputeVisiblePropBounds(b);
    const double diag = std::sqrt((b[1] - b[0]) * (b[1] - b[0]) + (b[3] - b[2]) * (b[3] - b[2])
                                  + (b[5] - b[4]) * (b[5] - b[4]));
    const double radius = std::max(0.4, 0.012 * (diag > 0 ? diag : 64.0));
    auto sphere = vtkSmartPointer<vtkSphereSource>::New();
    sphere->SetCenter(world[0], world[1], world[2]);
    sphere->SetRadius(radius);
    sphere->SetThetaResolution(16);
    sphere->SetPhiResolution(16);
    sphere->Update();
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputConnection(sphere->GetOutputPort());
    if (!this->m_lassoSeedActor) {
        this->m_lassoSeedActor = vtkSmartPointer<vtkActor>::New();
        renderer->AddActor(this->m_lassoSeedActor);
    }
    this->m_lassoSeedActor->SetMapper(mapper);
    this->m_lassoSeedActor->GetProperty()->SetColor(1.0, 0.9, 0.1);   // yellow seed
    this->m_lassoSeedActor->GetProperty()->LightingOff();
    this->m_lassoSeedActor->PickableOff();
    ui->vtkCube->renderWindow()->Render();
}

void vtkWindowCube::onLassoSelectReady()
{
    if (ui->vtkCube) ui->vtkCube->setCursor(this->m_lassoActive ? Qt::CrossCursor : Qt::ArrowCursor);
    const RemoteKinematicSelectResult r = this->m_lassoSelectWatcher.result();
    this->statusBar()->clearMessage();
    // The selection was cleared while this grow was in flight → drop the result.
    if (this->m_lassoDiscardResult) {
        this->m_lassoDiscardResult = false;
        this->m_lassoPendingRegrow = false;
        return;
    }
    // A slider moved during the grow → honour the newest params after handling this.
    const bool pending = this->m_lassoPendingRegrow;
    this->m_lassoPendingRegrow = false;
    if (!r.valid) {
        this->statusBar()->showMessage(u"Kinematic Lasso failed: %1"_s.arg(r.errorMessage), 4000);
        if (pending) this->requestLassoSelection();
        return;
    }
    if (r.count <= 0) {
        this->statusBar()->showMessage(
                u"Kinematic Lasso: nothing selected — click brighter emission."_s, 3000);
        if (pending) this->requestLassoSelection();
        return;
    }
    this->showLassoHint(false);   // a source is selected → the prompt has done its job

    // Keep the EXACT ROI mask (full-res voxel coords) so Apply sends what's shown
    // and the 2-D channel overlay can slice it per channel.
    this->m_lassoMaskBytes = r.data;
    this->m_lassoMaskOrigin[0] = r.originX;
    this->m_lassoMaskOrigin[1] = r.originY;
    this->m_lassoMaskOrigin[2] = r.originZ;
    this->m_lassoMaskDims[0] = r.roiWidth;
    this->m_lassoMaskDims[1] = r.roiHeight;
    this->m_lassoMaskDims[2] = r.roiDepth;
    this->updateLassoMaskOverlay(true);

    // Store the DISPLAY geometry for the ROI mask so the isosurface can be rebuilt
    // after 2-D edits. ROI display: world == full-res voxels → origin = selection
    // voxel, spacing 1. Preview/full: world == display voxels → origin = selection·s,
    // spacing s = (displayDim-1)/(fullDim-1) (same convention as the kinematic model).
    double sx = 1.0, sy = 1.0, sz = 1.0;
    double ox = r.originX, oy = r.originY, oz = r.originZ;
    if (!this->m_lassoSnapRoi) {
        const auto scale = [](int ddim1, int full1) -> double {
            return (full1 > 1 && ddim1 > 1) ? double(ddim1 - 1) / double(full1 - 1) : 1.0;
        };
        sx = scale(this->m_lassoSnapDim[0], r.fullWidth);
        sy = scale(this->m_lassoSnapDim[1], r.fullHeight);
        sz = scale(this->m_lassoSnapDim[2], r.fullDepth);
        ox = r.originX * sx;  oy = r.originY * sy;  oz = r.originZ * sz;
    }
    this->m_lassoDispOrigin[0] = ox; this->m_lassoDispOrigin[1] = oy; this->m_lassoDispOrigin[2] = oz;
    this->m_lassoDispSpacing[0] = sx; this->m_lassoDispSpacing[1] = sy; this->m_lassoDispSpacing[2] = sz;

    this->rebuildLassoIsosurface(true);
    if (!(ui->vtkCube && ui->vtkCube->renderWindow()
          && ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer())) {
        return;
    }

    this->ensureLassoPanel();
    if (this->m_lassoPanel) {
        this->m_lassoPanel->show();
        this->m_lassoPanel->raise();
    }
    if (this->m_lassoCountLabel) {
        this->m_lassoCountLabel->setText(u"%1 voxels selected"_s.arg(r.count));
    }
    this->statusBar()->showMessage(
            u"Kinematic Lasso: %1 voxels selected — tune reach / velocity coupling to refine."_s
            .arg(r.count), 4000);

    if (pending)
        this->requestLassoSelection();            // a slider moved mid-grow → re-grow now
    else
        this->setLassoApplyEnabled(true);         // the displayed surface is now current
}

void vtkWindowCube::setLassoApplyEnabled(bool on)
{
    if (this->m_lassoIsolateBtn) this->m_lassoIsolateBtn->setEnabled(on);
    if (this->m_lassoRemoveBtn)  this->m_lassoRemoveBtn->setEnabled(on);
    if (this->m_lassoEditToggle) this->m_lassoEditToggle->setEnabled(on);
}

void vtkWindowCube::ensureLassoPanel()
{
    if (this->m_lassoPanel) {
        return;
    }
    auto *dock = new QDockWidget(u"Kinematic Lasso"_s, this);
    dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    auto *body = new QWidget(dock);
    auto *outer = new QVBoxLayout(body);
    outer->setContentsMargins(12, 10, 12, 10);
    outer->setSpacing(8);

    auto *intro = new QLabel(
            u"Refine the selection: <b>Reach</b> grows it into the faint outskirts; "
            u"<b>Velocity coupling</b> follows the source further along velocity "
            u"(the kinematic knob). Double-click a source to select a different one."_s, body);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto *form = new QFormLayout();
    form->setSpacing(6);

    // Reach: slider 1..40 → reach value.
    m_lassoReachSlider = new QSlider(Qt::Horizontal, body);
    m_lassoReachSlider->setRange(1, 40);
    m_lassoReachSlider->setValue(std::clamp(int(std::lround(m_lassoReach)), 1, 40));
    m_lassoReachValue = new QLabel(QString::number(m_lassoReach, 'f', 0), body);
    auto *reachRow = new QWidget(body);
    auto *reachLay = new QHBoxLayout(reachRow);
    reachLay->setContentsMargins(0, 0, 0, 0);
    reachLay->addWidget(m_lassoReachSlider, 1);
    reachLay->addWidget(m_lassoReachValue);
    form->addRow(u"Reach"_s, reachRow);

    // Velocity coupling λ_v: slider 10..500 → λ_v = value/100 (0.1 .. 5.0).
    m_lassoLambdaSlider = new QSlider(Qt::Horizontal, body);
    m_lassoLambdaSlider->setRange(10, 500);
    m_lassoLambdaSlider->setValue(std::clamp(int(std::lround(m_lassoLambdaV * 100.0)), 10, 500));
    m_lassoLambdaValue = new QLabel(QString::number(m_lassoLambdaV, 'f', 2), body);
    auto *lamRow = new QWidget(body);
    auto *lamLay = new QHBoxLayout(lamRow);
    lamLay->setContentsMargins(0, 0, 0, 0);
    lamLay->addWidget(m_lassoLambdaSlider, 1);
    lamLay->addWidget(m_lassoLambdaValue);
    form->addRow(u"Velocity coupling"_s, lamRow);
    outer->addLayout(form);

    m_lassoCountLabel = new QLabel(u"—"_s, body);
    m_lassoCountLabel->setStyleSheet(u"color: palette(mid);"_s);
    outer->addWidget(m_lassoCountLabel);

    auto *hint = new QLabel(
            u"<i>One double-click grabs the whole connected source. "
            u"⇧-double-click adds a separate source, ⌥-double-click carves a region out.</i>"_s, body);
    hint->setWordWrap(true);
    outer->addWidget(hint);

    // Apply: turn the selection into a new masked cube.
    auto *applyRow = new QWidget(body);
    auto *applyLay = new QHBoxLayout(applyRow);
    applyLay->setContentsMargins(0, 0, 0, 0);
    auto *isolateBtn = new QPushButton(u"Isolate → new cube"_s, applyRow);
    isolateBtn->setToolTip(u"Blank everything OUTSIDE the selection."_s);
    auto *removeBtn = new QPushButton(u"Remove → new cube"_s, applyRow);
    removeBtn->setToolTip(u"Blank everything INSIDE the selection."_s);
    applyLay->addWidget(isolateBtn);
    applyLay->addWidget(removeBtn);
    outer->addWidget(applyRow);
    this->m_lassoIsolateBtn = isolateBtn;
    this->m_lassoRemoveBtn = removeBtn;
    // Only meaningful once a selection is on screen; a running grow disables them
    // so Apply can never act on a stale (superseded) isosurface.
    const bool ready = this->m_lassoHasSeed && !this->m_lassoSelectWatcher.isRunning();
    isolateBtn->setEnabled(ready);
    removeBtn->setEnabled(ready);

    // Phase 2 — refine the mask by drawing polygons on the 2-D channel map.
    auto *editToggle = new QPushButton(u"Refine on 2-D map…"_s, body);
    editToggle->setCheckable(true);
    editToggle->setToolTip(u"Draw a polygon on the 2-D channel map to add/subtract "
                           u"selection pixels on the current channel."_s);
    outer->addWidget(editToggle);
    auto *modeRow = new QWidget(body);
    auto *modeLay = new QHBoxLayout(modeRow);
    modeLay->setContentsMargins(0, 0, 0, 0);
    auto *addBtn = new QPushButton(u"Add"_s, modeRow);
    addBtn->setCheckable(true);
    addBtn->setChecked(true);
    auto *subBtn = new QPushButton(u"Subtract"_s, modeRow);
    subBtn->setCheckable(true);
    modeLay->addWidget(addBtn);
    modeLay->addWidget(subBtn);
    outer->addWidget(modeRow);
    modeRow->setEnabled(false);
    editToggle->setEnabled(this->m_lassoHasSeed);
    this->m_lassoEditToggle = editToggle;
    this->m_lassoEditAddBtn = addBtn;
    this->m_lassoEditSubBtn = subBtn;
    QObject::connect(editToggle, &QPushButton::toggled, this, [this, modeRow](bool on) {
        modeRow->setEnabled(on);
        this->setLassoEditActive(on);
    });
    const auto setEditMode = [this](bool add) {
        this->m_lassoEditAdd = add;
        if (this->m_lassoEditAddBtn) this->m_lassoEditAddBtn->setChecked(add);
        if (this->m_lassoEditSubBtn) this->m_lassoEditSubBtn->setChecked(!add);
        if (this->m_lassoEditActive) this->setLassoEditActive(true);   // refresh the status hint
    };
    QObject::connect(addBtn, &QPushButton::clicked, this, [setEditMode]() { setEditMode(true); });
    QObject::connect(subBtn, &QPushButton::clicked, this, [setEditMode]() { setEditMode(false); });

    auto *clearBtn = new QPushButton(u"Clear selection"_s, body);
    outer->addWidget(clearBtn);
    outer->addStretch(1);

    QObject::connect(isolateBtn, &QPushButton::clicked, this,
                     [this]() { this->applyLassoMask(u"blank_outside"_s); });
    QObject::connect(removeBtn, &QPushButton::clicked, this,
                     [this]() { this->applyLassoMask(u"blank_inside"_s); });

    // Live re-grow on slider release (label updates live on move).
    QObject::connect(m_lassoReachSlider, &QSlider::valueChanged, this, [this](int v) {
        this->m_lassoReach = double(v);
        if (this->m_lassoReachValue) this->m_lassoReachValue->setText(QString::number(v));
    });
    QObject::connect(m_lassoReachSlider, &QSlider::sliderReleased, this,
                     [this]() { this->requestLassoSelection(); });
    QObject::connect(m_lassoLambdaSlider, &QSlider::valueChanged, this, [this](int v) {
        this->m_lassoLambdaV = double(v) / 100.0;
        if (this->m_lassoLambdaValue)
            this->m_lassoLambdaValue->setText(QString::number(this->m_lassoLambdaV, 'f', 2));
    });
    QObject::connect(m_lassoLambdaSlider, &QSlider::sliderReleased, this,
                     [this]() { this->requestLassoSelection(); });
    QObject::connect(clearBtn, &QPushButton::clicked, this,
                     [this]() { this->clearKinematicLassoSelection(); });

    dock->setWidget(body);
    this->addDockWidget(Qt::RightDockWidgetArea, dock);
    this->m_lassoPanel = dock;
}

void vtkWindowCube::updateLassoMaskOverlay(bool render)
{
    // Draw the selection's outline on the 2-D channel map for the CURRENT channel,
    // so you can see (and, in phase 2, refine) which pixels are selected slice by
    // slice. Mirrors the kinematic 2-D contour: extract the ROI mask's XY plane for
    // this channel at full-res slice geometry (origin 0, spacing 1), placed at the
    // mask's (ox,oy) offset, contour at 0.5.
    auto *ren = this->sliceWin ? this->sliceWin->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!ren)
        return;
    if (this->m_lassoMask2dActor) {
        ren->RemoveActor(this->m_lassoMask2dActor);
        this->m_lassoMask2dActor = nullptr;
    }
    const int mw = this->m_lassoMaskDims[0], mh = this->m_lassoMaskDims[1], md = this->m_lassoMaskDims[2];
    if (!this->m_lassoMaskBytes.isEmpty() && mw > 0 && mh > 0 && md > 0) {
        const int ox = this->m_lassoMaskOrigin[0], oy = this->m_lassoMaskOrigin[1];
        const int oz = this->m_lassoMaskOrigin[2];
        const int ch = ui->spinSlice ? ui->spinSlice->value() - 1 : 0;   // 1-based UI → full-res z
        const int localZ = ch - oz;
        const qint64 planeCount = qint64(mw) * mh;
        const qint64 need = planeCount * md;
        if (localZ >= 0 && localZ < md
            && this->m_lassoMaskBytes.size() >= need * qint64(sizeof(float))) {
            auto plane = vtkSmartPointer<vtkImageData>::New();
            plane->SetExtent(ox, ox + mw - 1, oy, oy + mh - 1, 0, 0);
            plane->SetOrigin(0., 0., 0.);
            plane->SetSpacing(1., 1., 1.);
            auto ps = vtkSmartPointer<vtkFloatArray>::New();
            ps->SetNumberOfComponents(1);
            ps->SetNumberOfTuples(planeCount);
            const float *src = reinterpret_cast<const float *>(this->m_lassoMaskBytes.constData())
                    + qint64(localZ) * planeCount;
            std::memcpy(ps->GetVoidPointer(0), src,
                        sizeof(float) * static_cast<std::size_t>(planeCount));
            plane->GetPointData()->SetScalars(ps);

            auto contour = vtkSmartPointer<vtkContourFilter>::New();
            contour->SetInputData(plane);
            contour->SetValue(0, 0.5);           // boundary of the binary mask
            contour->Update();

            auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputConnection(contour->GetOutputPort());
            mapper->ScalarVisibilityOff();
            auto actor = vtkSmartPointer<vtkActor>::New();
            actor->SetMapper(mapper);
            actor->GetProperty()->SetColor(0.35, 1.0, 0.55);   // green — matches the 3-D isosurface
            actor->GetProperty()->SetLineWidth(2.0);
            actor->GetProperty()->LightingOff();
            actor->SetPosition(0., 0., 0.5);     // nudge in front of the image slice
            actor->PickableOff();
            ren->AddActor(actor);
            this->m_lassoMask2dActor = actor;
            ren->ResetCameraClippingRange();
        }
    }
    if (render && this->sliceWin)
        this->sliceWin->Render();
}

void vtkWindowCube::rebuildLassoIsosurface(bool render)
{
    auto *renderer = ui->vtkCube && ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!renderer)
        return;
    const int mw = this->m_lassoMaskDims[0], mh = this->m_lassoMaskDims[1], md = this->m_lassoMaskDims[2];
    if (this->m_lassoMaskBytes.isEmpty() || mw <= 0 || mh <= 0 || md <= 0) {
        if (this->m_lassoActor) { renderer->RemoveActor(this->m_lassoActor); this->m_lassoActor = nullptr; }
        this->m_lassoImage = nullptr;
        if (render && ui->vtkCube->renderWindow()) ui->vtkCube->renderWindow()->Render();
        return;
    }
    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetDimensions(mw, mh, md);
    img->SetOrigin(this->m_lassoDispOrigin);
    img->SetSpacing(this->m_lassoDispSpacing);
    auto scalars = vtkSmartPointer<vtkFloatArray>::New();
    scalars->SetNumberOfComponents(1);
    scalars->SetNumberOfTuples(qint64(mw) * mh * md);
    std::memcpy(scalars->GetVoidPointer(0), this->m_lassoMaskBytes.constData(),
                static_cast<std::size_t>(this->m_lassoMaskBytes.size()));
    img->GetPointData()->SetScalars(scalars);

    auto contour = vtkSmartPointer<vtkFlyingEdges3D>::New();
    contour->SetInputData(img);
    contour->SetValue(0, 0.5);          // binary 0/1 mask → surface at 0.5
    contour->ComputeNormalsOn();
    contour->Update();
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputConnection(contour->GetOutputPort());
    mapper->ScalarVisibilityOff();
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.20, 0.95, 0.35);   // green — the selection
    actor->GetProperty()->SetOpacity(0.55);
    actor->PickableOff();
    if (this->m_lassoActor)
        renderer->RemoveActor(this->m_lassoActor);
    this->m_lassoActor = actor;
    this->m_lassoImage = img;
    renderer->AddActor(actor);
    if (render && ui->vtkCube->renderWindow())
        ui->vtkCube->renderWindow()->Render();
}

// ── Phase 2: draw a polygon on the 2-D channel map to add/subtract mask pixels ──
void vtkWindowCube::setLassoEditActive(bool on)
{
    if (on) {
        // Editing is slice-only (the mapping + overlay are the channel map). Refuse
        // if the 2-D view is showing a moment map, so observers never land on the
        // wrong (moment) interactor.
        if (!this->viewingSlice()) {
            this->statusBar()->showMessage(
                    u"Mask edit: switch the 2-D view to the channel map first."_s, 3500);
            if (this->m_lassoEditToggle) {
                QSignalBlocker b(this->m_lassoEditToggle);
                this->m_lassoEditToggle->setChecked(false);
            }
            this->m_lassoEditActive = false;
            return;
        }
        auto *iren = ui->vtkImage && this->active2dWin()
                ? this->active2dWin()->GetInteractor() : nullptr;
        if (!iren) {
            this->m_lassoEditActive = false;
            return;
        }
        this->m_lassoEditActive = true;
        this->m_lassoEditIren = iren;
        // Peer 2-D tools share this interactor's permanent observers; those callbacks
        // early-return while m_lassoEditActive so a click doesn't also toggle the probe
        // or drop a PV / region vertex.
        // Neutralise the default window/level image style while drawing (a tiny
        // press-drag would otherwise change brightness) — set it on THIS interactor
        // directly so the restore later hits the same one regardless of view swaps.
        {
            vtkNew<vtkInteractorStyleUser> userStyle;
            iren->SetInteractorStyle(userStyle);
        }
        if (!this->m_lassoEditClickTag)
            this->m_lassoEditClickTag = iren->AddObserver(
                    vtkCommand::LeftButtonPressEvent, this, &vtkWindowCube::onLassoEditClick, 2.0);
        if (!this->m_lassoEditMoveTag)
            this->m_lassoEditMoveTag = iren->AddObserver(
                    vtkCommand::MouseMoveEvent, this, &vtkWindowCube::onLassoEditMove, 2.0);
        if (!this->m_lassoEditRightTag)
            this->m_lassoEditRightTag = iren->AddObserver(
                    vtkCommand::RightButtonPressEvent, this, &vtkWindowCube::commitLassoEditPolygon, 2.0);
        if (ui->vtkImage) if (auto *w2d = this->active2dWidget()) w2d->setCursor(Qt::CrossCursor);
        this->statusBar()->showMessage(
                u"Mask edit: click points on the channel map; right-click (or click the first point) "
                u"to close and %1."_s.arg(this->m_lassoEditAdd ? u"ADD"_s : u"SUBTRACT"_s), 0);
    } else {
        this->m_lassoEditActive = false;
        // Remove observers from the EXACT interactor we added them to (ui->vtkImage
        // may since have swapped to the moment window).
        auto *iren = this->m_lassoEditIren.GetPointer();
        if (iren) {
            if (this->m_lassoEditClickTag) { iren->RemoveObserver(this->m_lassoEditClickTag); this->m_lassoEditClickTag = 0; }
            if (this->m_lassoEditMoveTag)  { iren->RemoveObserver(this->m_lassoEditMoveTag);  this->m_lassoEditMoveTag = 0; }
            if (this->m_lassoEditRightTag) { iren->RemoveObserver(this->m_lassoEditRightTag); this->m_lassoEditRightTag = 0; }
            // Restore the default window/level style on the SAME interactor, even if
            // the 2-D view has since swapped to the moment window.
            vtkNew<vtkInteractorStyleImage> imgStyle;
            iren->SetInteractorStyle(imgStyle);
        }
        this->m_lassoEditClickTag = this->m_lassoEditMoveTag = this->m_lassoEditRightTag = 0;
        this->m_lassoEditIren = nullptr;
        if (ui->vtkImage) if (auto *w2d = this->active2dWidget()) w2d->setCursor(Qt::ArrowCursor);
        this->m_lassoEditVerts.clear();
        this->refreshLassoEditPreview();
        this->statusBar()->clearMessage();
    }
}

bool vtkWindowCube::mapDisplayToFullVoxel(int dx, int dy, int &outX, int &outY) const
{
    auto *renderer = ui->vtkImage && this->active2dWin()
            ? this->active2dWin()->GetRenderers()->GetFirstRenderer() : nullptr;
    auto *imageData = vtkImageData::SafeDownCast(
            this->remoteSliceDisplaySource->GetOutputDataObject(0));
    if (!renderer || !imageData)
        return false;
    this->coordinate->SetValue(dx, dy);
    const double *w = this->coordinate->GetComputedWorldValue(renderer);
    if (!w)
        return false;
    int extent[6];
    double origin[3], spacing[3];
    imageData->GetExtent(extent);
    imageData->GetOrigin(origin);
    imageData->GetSpacing(spacing);
    outX = int(std::lround(extent[0] + (w[0] - origin[0]) / spacing[0]));
    outY = int(std::lround(extent[2] + (w[1] - origin[1]) / spacing[1]));   // ext[2], not ext[1]
    return true;
}

void vtkWindowCube::onLassoEditClick()
{
    if (!this->m_lassoEditActive)
        return;
    auto *iren = ui->vtkImage && this->active2dWin()
            ? this->active2dWin()->GetInteractor() : nullptr;
    if (!iren)
        return;
    int pos[2];
    iren->GetEventPosition(pos);
    int vx = 0, vy = 0;
    if (!this->mapDisplayToFullVoxel(pos[0], pos[1], vx, vy))
        return;
    // Click near the first point → close and commit.
    if (this->m_lassoEditVerts.size() >= 3) {
        const std::array<int, 2> &f = this->m_lassoEditVerts.front();
        if (std::abs(vx - f[0]) <= 3 && std::abs(vy - f[1]) <= 3) {
            this->commitLassoEditPolygon();
            return;
        }
    }
    this->m_lassoEditVerts.push_back({ vx, vy });
    this->refreshLassoEditPreview();
}

void vtkWindowCube::onLassoEditMove()
{
    if (!this->m_lassoEditActive || this->m_lassoEditVerts.empty())
        return;
    auto *iren = ui->vtkImage && this->active2dWin()
            ? this->active2dWin()->GetInteractor() : nullptr;
    if (!iren)
        return;
    int pos[2];
    iren->GetEventPosition(pos);
    int vx = 0, vy = 0;
    if (!this->mapDisplayToFullVoxel(pos[0], pos[1], vx, vy))
        return;
    // Rubber-band: draw the committed points plus a segment to the live cursor.
    std::vector<std::array<int, 2>> preview = this->m_lassoEditVerts;
    preview.push_back({ vx, vy });
    auto *ren = this->sliceWin ? this->sliceWin->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!ren)
        return;
    if (this->m_lassoEditPolyActor) {
        ren->RemoveActor(this->m_lassoEditPolyActor);
        this->m_lassoEditPolyActor = nullptr;
    }
    auto pts = vtkSmartPointer<vtkPoints>::New();
    for (const auto &p : preview)
        pts->InsertNextPoint(double(p[0]), double(p[1]), 0.6);
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    lines->InsertNextCell(vtkIdType(preview.size()));
    for (vtkIdType i = 0; i < vtkIdType(preview.size()); ++i)
        lines->InsertCellPoint(i);
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    pd->SetPoints(pts);
    pd->SetLines(lines);
    auto mp = vtkSmartPointer<vtkPolyDataMapper>::New();
    mp->SetInputData(pd);
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mp);
    actor->GetProperty()->SetColor(this->m_lassoEditAdd ? 1.0 : 1.0,
                                   this->m_lassoEditAdd ? 0.85 : 0.4,
                                   this->m_lassoEditAdd ? 0.1 : 0.4);  // yellow=add, red=subtract
    actor->GetProperty()->SetLineWidth(1.5);
    actor->GetProperty()->LightingOff();
    actor->PickableOff();
    ren->AddActor(actor);
    this->m_lassoEditPolyActor = actor;
    if (this->sliceWin) this->sliceWin->Render();
}

void vtkWindowCube::refreshLassoEditPreview()
{
    auto *ren = this->sliceWin ? this->sliceWin->GetRenderers()->GetFirstRenderer() : nullptr;
    if (!ren)
        return;
    if (this->m_lassoEditPolyActor) {
        ren->RemoveActor(this->m_lassoEditPolyActor);
        this->m_lassoEditPolyActor = nullptr;
    }
    if (this->m_lassoEditVerts.size() >= 2) {
        auto pts = vtkSmartPointer<vtkPoints>::New();
        for (const auto &p : this->m_lassoEditVerts)
            pts->InsertNextPoint(double(p[0]), double(p[1]), 0.6);
        auto lines = vtkSmartPointer<vtkCellArray>::New();
        lines->InsertNextCell(vtkIdType(this->m_lassoEditVerts.size()));
        for (vtkIdType i = 0; i < vtkIdType(this->m_lassoEditVerts.size()); ++i)
            lines->InsertCellPoint(i);
        auto pd = vtkSmartPointer<vtkPolyData>::New();
        pd->SetPoints(pts);
        pd->SetLines(lines);
        auto mp = vtkSmartPointer<vtkPolyDataMapper>::New();
        mp->SetInputData(pd);
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mp);
        actor->GetProperty()->SetColor(this->m_lassoEditAdd ? 1.0 : 1.0,
                                       this->m_lassoEditAdd ? 0.85 : 0.4,
                                       this->m_lassoEditAdd ? 0.1 : 0.4);
        actor->GetProperty()->SetLineWidth(1.5);
        actor->GetProperty()->LightingOff();
        actor->PickableOff();
        ren->AddActor(actor);
        this->m_lassoEditPolyActor = actor;
    }
    if (this->sliceWin) this->sliceWin->Render();
}

void vtkWindowCube::commitLassoEditPolygon()
{
    if (!this->m_lassoEditActive)
        return;
    if (this->m_lassoEditVerts.size() < 3) {
        this->m_lassoEditVerts.clear();
        this->refreshLassoEditPreview();
        this->statusBar()->showMessage(u"Mask edit: need at least 3 points to close a polygon."_s, 2000);
        return;
    }
    const int mw = this->m_lassoMaskDims[0], mh = this->m_lassoMaskDims[1], md = this->m_lassoMaskDims[2];
    const int ox = this->m_lassoMaskOrigin[0], oy = this->m_lassoMaskOrigin[1], oz = this->m_lassoMaskOrigin[2];
    const int ch = ui->spinSlice ? ui->spinSlice->value() - 1 : 0;   // 1-based UI → full-res z
    const int lz = ch - oz;
    if (this->m_lassoMaskBytes.isEmpty() || mw <= 0 || mh <= 0 || md <= 0 || lz < 0 || lz >= md) {
        this->statusBar()->showMessage(
                u"Mask edit: this channel is outside the current selection extent (extend not yet supported)."_s,
                3500);
        this->m_lassoEditVerts.clear();
        this->refreshLassoEditPreview();
        return;
    }
    // Rasterize the polygon over the ROI-clamped bounding box at this channel.
    int px0 = std::numeric_limits<int>::max(), px1 = std::numeric_limits<int>::min();
    int py0 = std::numeric_limits<int>::max(), py1 = std::numeric_limits<int>::min();
    for (const auto &v : this->m_lassoEditVerts) {
        px0 = std::min(px0, v[0]); px1 = std::max(px1, v[0]);
        py0 = std::min(py0, v[1]); py1 = std::max(py1, v[1]);
    }
    px0 = std::max(px0, ox); px1 = std::min(px1, ox + mw - 1);
    py0 = std::max(py0, oy); py1 = std::min(py1, oy + mh - 1);
    float *mask = reinterpret_cast<float *>(this->m_lassoMaskBytes.data());
    const float val = this->m_lassoEditAdd ? 1.0f : 0.0f;
    qint64 changed = 0;
    for (int y = py0; y <= py1; ++y) {
        for (int x = px0; x <= px1; ++x) {
            if (pointInPolygon(x, y, this->m_lassoEditVerts)) {
                const qint64 idx = (qint64(lz) * mh + (y - oy)) * mw + (x - ox);
                if (mask[idx] != val) { mask[idx] = val; ++changed; }
            }
        }
    }
    this->m_lassoEditVerts.clear();
    this->refreshLassoEditPreview();
    if (changed > 0) {
        this->rebuildLassoIsosurface(true);
        this->updateLassoMaskOverlay(true);
    }
    this->statusBar()->showMessage(
            u"Mask edit: %1 %2 voxels on channel %3."_s
                    .arg(this->m_lassoEditAdd ? u"added"_s : u"removed"_s)
                    .arg(changed).arg(ch + 1), 3000);
}

void vtkWindowCube::clearKinematicLassoSelection()
{
    // If a grow is in flight, discard its result so it doesn't re-add a selection
    // the user just cleared.
    if (this->m_lassoSelectWatcher.isRunning()) {
        this->m_lassoDiscardResult = true;
    }
    this->m_lassoPendingRegrow = false;
    auto *renderer = ui->vtkCube && ui->vtkCube->renderWindow()
            ? ui->vtkCube->renderWindow()->GetRenderers()->GetFirstRenderer() : nullptr;
    if (renderer) {
        if (this->m_lassoActor)     renderer->RemoveActor(this->m_lassoActor);
        if (this->m_lassoSeedActor) renderer->RemoveActor(this->m_lassoSeedActor);
        if (ui->vtkCube->renderWindow()) ui->vtkCube->renderWindow()->Render();
    }
    this->m_lassoActor = nullptr;
    this->m_lassoImage = nullptr;
    this->m_lassoSeedActor = nullptr;
    this->m_lassoHasSeed = false;
    this->m_lassoAddSeeds.clear();
    this->m_lassoSubSeeds.clear();
    this->m_lassoMaskBytes.clear();
    if (this->m_lassoEditActive) this->setLassoEditActive(false);
    if (this->m_lassoEditToggle) {
        QSignalBlocker blk(this->m_lassoEditToggle);
        this->m_lassoEditToggle->setChecked(false);
    }
    this->updateLassoMaskOverlay(true);     // remove the 2-D outline
    this->setLassoApplyEnabled(false);
    if (this->m_lassoCountLabel) this->m_lassoCountLabel->setText(u"—"_s);
    // Back to the start: re-arm the call-to-action so it's obvious a click is needed.
    if (this->m_lassoActive) this->showLassoHint(true);
}

void vtkWindowCube::applyLassoMask(const QString &mode)
{
    if (!this->m_lassoHasSeed) {
        this->statusBar()->showMessage(u"Kinematic Lasso: click a source first."_s, 3000);
        return;
    }
    // Never apply while a grow is still computing (or a re-grow is queued): the
    // displayed isosurface and the seed/slider state could be out of sync.
    if (this->m_lassoSelectWatcher.isRunning() || this->m_lassoPendingRegrow) {
        this->statusBar()->showMessage(
                u"Kinematic Lasso: wait for the selection to finish before applying."_s, 3000);
        return;
    }
    if (this->m_lassoMaskBytes.isEmpty()) {
        this->statusBar()->showMessage(u"Kinematic Lasso: no selection to apply."_s, 3000);
        return;
    }
    const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
    const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
    // Apply the EXACT mask currently shown (bytes + full-res origin/dims), not a
    // re-grow — so a hand-refined mask (phase 2) will apply verbatim too.
    const QByteArray maskBytes = this->m_lassoMaskBytes;
    const int ox = this->m_lassoMaskOrigin[0], oy = this->m_lassoMaskOrigin[1],
              oz = this->m_lassoMaskOrigin[2];
    const int mw = this->m_lassoMaskDims[0], mh = this->m_lassoMaskDims[1],
              md = this->m_lassoMaskDims[2];
    this->statusBar()->showMessage(u"Kinematic Lasso: applying mask…"_s, 0);

    auto *watcher = new QFutureWatcher<BackendSaveSubregionResult>(this);
    QPointer<vtkWindowCube> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSaveSubregionResult>::finished, this,
                     [self, watcher, mode]() {
                         const auto r = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Kinematic Lasso"_s,
                                     u"Masking failed:\n\n%1"_s.arg(r.error));
                             return;
                         }
                         emit self->workspaceExportsChanged();
                         const QString what = (mode == u"blank_inside"_s) ? u"removed"_s : u"isolated"_s;
                         QMessageBox::information(self, u"Kinematic Lasso"_s,
                             u"Selection %1 → new masked cube '%2' in the Workspace Exports "
                             u"(%3 × %4 × %5).\n\nOpen it from the Data Hub to analyse just the "
                             u"selection (moment map, profile, …)."_s
                             .arg(what, r.newDatasetId).arg(r.width).arg(r.height).arg(r.depth));
                     });
    watcher->setFuture(QtConcurrent::run(
            [url, token, session, dataset, maskBytes, ox, oy, oz, mw, mh, md, mode]() {
                BackendClient c(url, token);
                c.setSessionId(session);
                return c.applyCubeMask(dataset, maskBytes, ox, oy, oz, mw, mh, md, mode);
            }));
}

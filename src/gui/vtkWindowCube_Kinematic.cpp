// Kinematic-model overlay (F2) — partial class of vtkWindowCube.
//
// Prompts for a compact tilted-ring parameter set, asks the backend to build a
// model PPV cube of a regularly rotating disk on the data cube's own grid
// (POST /v1/cube/kinematic_model_bin), then contours that model and overlays it
// on the cube renderer. Gas in the data that the model does not cover, or
// covers at a different velocity, is the anomalous / extra-planar gas this is
// meant to reveal (SlicerAstro / van der Hulst).
//
// Two-part UX:
//   * The modal dialog collects only the *model* parameters (geometry +
//     kinematics) — changing those requires a re-fetch.
//   * Once an overlay exists, a live "Kinematic Model" dock panel restyles it
//     (contour level, colour, opacity, wireframe/surface, visibility, remove)
//     with NO re-fetch: the model cube + its FlyingEdges filter are kept
//     client-side, so the level is just a SetValue and the rest are property
//     mutations.
//
// Threading: the network fetch, the float→vtkImageData copy and the initial
// FlyingEdges contour all run on a QtConcurrent worker that captures only plain
// values (never `this`); a QFutureWatcher member delivers the result on the GUI
// thread. Closing the window during a build is safe.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "AnalysisParamStore.h"
#include "app/BackendClient.h"

#include <vtkActor.h>
#include <vtkContourFilter.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkTrivialProducer.h>

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QtConcurrentRun>
#include <QWidget>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

namespace {
constexpr double kInitialFraction = 0.5;   // initial contour level (× model peak)
constexpr double kInitialOpacity = 0.8;

QDoubleSpinBox *makeSpin(double lo, double hi, double val, int decimals,
                         double step, const QString &suffix = QString())
{
    auto *s = new QDoubleSpinBox();
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setValue(val);
    if (!suffix.isEmpty())
        s->setSuffix(suffix);
    s->setMinimumWidth(140);
    return s;
}

// Worker (runs on a QtConcurrent thread): fetch the model and build the overlay
// actor entirely off the GUI thread. Captures only plain copies. `nx/ny/nz` are
// the data cube's FULL grid (the model is built there). The overlay is mapped
// onto the live display geometry (`ddx/ddy/ddz` dims, `dox/doy/doz` origin), a
// GUI-thread snapshot:
//   * display origin = 0 (downsampled preview OR full-res, origin 0 / unit
//     spacing): scale so the model spans the displayed extent —
//     spacing = (dispDim-1)/(fullDim-1) (→ 1 at full res).
//   * display origin ≠ 0 (ROI sub-volume at its global voxel offset, unit
//     spacing): keep the model in global voxel coords (origin 0, spacing 1).
// Appearance is left at defaults here (cyan, wireframe, kInitialOpacity); the
// live panel takes over once the overlay is shown.
RemoteKinematicModelResult fetchKinematicModelWorker(
        QString url, QString token, QString session, QString datasetId,
        int nx, int ny, int nz, int ddx, int ddy, int ddz,
        double dox, double doy, double doz, double x0, double y0, double pa, double incl,
        double vsys, double vrot, double rmax, double sigma)
{
    RemoteKinematicModelResult out;
    out.fraction = kInitialFraction;

    BackendClient client(url, token);
    client.setSessionId(session);
    const BackendKinematicModelResult m = client.requestKinematicModel(
            datasetId, x0, y0, pa, incl, vsys, vrot, rmax, sigma);
    if (!m.valid) {
        out.errorMessage = m.error;
        return out;
    }
    out.peak = m.peak;
    out.spectralMode = m.spectralMode;
    out.spectralUnit = m.spectralAxisUnit;

    if (m.width != nx || m.height != ny || m.depth != nz) {
        out.errorMessage =
                u"Model grid %1×%2×%3 does not match the cube %4×%5×%6."_s
                        .arg(m.width).arg(m.height).arg(m.depth).arg(nx).arg(ny).arg(nz);
        return out;
    }
    const qint64 expected = qint64(m.width) * m.height * m.depth * qint64(sizeof(float));
    if (m.data.size() != expected) {
        out.errorMessage = u"Unexpected model payload size."_s;
        return out;
    }
    if (m.peak <= 0.0) {
        out.empty = true;   // valid request, but no emission landed on a channel
        out.valid = true;
        return out;
    }

    const bool roi = (dox != 0.0 || doy != 0.0 || doz != 0.0);
    double sx = 1.0, sy = 1.0, sz = 1.0;
    if (!roi) {
        sx = (nx > 1) ? double(ddx - 1) / double(nx - 1) : 1.0;
        sy = (ny > 1) ? double(ddy - 1) / double(ny - 1) : 1.0;
        sz = (nz > 1) ? double(ddz - 1) / double(nz - 1) : 1.0;
    }

    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetDimensions(m.width, m.height, m.depth);
    img->SetOrigin(0., 0., 0.);
    img->SetSpacing(sx, sy, sz);
    auto scalars = vtkSmartPointer<vtkFloatArray>::New();
    scalars->SetNumberOfComponents(1);
    scalars->SetNumberOfTuples(qint64(m.width) * m.height * m.depth);
    std::memcpy(scalars->GetVoidPointer(0), m.data.constData(),
                static_cast<std::size_t>(m.data.size()));
    img->GetPointData()->SetScalars(scalars);

    auto contour = vtkSmartPointer<vtkFlyingEdges3D>::New();
    contour->SetInputData(img);
    contour->SetValue(0, kInitialFraction * m.peak);
    contour->ComputeNormalsOn();
    contour->Update();

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputConnection(contour->GetOutputPort());
    mapper->ScalarVisibilityOff();
    mapper->Update();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    auto *prop = actor->GetProperty();
    prop->SetColor(0.25, 0.85, 1.0);             // cyan — distinct from data
    prop->SetOpacity(kInitialOpacity);
    prop->SetRepresentationToWireframe();         // default: see data through
    prop->SetLineWidth(1.5);
    prop->LightingOff();
    actor->PickableOff();

    out.actor = actor;
    out.image = img;
    out.contour = contour;
    out.numVertices = contour->GetOutput() ? contour->GetOutput()->GetNumberOfPoints() : 0;
    out.valid = true;
    return out;
}
}  // namespace

void vtkWindowCube::clearKinematicModelOverlay()
{
    if (this->m_kinematicModelActor) {
        if (auto *rw = ui->vtkCube->renderWindow()) {
            if (auto *ren = rw->GetRenderers()->GetFirstRenderer())
                ren->RemoveActor(this->m_kinematicModelActor);
            rw->Render();
        }
    }
    this->m_kinematicModelActor = nullptr;
    this->m_kinematicModelImage = nullptr;
    this->m_kinematicContour = nullptr;
    this->m_kinematicModelPeak = 0.0;
    // Drop the 2-D channel-map contour too (image now null → removed).
    this->updateKinematic2dContour();
    if (this->m_kinematicPanel)
        this->m_kinematicPanel->hide();
}

void vtkWindowCube::showKinematicModelDialog()
{
    const int nx = this->remoteDatasetWidth;
    const int ny = this->remoteDatasetHeight;
    const int nz = this->remoteDatasetDepth;
    if (nx <= 0 || ny <= 0 || nz <= 0) {
        QMessageBox::warning(this, u"Kinematic Model"_s,
                             u"Cube geometry is not available yet."_s);
        return;
    }
    if (this->m_kinematicWatcher.isRunning()) {
        this->statusBar()->showMessage(u"A kinematic model is already being built…"_s, 4000);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(u"Tilted-Ring Kinematic Model"_s);
    auto *form = new QFormLayout(&dlg);

    auto *intro = new QLabel(
            u"Build a regularly-rotating-disk model on the cube. Velocity fields "
            u"(Vsys / Vrot / σ) are in the cube's spectral unit — km/s once the "
            u"axis converts. Appearance (level, colour, opacity, wireframe) is "
            u"tuned live afterwards in the Kinematic Model panel."_s);
    intro->setWordWrap(true);
    form->addRow(intro);

    auto *spinX0   = makeSpin(0.0, nx, nx / 2.0, 1, 1.0, u" px"_s);
    auto *spinY0   = makeSpin(0.0, ny, ny / 2.0, 1, 1.0, u" px"_s);
    auto *spinPa   = makeSpin(0.0, 360.0, 0.0, 1, 1.0, u" °"_s);
    auto *spinIncl = makeSpin(0.0, 90.0, 45.0, 1, 1.0, u" °"_s);
    auto *spinRmax = makeSpin(1.0, std::max(nx, ny), std::min(nx, ny) / 4.0, 1, 1.0, u" px"_s);
    auto *spinVsys = makeSpin(-1.0e12, 1.0e12, 0.0, 3, 1.0);
    auto *spinVrot = makeSpin(-1.0e9, 1.0e9, 100.0, 3, 1.0);
    auto *spinSig  = makeSpin(1.0e-3, 1.0e9, 10.0, 3, 1.0);

    form->addRow(u"Centre X (x0)"_s, spinX0);
    form->addRow(u"Centre Y (y0)"_s, spinY0);
    form->addRow(u"Position angle (PA)"_s, spinPa);
    form->addRow(u"Inclination"_s, spinIncl);
    form->addRow(u"Disk radius (rmax)"_s, spinRmax);
    form->addRow(u"Systemic velocity (Vsys)"_s, spinVsys);
    form->addRow(u"Rotation velocity (Vrot)"_s, spinVrot);
    form->addRow(u"Velocity dispersion (σ)"_s, spinSig);

    // Pre-fill from the parameters last used for THIS dataset (if any) so the
    // user doesn't re-enter the disk geometry every session. setValue clamps to
    // each spin's range automatically.
    {
        const QVariantMap saved = AnalysisParamStore().load(this->filepath, u"kinematic"_s);
        if (!saved.isEmpty()) {
            if (saved.contains(u"x0"_s))    spinX0->setValue(saved.value(u"x0"_s).toDouble());
            if (saved.contains(u"y0"_s))    spinY0->setValue(saved.value(u"y0"_s).toDouble());
            if (saved.contains(u"pa"_s))    spinPa->setValue(saved.value(u"pa"_s).toDouble());
            if (saved.contains(u"incl"_s))  spinIncl->setValue(saved.value(u"incl"_s).toDouble());
            if (saved.contains(u"rmax"_s))  spinRmax->setValue(saved.value(u"rmax"_s).toDouble());
            if (saved.contains(u"vsys"_s))  spinVsys->setValue(saved.value(u"vsys"_s).toDouble());
            if (saved.contains(u"vrot"_s))  spinVrot->setValue(saved.value(u"vrot"_s).toDouble());
            if (saved.contains(u"sigma"_s)) spinSig->setValue(saved.value(u"sigma"_s).toDouble());
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Overlay"_s);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // Remember these parameters for this dataset so the dialog re-opens pre-filled.
    AnalysisParamStore().save(this->filepath, u"kinematic"_s, {
            { u"x0"_s, spinX0->value() },   { u"y0"_s, spinY0->value() },
            { u"pa"_s, spinPa->value() },   { u"incl"_s, spinIncl->value() },
            { u"rmax"_s, spinRmax->value() }, { u"vsys"_s, spinVsys->value() },
            { u"vrot"_s, spinVrot->value() }, { u"sigma"_s, spinSig->value() } });

    // Snapshot the live display geometry (preview / full-res / ROI) so the
    // worker can register the full-grid model onto what's on screen.
    int ddim[3] = { nx, ny, nz };
    double dorg[3] = { 0., 0., 0. };
    if (auto *obj = this->cubeDisplaySource->GetOutputDataObject(0)) {
        if (auto *im = vtkImageData::SafeDownCast(obj)) {
            im->GetDimensions(ddim);
            im->GetOrigin(dorg);
        }
    }
    this->m_kinematicLaunchDim = { ddim[0], ddim[1], ddim[2] };
    this->m_kinematicLaunchOrigin = { dorg[0], dorg[1], dorg[2] };

    if (this->m_actKinematicModel)
        this->m_actKinematicModel->setEnabled(false);
    this->statusBar()->showMessage(u"Building tilted-ring kinematic model…"_s);
    this->m_kinematicWatcher.setFuture(QtConcurrent::run(
            &fetchKinematicModelWorker, this->remoteBackendUrl, this->remoteBackendToken,
            this->remoteSessionId, this->remoteDatasetId, nx, ny, nz,
            ddim[0], ddim[1], ddim[2], dorg[0], dorg[1], dorg[2],
            spinX0->value(), spinY0->value(), spinPa->value(), spinIncl->value(),
            spinVsys->value(), spinVrot->value(), spinRmax->value(), spinSig->value()));
}

void vtkWindowCube::onKinematicModelReady()
{
    if (this->m_actKinematicModel)
        this->m_actKinematicModel->setEnabled(true);

    const RemoteKinematicModelResult result = this->m_kinematicWatcher.result();
    this->statusBar()->clearMessage();

    if (!result.valid) {
        QMessageBox::warning(this, u"Kinematic Model"_s,
                             u"Model build failed: %1"_s.arg(result.errorMessage));
        return;
    }
    if (result.empty) {
        QMessageBox::information(
                this, u"Kinematic Model"_s,
                u"The model is empty (no emission landed on a channel).\n\n"
                u"Spectral mode: %1 (unit: %2). Set Vsys/Vrot/σ in that unit — "
                u"for a velocity cube use km/s within the cube's velocity range."_s
                        .arg(result.spectralMode.isEmpty() ? u"?"_s : result.spectralMode)
                        .arg(result.spectralUnit.isEmpty() ? u"?"_s : result.spectralUnit));
        return;
    }
    if (!result.actor) {
        QMessageBox::warning(this, u"Kinematic Model"_s, u"Model produced no surface."_s);
        return;
    }

    // Reject the actor if the display was reloaded while the worker ran — it was
    // scaled for the old geometry and would misregister against the new one.
    int curDim[3] = { 0, 0, 0 };
    double curOrg[3] = { 0., 0., 0. };
    if (auto *obj = this->cubeDisplaySource->GetOutputDataObject(0)) {
        if (auto *im = vtkImageData::SafeDownCast(obj)) {
            im->GetDimensions(curDim);
            im->GetOrigin(curOrg);
        }
    }
    const bool geomChanged =
            curDim[0] != m_kinematicLaunchDim[0] || curDim[1] != m_kinematicLaunchDim[1]
            || curDim[2] != m_kinematicLaunchDim[2] || curOrg[0] != m_kinematicLaunchOrigin[0]
            || curOrg[1] != m_kinematicLaunchOrigin[1] || curOrg[2] != m_kinematicLaunchOrigin[2];
    if (geomChanged) {
        this->statusBar()->showMessage(
                u"Cube display changed while building — re-run the kinematic overlay."_s, 6000);
        return;
    }

    // Replace any previous overlay so re-running never stacks surfaces, then keep
    // the model cube + contour for live restyling from the panel.
    this->clearKinematicModelOverlay();
    this->m_kinematicModelActor = result.actor;
    this->m_kinematicModelImage = result.image;
    this->m_kinematicContour = result.contour;
    this->m_kinematicModelPeak = result.peak;
    if (auto *rw = ui->vtkCube->renderWindow()) {
        if (auto *ren = rw->GetRenderers()->GetFirstRenderer())
            ren->AddActor(result.actor);
        rw->Render();
    }

    this->ensureKinematicPanel();
    // Sync the (reused) panel widgets to the new overlay's defaults without
    // firing their slots (the worker already built with these values).
    if (this->m_kinLevelSlider) {
        QSignalBlocker b(this->m_kinLevelSlider);
        this->m_kinLevelSlider->setValue(int(result.fraction * 100));
    }
    if (this->m_kinOpacitySlider) {
        QSignalBlocker b(this->m_kinOpacitySlider);
        this->m_kinOpacitySlider->setValue(int(0.8 * 100));
    }
    if (this->m_kinRepCombo) {
        QSignalBlocker b(this->m_kinRepCombo);
        this->m_kinRepCombo->setCurrentIndex(0);   // wireframe
    }
    if (this->m_kinVisibleCheck) {
        QSignalBlocker b(this->m_kinVisibleCheck);
        this->m_kinVisibleCheck->setChecked(true);
    }
    if (this->m_kinematicPanel) {
        this->m_kinematicPanel->show();
        this->m_kinematicPanel->raise();
    }
    // Draw the 2-D channel-map contour for the current channel (default-on).
    this->updateKinematic2dContour();

    this->statusBar()->showMessage(
            u"Kinematic model overlaid (%1 vertices, mode %2 [%3]) — restyle it in the panel."_s
                    .arg(result.numVertices)
                    .arg(result.spectralMode.isEmpty() ? u"?"_s : result.spectralMode)
                    .arg(result.spectralUnit.isEmpty() ? u"?"_s : result.spectralUnit),
            8000);
}

void vtkWindowCube::updateKinematic2dContour(bool render)
{
    auto *ren = this->sliceWin->GetRenderers()->GetFirstRenderer();
    if (!ren)
        return;
    if (this->m_kinematic2dContourActor) {
        ren->RemoveActor(this->m_kinematic2dContourActor);
        this->m_kinematic2dContourActor = nullptr;
    }

    const bool show = this->m_kinShow2dCheck && this->m_kinShow2dCheck->isChecked();
    if (show && this->m_kinematicModelImage && this->m_kinematicModelPeak > 0.0) {
        int dims[3] = { 0, 0, 0 };
        this->m_kinematicModelImage->GetDimensions(dims);
        const int nx = dims[0], ny = dims[1], nz = dims[2];
        const int ch = ui->spinSlice ? ui->spinSlice->value() - 1 : 0;   // 1-based UI
        auto *arr = vtkFloatArray::SafeDownCast(
                this->m_kinematicModelImage->GetPointData()->GetScalars());
        if (arr && arr->GetNumberOfComponents() == 1
            && arr->GetNumberOfTuples() >= vtkIdType(nx) * ny * nz
            && ch >= 0 && ch < nz && nx > 0 && ny > 0) {
            // Extract the model's XY plane for this channel at full XY resolution
            // (origin 0, unit spacing) — exactly the 2-D channel map's geometry,
            // regardless of any 3-D display downsampling.
            auto plane = vtkSmartPointer<vtkImageData>::New();
            plane->SetExtent(0, nx - 1, 0, ny - 1, 0, 0);
            plane->SetOrigin(0., 0., 0.);
            plane->SetSpacing(1., 1., 1.);
            auto ps = vtkSmartPointer<vtkFloatArray>::New();
            ps->SetNumberOfComponents(1);
            ps->SetNumberOfTuples(qint64(nx) * ny);
            const float *src = static_cast<const float *>(arr->GetVoidPointer(0))
                    + qint64(ch) * nx * ny;
            std::memcpy(ps->GetVoidPointer(0), src,
                        sizeof(float) * static_cast<std::size_t>(qint64(nx) * ny));
            plane->GetPointData()->SetScalars(ps);

            auto contour = vtkSmartPointer<vtkContourFilter>::New();
            contour->SetInputData(plane);
            const int pct = this->m_kinLevelSlider ? this->m_kinLevelSlider->value() : 50;
            contour->SetValue(0, (pct / 100.0) * this->m_kinematicModelPeak);
            contour->Update();

            auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputConnection(contour->GetOutputPort());
            mapper->ScalarVisibilityOff();
            auto actor = vtkSmartPointer<vtkActor>::New();
            actor->SetMapper(mapper);
            double rgb[3] = { 0.25, 0.85, 1.0 };
            if (this->m_kinematicModelActor)
                this->m_kinematicModelActor->GetProperty()->GetColor(rgb);
            actor->GetProperty()->SetColor(rgb[0], rgb[1], rgb[2]);
            actor->GetProperty()->SetLineWidth(2.0);
            actor->GetProperty()->LightingOff();
            actor->SetPosition(0., 0., 0.5);   // nudge in front of the image slice
            actor->PickableOff();
            ren->AddActor(actor);
            this->m_kinematic2dContourActor = actor;
            // Ensure the nudged-forward contour is inside the clip range on the
            // first draw / after a level change.
            ren->ResetCameraClippingRange();
        }
    }
    if (render)
        this->sliceWin->Render();
}

void vtkWindowCube::applyKinematicRepresentation()
{
    if (!this->m_kinematicModelActor || !this->m_kinRepCombo)
        return;
    auto *prop = this->m_kinematicModelActor->GetProperty();
    if (this->m_kinRepCombo->currentIndex() == 0) {   // Wireframe
        prop->SetRepresentationToWireframe();
        prop->SetLineWidth(1.5);
        prop->LightingOff();
    } else {                                          // Surface
        prop->SetRepresentationToSurface();
        prop->LightingOn();
        prop->SetSpecular(0.2);
    }
    if (auto *rw = ui->vtkCube->renderWindow())
        rw->Render();
}

void vtkWindowCube::ensureKinematicPanel()
{
    if (this->m_kinematicPanel)
        return;   // built once; reused across overlays

    auto *dock = new QDockWidget(u"Kinematic Model"_s, this);
    dock->setObjectName(u"kinematicModelDock"_s);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto *body = new QWidget(dock);
    auto *form = new QFormLayout(body);

    // Contour level (× peak) — re-`SetValue` on the kept FlyingEdges, no refetch.
    auto *level = new QSlider(Qt::Horizontal, body);
    level->setRange(5, 95);            // percent of peak
    level->setValue(int(kInitialFraction * 100));
    form->addRow(u"Contour level (% peak)"_s, level);
    this->m_kinLevelSlider = level;
    QObject::connect(level, &QSlider::valueChanged, this, [this](int pct) {
        if (this->m_kinematicContour && this->m_kinematicModelPeak > 0.0) {
            this->m_kinematicContour->SetValue(0, (pct / 100.0) * this->m_kinematicModelPeak);
            if (auto *rw = ui->vtkCube->renderWindow())
                rw->Render();
        }
        this->updateKinematic2dContour();   // keep the 2-D contour at the same level
    });

    // Opacity.
    auto *opacity = new QSlider(Qt::Horizontal, body);
    opacity->setRange(5, 100);
    opacity->setValue(int(kInitialOpacity * 100));
    form->addRow(u"Opacity (%)"_s, opacity);
    this->m_kinOpacitySlider = opacity;
    QObject::connect(opacity, &QSlider::valueChanged, this, [this](int pct) {
        if (this->m_kinematicModelActor) {
            this->m_kinematicModelActor->GetProperty()->SetOpacity(pct / 100.0);
            if (auto *rw = ui->vtkCube->renderWindow())
                rw->Render();
        }
    });

    // Representation.
    auto *rep = new QComboBox(body);
    rep->addItems({ u"Wireframe (see data through)"_s, u"Surface"_s });
    form->addRow(u"Representation"_s, rep);
    this->m_kinRepCombo = rep;
    QObject::connect(rep, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->applyKinematicRepresentation(); });

    // Colour.
    auto *colorBtn = new QPushButton(u"Colour…"_s, body);
    form->addRow(u"Colour"_s, colorBtn);
    QObject::connect(colorBtn, &QPushButton::clicked, this, [this]() {
        if (!this->m_kinematicModelActor)
            return;
        double rgb[3];
        this->m_kinematicModelActor->GetProperty()->GetColor(rgb);
        const QColor initial = QColor::fromRgbF(rgb[0], rgb[1], rgb[2]);
        const QColor picked = QColorDialog::getColor(initial, this, u"Model colour"_s);
        if (picked.isValid()) {
            this->m_kinematicModelActor->GetProperty()->SetColor(
                    picked.redF(), picked.greenF(), picked.blueF());
            if (auto *rw = ui->vtkCube->renderWindow())
                rw->Render();
            this->updateKinematic2dContour();   // match the 2-D contour colour
        }
    });

    // 2-D channel-map contour (the model-vs-data diagnostic) — on by default.
    auto *show2d = new QCheckBox(u"Contour on 2D channel maps"_s, body);
    show2d->setChecked(true);
    form->addRow(show2d);
    this->m_kinShow2dCheck = show2d;
    QObject::connect(show2d, &QCheckBox::toggled, this,
                     [this](bool) { this->updateKinematic2dContour(); });

    // Visibility.
    auto *visible = new QCheckBox(u"Show overlay"_s, body);
    visible->setChecked(true);
    form->addRow(visible);
    this->m_kinVisibleCheck = visible;
    QObject::connect(visible, &QCheckBox::toggled, this, [this](bool on) {
        if (this->m_kinematicModelActor) {
            this->m_kinematicModelActor->SetVisibility(on);
            if (auto *rw = ui->vtkCube->renderWindow())
                rw->Render();
        }
    });

    // Remove.
    auto *removeBtn = new QPushButton(u"Remove overlay"_s, body);
    form->addRow(removeBtn);
    QObject::connect(removeBtn, &QPushButton::clicked, this,
                     [this]() { this->clearKinematicModelOverlay(); });

    dock->setWidget(body);
    this->addDockWidget(Qt::RightDockWidgetArea, dock);
    // Let the user reopen the panel after closing it (while an overlay lives).
    if (ui->menuView)
        ui->menuView->addAction(dock->toggleViewAction());
    this->m_kinematicPanel = dock;
}

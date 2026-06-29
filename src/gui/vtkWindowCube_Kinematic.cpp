// Kinematic-model overlay (F2) — partial class of vtkWindowCube.
//
// Prompts for a compact tilted-ring parameter set, asks the backend to build a
// model PPV cube of a regularly rotating disk on the data cube's own grid
// (POST /v1/cube/kinematic_model_bin), then contours that model and overlays it
// — semi-transparent — on the cube renderer. Gas in the data that the model
// does not cover, or covers at a different velocity, is the anomalous /
// extra-planar gas this is meant to reveal (SlicerAstro / van der Hulst).
//
// Threading: the network fetch, the float→vtkImageData copy and the FlyingEdges
// contour all run on a worker thread (QtConcurrent), exactly like the cube's
// remote subvolume/isosurface paths. The worker captures plain values (URL,
// token, params) — never `this` — so closing the window (WA_DeleteOnClose)
// during the request cannot dangle: the QFutureWatcher member is destroyed with
// the window and its finished() never fires into a dead `this`.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "app/BackendClient.h"

#include <vtkActor.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges3D.h>
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

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

namespace {
// Labelled QDoubleSpinBox with sensible chrome.
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
// the data cube's FULL grid (the model is built there). The overlay must
// register with whatever the cube is *currently* displaying, so we map the
// full-grid model onto the live display image's geometry (`ddx/ddy/ddz` dims,
// `dox/doy/doz` origin), read on the GUI thread before launch:
//   * display origin = 0 (downsampled preview OR full-res, which both use
//     origin 0 / unit spacing): scale the model so it spans the displayed
//     extent — spacing = (dispDim-1)/(fullDim-1) (→ 1 at full res).
//   * display origin ≠ 0 (ROI sub-volume, placed at its global voxel offset
//     with unit spacing): keep the model in global voxel coords (origin 0,
//     spacing 1) so it aligns with the sub-volume in the shared voxel space.
RemoteKinematicModelResult fetchKinematicModelWorker(
        QString url, QString token, QString session, QString datasetId,
        int nx, int ny, int nz, int ddx, int ddy, int ddz,
        double dox, double doy, double doz, double x0, double y0, double pa, double incl,
        double vsys, double vrot, double rmax, double sigma, double frac)
{
    RemoteKinematicModelResult out;
    out.fraction = frac;

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

    // Register the full-grid model onto the current display geometry (see the
    // function comment for the preview/full-res vs ROI cases).
    const bool roi = (dox != 0.0 || doy != 0.0 || doz != 0.0);
    double sx = 1.0, sy = 1.0, sz = 1.0, ox = 0.0, oy = 0.0, oz = 0.0;
    if (!roi) {
        sx = (nx > 1) ? double(ddx - 1) / double(nx - 1) : 1.0;
        sy = (ny > 1) ? double(ddy - 1) / double(ny - 1) : 1.0;
        sz = (nz > 1) ? double(ddz - 1) / double(nz - 1) : 1.0;
    }

    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetDimensions(m.width, m.height, m.depth);
    img->SetOrigin(ox, oy, oz);
    img->SetSpacing(sx, sy, sz);
    auto scalars = vtkSmartPointer<vtkFloatArray>::New();
    scalars->SetNumberOfComponents(1);
    scalars->SetNumberOfTuples(qint64(m.width) * m.height * m.depth);
    std::memcpy(scalars->GetVoidPointer(0), m.data.constData(),
                static_cast<std::size_t>(m.data.size()));
    img->GetPointData()->SetScalars(scalars);

    auto contour = vtkSmartPointer<vtkFlyingEdges3D>::New();
    contour->SetInputData(img);
    contour->SetValue(0, frac * m.peak);
    contour->ComputeNormalsOn();
    contour->Update();

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputConnection(contour->GetOutputPort());
    mapper->ScalarVisibilityOff();
    mapper->Update();   // build the polydata pipeline off the GUI thread

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.25, 0.85, 1.0);   // cyan — distinct from data
    actor->GetProperty()->SetOpacity(0.45);
    actor->GetProperty()->SetSpecular(0.2);
    actor->PickableOff();

    out.actor = actor;
    out.numVertices = contour->GetOutput() ? contour->GetOutput()->GetNumberOfPoints() : 0;
    out.valid = true;
    return out;
}
}  // namespace

void vtkWindowCube::clearKinematicModelOverlay()
{
    if (!this->m_kinematicModelActor)
        return;
    if (auto *rw = ui->vtkCube->renderWindow()) {
        if (auto *ren = rw->GetRenderers()->GetFirstRenderer())
            ren->RemoveActor(this->m_kinematicModelActor);
        rw->Render();
    }
    this->m_kinematicModelActor = nullptr;
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
            u"Overlay a regularly-rotating-disk model on the cube. Velocity "
            u"fields (Vsys / Vrot / σ) are in the cube's spectral unit — km/s "
            u"once the axis converts. Tune the geometry against the data."_s);
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
    auto *spinFrac = makeSpin(0.05, 0.95, 0.5, 2, 0.05);

    form->addRow(u"Centre X (x0)"_s, spinX0);
    form->addRow(u"Centre Y (y0)"_s, spinY0);
    form->addRow(u"Position angle (PA)"_s, spinPa);
    form->addRow(u"Inclination"_s, spinIncl);
    form->addRow(u"Disk radius (rmax)"_s, spinRmax);
    form->addRow(u"Systemic velocity (Vsys)"_s, spinVsys);
    form->addRow(u"Rotation velocity (Vrot)"_s, spinVrot);
    form->addRow(u"Velocity dispersion (σ)"_s, spinSig);
    form->addRow(u"Contour level (× peak)"_s, spinFrac);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Overlay"_s);
    if (this->m_kinematicModelActor) {
        QPushButton *removeBtn =
                buttons->addButton(u"Remove Overlay"_s, QDialogButtonBox::ResetRole);
        QObject::connect(removeBtn, &QPushButton::clicked, &dlg, [this, &dlg]() {
            this->clearKinematicModelOverlay();
            dlg.reject();
        });
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // Snapshot the live display geometry (preview / full-res / ROI) so the
    // worker can register the full-grid model onto exactly what's on screen.
    int ddim[3] = { nx, ny, nz };
    double dorg[3] = { 0., 0., 0. };
    if (auto *obj = this->cubeDisplaySource->GetOutputDataObject(0)) {
        if (auto *im = vtkImageData::SafeDownCast(obj)) {
            im->GetDimensions(ddim);
            im->GetOrigin(dorg);
        }
    }
    // Remember the geometry the result will be registered against, so the
    // finished handler can reject a stale actor if the display is reloaded
    // (preview→full-res / ROI) while the worker runs.
    this->m_kinematicLaunchDim = { ddim[0], ddim[1], ddim[2] };
    this->m_kinematicLaunchOrigin = { dorg[0], dorg[1], dorg[2] };

    // ── Launch the async build (worker captures only plain copies) ───────────
    if (this->m_actKinematicModel)
        this->m_actKinematicModel->setEnabled(false);
    this->statusBar()->showMessage(u"Building tilted-ring kinematic model…"_s);
    this->m_kinematicWatcher.setFuture(QtConcurrent::run(
            &fetchKinematicModelWorker, this->remoteBackendUrl, this->remoteBackendToken,
            this->remoteSessionId, this->remoteDatasetId, nx, ny, nz,
            ddim[0], ddim[1], ddim[2], dorg[0], dorg[1], dorg[2],
            spinX0->value(), spinY0->value(), spinPa->value(), spinIncl->value(),
            spinVsys->value(), spinVrot->value(), spinRmax->value(), spinSig->value(),
            spinFrac->value()));
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

    // Replace any previous overlay so re-running never stacks surfaces.
    this->clearKinematicModelOverlay();
    this->m_kinematicModelActor = result.actor;
    if (auto *rw = ui->vtkCube->renderWindow()) {
        if (auto *ren = rw->GetRenderers()->GetFirstRenderer())
            ren->AddActor(result.actor);
        rw->Render();
    }

    this->statusBar()->showMessage(
            u"Kinematic model overlaid (%1 vertices, level %2×peak, mode %3 [%4])."_s
                    .arg(result.numVertices).arg(result.fraction, 0, 'g', 2)
                    .arg(result.spectralMode.isEmpty() ? u"?"_s : result.spectralMode)
                    .arg(result.spectralUnit.isEmpty() ? u"?"_s : result.spectralUnit),
            8000);
}

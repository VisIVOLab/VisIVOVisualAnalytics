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

#include "ToolProgressSheet.h"
#include "theme/ToolDialogStyle.h"
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
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QtConcurrentRun>
#include <QTableWidget>
#include <QWidget>
#include <QTimer>
#include <QScreen>
#include <QFrame>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QStatusBar>
#include <QStandardPaths>
#include <QProgressDialog>
#include <QPointer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QFileDialog>
#include <QFile>
#include <QDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using namespace Qt::StringLiterals;

namespace {

/// A float32 width × height payload as a 2-D vtkImageData (null on a size mismatch).
vtkSmartPointer<vtkImageData> fitPlaneImage(const QByteArray &data, int w, int h)
{
    if (w <= 0 || h <= 0
        || data.size() != static_cast<qsizetype>(w) * h * static_cast<qsizetype>(sizeof(float)))
        return nullptr;
    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetExtent(0, w - 1, 0, h - 1, 0, 0);
    img->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(img->GetScalarPointer(), data.constData(),
                static_cast<std::size_t>(w) * h * sizeof(float));
    return img;
}

/// data − model, NaN where either is missing.
QByteArray fitResidualPlane(const QByteArray &data, const QByteArray &model)
{
    if (data.size() != model.size())
        return {};
    QByteArray out(data.size(), Qt::Uninitialized);
    const auto *d = reinterpret_cast<const float *>(data.constData());
    const auto *m = reinterpret_cast<const float *>(model.constData());
    auto *o = reinterpret_cast<float *>(out.data());
    const qsizetype n = data.size() / qsizetype(sizeof(float));
    for (qsizetype k = 0; k < n; ++k)
        o[k] = (std::isfinite(d[k]) && std::isfinite(m[k])) ? d[k] - m[k]
                                                           : std::numeric_limits<float>::quiet_NaN();
    return out;
}
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
        double vsys, double vrot, double rmax, double sigma,
        QVector<std::array<double, 4>> rings)
{
    RemoteKinematicModelResult out;
    out.fraction = kInitialFraction;

    BackendClient client(url, token);
    client.setSessionId(session);
    const BackendKinematicModelResult m = client.requestKinematicModel(
            datasetId, x0, y0, pa, incl, vsys, vrot, rmax, sigma, rings);
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
    this->releaseToolSettings(u"Kinematic Model"_s);
    this->refreshToolSettingsPanel(/*reveal=*/false);
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
    // The settings window stays open across the build, carries its own progress
    // sheet and is ready for the next set of rings — like every other tool.
    if (m_kinematicDialog) {
        this->refreshKinematicFitAvailability();
        m_kinematicDialog->show();
        m_kinematicDialog->raise();
        m_kinematicDialog->activateWindow();
        return;
    }
    auto *dlgPtr = new QDialog(this);
    m_kinematicDialog = dlgPtr;
    dlgPtr->setAttribute(Qt::WA_DeleteOnClose);
    QDialog &dlg = *dlgPtr;
    dlg.setWindowTitle(u"Tilted-Ring Kinematic Model"_s);
    // The parameters scroll; the buttons stay at the bottom. With a fit's summary
    // and seven rings the form is taller than a laptop screen, and a dialog the
    // screen clamps draws its rows on top of each other.
    auto *outer = new QVBoxLayout(&dlg);
    auto *scroll = new QScrollArea(&dlg);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *formHost = new QWidget(scroll);
    auto *form = new QFormLayout(formHost);
    scroll->setWidget(formHost);
    outer->addWidget(scroll, 1);

    auto *intro = new QLabel(
            u"Build a regularly-rotating-disk model on the cube. Velocity fields "
            u"(Vsys / Vrot / σ) are in the cube's spectral unit, km/s once the "
            u"axis converts. Appearance (level, colour, opacity, wireframe) is "
            u"tuned live afterwards in the Kinematic Model panel."_s);
    intro->setWordWrap(true);
    form->addRow(intro);

    // The parameters can be FITTED from the lasso selection instead of typed: the
    // selection is measured on the raw cube (moment maps, flux, widths), its
    // geometry read from moment 0, and a flat and a per-ring disc fitted to its
    // velocity field (POST /v1/cube/kinematic_fit_bin).
    auto *fitBtn = new QPushButton(u"Fit from the lasso selection"_s, &dlg);
    auto *fitInfo = new QLabel(&dlg);
    fitInfo->setWordWrap(true);
    fitInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fitInfo->setStyleSheet(ToolDialogStyle::helperCss());
    this->m_kinFitBtn = fitBtn;
    this->m_kinFitInfo = fitInfo;
    this->refreshKinematicFitAvailability();
    form->addRow(fitBtn);
    form->addRow(fitInfo);

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
            auto *restoredHint = new QLabel(
                    u"↺ Restored from your last session for this cube."_s);
            restoredHint->setStyleSheet(u"color: palette(mid); font-style: italic;"_s);
            form->addRow(restoredHint);
        }
    }

    // ── Per-ring (warp / rotation curve) profile ──────────────────────────────
    // When enabled, PA / inclination / Vrot vary with radius (the 3DBarolo /
    // TiRiFiC picture) via the ring table below; the scalar PA/Incl/Vrot/rmax
    // above are then ignored (x0/y0/Vsys/σ stay shared).
    auto *perRingCheck = new QCheckBox(u"Per-ring profile (warp / rotation curve)"_s);
    form->addRow(perRingCheck);

    auto *ringTable = new QTableWidget(0, 4, &dlg);
    ringTable->setHorizontalHeaderLabels({ u"Radius (px)"_s, u"PA (°)"_s, u"Incl (°)"_s, u"Vrot"_s });
    ringTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ringTable->verticalHeader()->setVisible(false);
    ringTable->setMinimumHeight(140);
    ringTable->setVisible(false);
    form->addRow(ringTable);

    auto *ringButtonsW = new QWidget(&dlg);
    auto *ringButtons = new QHBoxLayout(ringButtonsW);
    ringButtons->setContentsMargins(0, 0, 0, 0);
    auto *addRingBtn = new QPushButton(u"Add ring"_s, ringButtonsW);
    auto *removeRingBtn = new QPushButton(u"Remove selected"_s, ringButtonsW);
    ringButtons->addWidget(addRingBtn);
    ringButtons->addWidget(removeRingBtn);
    ringButtons->addStretch(1);
    ringButtonsW->setVisible(false);
    form->addRow(ringButtonsW);

    const auto addRingRow = [ringTable](double r, double pa, double incl, double vrot) {
        const int row = ringTable->rowCount();
        ringTable->insertRow(row);
        const double vals[4] = { r, pa, incl, vrot };
        for (int c = 0; c < 4; ++c) {
            ringTable->setItem(row, c, new QTableWidgetItem(QString::number(vals[c], 'g', 6)));
        }
    };
    QObject::connect(perRingCheck, &QCheckBox::toggled, &dlg,
                     [=](bool on) {
        ringTable->setVisible(on);
        ringButtonsW->setVisible(on);
        spinPa->setEnabled(!on);
        spinIncl->setEnabled(!on);
        spinVrot->setEnabled(!on);
        spinRmax->setEnabled(!on);
        // Seed two rings from the scalar values on first enable (inner half-Vrot,
        // outer full-Vrot at rmax) so the user has a sensible starting curve.
        if (on && ringTable->rowCount() == 0) {
            const double rmax = spinRmax->value();
            addRingRow(std::max(1.0, rmax / 3.0), spinPa->value(), spinIncl->value(),
                       spinVrot->value() * 0.5);
            addRingRow(rmax, spinPa->value(), spinIncl->value(), spinVrot->value());
        }
    });
    QObject::connect(addRingBtn, &QPushButton::clicked, &dlg, [=]() {
        // New ring at a radius strictly beyond the current outermost (ring radii
        // must be distinct — the backend rejects duplicates).
        double maxR = 0.0;
        for (int row = 0; row < ringTable->rowCount(); ++row) {
            if (auto *it = ringTable->item(row, 0)) {
                maxR = std::max(maxR, it->text().toDouble());
            }
        }
        const double step = std::max(1.0, spinRmax->value() * 0.25);
        const double newR = ringTable->rowCount() == 0 ? spinRmax->value() : maxR + step;
        addRingRow(newR, spinPa->value(), spinIncl->value(), spinVrot->value());
    });
    QObject::connect(removeRingBtn, &QPushButton::clicked, &dlg, [ringTable]() {
        const int row = ringTable->currentRow() >= 0 ? ringTable->currentRow()
                                                      : ringTable->rowCount() - 1;
        if (row >= 0) ringTable->removeRow(row);
    });

    // The ring table, validated: the same rules for Overlay and for the export, so
    // neither turns a mistyped cell into a zero.
    const auto readRings = [ringTable](QString *error) {
        QVector<std::array<double, 4>> out;
        for (int row = 0; row < ringTable->rowCount(); ++row) {
            double vals[4];
            for (int c = 0; c < 4; ++c) {
                auto *it = ringTable->item(row, c);
                bool ok = false;
                vals[c] = it ? it->text().trimmed().toDouble(&ok) : 0.0;
                if (!ok || !std::isfinite(vals[c])) {
                    *error = u"Ring %1 has a non-numeric value, fix the table or disable "
                             u"per-ring mode."_s.arg(row + 1);
                    return QVector<std::array<double, 4>>();
                }
            }
            if (vals[0] <= 0.0) {
                *error = u"Ring %1: radius must be > 0."_s.arg(row + 1);
                return QVector<std::array<double, 4>>();
            }
            if (vals[2] < 0.0 || vals[2] > 90.0) {
                *error = u"Ring %1: inclination must be between 0 and 90°."_s.arg(row + 1);
                return QVector<std::array<double, 4>>();
            }
            out.append({ vals[0], vals[1], vals[2], vals[3] });
        }
        QVector<double> radii;
        for (const auto &ring : out)
            radii.append(ring[0]);
        std::sort(radii.begin(), radii.end());
        if (std::adjacent_find(radii.begin(), radii.end()) != radii.end()) {
            *error = u"Ring radii must be distinct."_s;
            return QVector<std::array<double, 4>>();
        }
        return out;
    };

    // The rings as JSON: what a fit found (or was edited into) travels with the
    // rest of the analysis instead of being retyped.
    auto *saveRingsBtn = new QPushButton(u"Save rings as JSON…"_s, ringButtonsW);
    ringButtons->addWidget(saveRingsBtn);
    QObject::connect(saveRingsBtn, &QPushButton::clicked, &dlg, [=, this]() {
        QString error;
        const auto rings = readRings(&error);
        if (!error.isEmpty() || rings.isEmpty()) {
            QMessageBox::warning(dlgPtr, u"Kinematic Model"_s,
                                 error.isEmpty() ? u"The ring table is empty."_s : error);
            return;
        }
        QJsonArray arr;
        for (const auto &ring : rings)
            arr.append(QJsonObject{ { u"r_pix"_s, ring[0] }, { u"pa_deg"_s, ring[1] },
                                    { u"incl_deg"_s, ring[2] }, { u"vrot"_s, ring[3] } });
        const QJsonObject doc{
                { u"dataset"_s, this->filepath },
                { u"x0"_s, spinX0->value() }, { u"y0"_s, spinY0->value() },
                { u"vsys"_s, spinVsys->value() }, { u"sigma"_s, spinSig->value() },
                { u"velocity_unit"_s, u"km/s"_s }, { u"rings"_s, arr } };
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString path = QFileDialog::getSaveFileName(
                dlgPtr, u"Save rings as JSON"_s,
                QDir(dir).filePath(QFileInfo(this->filepath).completeBaseName() + u"_rings.json"_s),
                u"JSON (*.json)"_s);
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented)) < 0)
            QMessageBox::warning(dlgPtr, u"Kinematic Model"_s,
                                 u"Could not write %1:\n%2"_s.arg(path, f.errorString()));
    });

    QObject::connect(fitBtn, &QPushButton::clicked, &dlg, [=, this]() {
        if (this->m_lassoMaskBytes.isEmpty() || this->m_kinFitRunning)
            return;
        this->m_kinFitRunning = true;
        fitBtn->setEnabled(false);
        // The request's own selection, and its description, for the result.
        const QByteArray mask = this->m_lassoMaskBytes;
        const std::array<int, 3> o = { this->m_lassoMaskOrigin[0], this->m_lassoMaskOrigin[1],
                                       this->m_lassoMaskOrigin[2] };
        const std::array<int, 3> d = { this->m_lassoMaskDims[0], this->m_lassoMaskDims[1],
                                       this->m_lassoMaskDims[2] };
        const QString selection = u"Kinematic Lasso, %1 voxels"_s.arg(
                this->m_lassoCountLabel ? this->m_lassoCountLabel->text().section(u' ', 0, 0)
                                        : u"?"_s);
        QPointer<QProgressDialog> sheet =
                visivo::showToolProgressSheet(dlgPtr, u"Fitting the selection"_s);
        auto *watcher = new QFutureWatcher<BackendKinematicFitResult>(dlgPtr);
        // Closing the dialog mid-fit deletes the watcher with it, and its result
        // is never handled: the request is over either way.
        this->m_kinFitRequest = watcher;
        QObject::connect(watcher, &QObject::destroyed, this, [this, watcher]() {
            if (this->m_kinFitRequest != watcher)
                return;   // an older request, long handled
            this->m_kinFitRequest = nullptr;
            this->m_kinFitRunning = false;
            this->refreshKinematicFitAvailability();
        });
        QObject::connect(watcher, &QFutureWatcher<BackendKinematicFitResult>::finished, dlgPtr,
                         [=, this]() {
            const BackendKinematicFitResult r = watcher->result();
            watcher->deleteLater();
            if (sheet)
                sheet->close();
            if (this->m_kinFitRequest == watcher)
                this->m_kinFitRequest = nullptr;
            this->m_kinFitRunning = false;
            this->refreshKinematicFitAvailability();
            // The selection moved on while this fit ran: its numbers describe a
            // selection no longer on screen, so they are not published as this one's.
            if (this->m_lassoMaskBytes != mask
                || this->m_lassoMaskOrigin[0] != o[0] || this->m_lassoMaskOrigin[1] != o[1]
                || this->m_lassoMaskOrigin[2] != o[2] || this->m_lassoMaskDims[0] != d[0]
                || this->m_lassoMaskDims[1] != d[1] || this->m_lassoMaskDims[2] != d[2]) {
                QMessageBox::information(dlgPtr, u"Kinematic Model"_s,
                        u"The selection changed while it was being fitted. Fit it again."_s);
                return;
            }
            if (!r.valid) {
                QMessageBox::warning(dlgPtr, u"Kinematic Model"_s,
                                     u"The fit failed:\n\n%1"_s.arg(r.error));
                return;
            }
            spinX0->setValue(r.x0);
            spinY0->setValue(r.y0);
            spinPa->setValue(r.pa);
            spinIncl->setValue(r.incl);
            spinRmax->setValue(r.rmax);
            spinVsys->setValue(r.vsys);
            spinVrot->setValue(r.vrot);
            spinSig->setValue(r.lineSigma);
            ringTable->setRowCount(0);
            for (const BackendKinematicRing &ring : r.rings)
                addRingRow(ring.r, ring.pa, ring.incl, ring.vrot);
            // The warped model is the fit's when it has rings enough to vary.
            perRingCheck->setChecked(r.rings.size() >= 3);
            fitInfo->setText(describeKinematicFit(r));
            fitInfo->setProperty("fitShown", true);
            this->publishKinematicFit(r, selection);
        });
        const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
        const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
        const double sigma = this->m_noiseSigma;
        watcher->setFuture(QtConcurrent::run([=]() {
            BackendClient c(url, token);
            c.setSessionId(session);
            return c.kinematicFit(dataset, mask, o[0], o[1], o[2], d[0], d[1], d[2], sigma);
        }));
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Close, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Overlay"_s);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::close);
    outer->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                     [this, dlgPtr, nx, ny, nz, perRingCheck, readRings, spinX0, spinY0,
                      spinPa, spinIncl, spinRmax, spinVsys, spinVrot, spinSig, fitInfo]() {
    // isRunning() is false as soon as the WORKER ends, while `finished` is still
    // queued for the GUI thread. A second click in that window replaced the
    // member watcher's future and overwrote m_kinematicSheet, so the first
    // sheet stayed on screen forever and its result was dropped (
    // reproduced in a Qt harness). The flag is held until the result has been
    // HANDLED, not until the worker stops.
    if (this->m_kinematicBusy)
        return;
    this->m_kinematicBusy = true;

    // Build the per-ring profile from the table (empty → scalar model). Validate
    // each cell so a stray non-numeric entry doesn't silently become 0.
    QVector<std::array<double, 4>> rings;
    if (perRingCheck->isChecked()) {
        QString error;
        rings = readRings(&error);
        if (error.isEmpty() && rings.isEmpty())
            error = u"Per-ring profile is enabled but the ring table is empty. Add at least "
                    u"one ring, or disable per-ring mode."_s;
        if (!error.isEmpty()) {
            QMessageBox::warning(dlgPtr, u"Kinematic Model"_s, error);
            this->m_kinematicBusy = false;
            return;
        }
    }

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
    this->m_kinematicSheet = visivo::showToolProgressSheet(
            dlgPtr, u"Building the tilted-ring model"_s);
    this->m_kinematicWatcher.setFuture(QtConcurrent::run(
            &fetchKinematicModelWorker, this->remoteBackendUrl, this->remoteBackendToken,
            this->remoteSessionId, this->remoteDatasetId, nx, ny, nz,
            ddim[0], ddim[1], ddim[2], dorg[0], dorg[1], dorg[2],
            spinX0->value(), spinY0->value(), spinPa->value(), spinIncl->value(),
            spinVsys->value(), spinVrot->value(), spinRmax->value(), spinSig->value(),
            rings));

    // With a selection on screen, the comparator also says what THIS model —
    // fitted or edited — leaves behind: its residual against the selection's
    // velocity field (POST /v1/cube/kinematic_residual_bin).
    const quint64 generation = ++this->m_kinOverlayGeneration;
    this->m_kinBuildGeneration = generation;
    this->m_kinPendingResidual = nullptr;   // an older Overlay's residual is moot now
    if (!this->m_lassoMaskBytes.isEmpty()) {
        const QByteArray mask = this->m_lassoMaskBytes;
        const std::array<int, 3> o = { this->m_lassoMaskOrigin[0], this->m_lassoMaskOrigin[1],
                                       this->m_lassoMaskOrigin[2] };
        const std::array<int, 3> d = { this->m_lassoMaskDims[0], this->m_lassoMaskDims[1],
                                       this->m_lassoMaskDims[2] };
        const QString selection = u"Kinematic Lasso, %1 voxels"_s.arg(
                this->m_lassoCountLabel ? this->m_lassoCountLabel->text().section(u' ', 0, 0)
                                        : u"?"_s);
        const QString url = this->remoteBackendUrl, token = this->remoteBackendToken;
        const QString session = this->remoteSessionId, dataset = this->remoteDatasetId;
        const double x0 = spinX0->value(), y0 = spinY0->value(), vsys = spinVsys->value();
        const double lineSigma = spinSig->value(), pa = spinPa->value(), incl = spinIncl->value();
        const double vrot = spinVrot->value(), rmax = spinRmax->value();
        const double sigma = this->m_noiseSigma;
        QVariantMap params;
        params.insert(u"Selection"_s, selection);
        params.insert(u"Model"_s, rings.isEmpty()
                ? u"flat disc: centre (%1, %2), PA %3°, i %4°, V_rot %5, r_max %6, V_sys %7"_s
                          .arg(x0, 0, 'f', 1).arg(y0, 0, 'f', 1).arg(pa, 0, 'f', 1)
                          .arg(incl, 0, 'f', 1).arg(vrot, 0, 'f', 1).arg(rmax, 0, 'f', 1)
                          .arg(vsys, 0, 'f', 1)
                : u"%1 rings, centre (%2, %3), V_sys %4"_s.arg(rings.size())
                          .arg(x0, 0, 'f', 1).arg(y0, 0, 'f', 1).arg(vsys, 0, 'f', 1));
        auto *rw = new QFutureWatcher<BackendKinematicResidualResult>(dlgPtr);
        // The label may go with its dialog before a deferred publication runs.
        QPointer<QLabel> infoLabel(fitInfo);
        QObject::connect(rw, &QFutureWatcher<BackendKinematicResidualResult>::finished, dlgPtr,
                         [this, rw, infoLabel, params, mask, o, d, generation]() mutable {
            const BackendKinematicResidualResult r = rw->result();
            rw->deleteLater();
            // Only the latest Overlay's residual, and only for the selection still
            // shown (the same mask at the same place).
            const bool sameSelection = this->m_lassoMaskBytes == mask
                    && this->m_lassoMaskOrigin[0] == o[0] && this->m_lassoMaskOrigin[1] == o[1]
                    && this->m_lassoMaskOrigin[2] == o[2] && this->m_lassoMaskDims[0] == d[0]
                    && this->m_lassoMaskDims[1] == d[1] && this->m_lassoMaskDims[2] == d[2];
            if (!r.valid || generation != this->m_kinOverlayGeneration || !sameSelection)
                return;
            // Publish once THIS Overlay's model is the one on screen: the build may
            // still be running, or may fail and leave the previous overlay up.
            auto publish = [this, r, infoLabel, params, mask, o, d, generation]() mutable {
                // Deferred until the model is in place: still this Overlay, still
                // this selection?
                if (generation != this->m_kinOverlayGeneration || this->m_lassoMaskBytes != mask
                    || this->m_lassoMaskOrigin[0] != o[0] || this->m_lassoMaskOrigin[1] != o[1]
                    || this->m_lassoMaskOrigin[2] != o[2] || this->m_lassoMaskDims[0] != d[0]
                    || this->m_lassoMaskDims[1] != d[1] || this->m_lassoMaskDims[2] != d[2])
                    return;
                const auto f = [](double v) {
                    return std::isfinite(v) ? QString::number(v, 'f', 1) : u"-"_s;
                };
                const QString line = u"|v − v_model| median %1, p90 %2 km/s over %3 px"_s
                                             .arg(f(r.residual.median), f(r.residual.p90))
                                             .arg(r.residual.nPix);
                params.insert(u"Noise σ"_s, u"%1 (%2)"_s.arg(r.sigma, 0, 'g', 4).arg(r.sigmaSource));
                params.insert(u"Velocities"_s, u"km/s, %1"_s.arg(r.velocityConvention));
                params.insert(u"Residual"_s, line);
                if (auto img = fitPlaneImage(fitResidualPlane(r.velocityField, r.model), r.width,
                                             r.height))
                    this->addImageProduct(u"kinematics"_s, u"kinfit-resid-overlaid"_s,
                                          u"Residual v − v_model (overlaid model)"_s, img, u"km/s"_s,
                                          params, u"CoolWarm"_s, /*symmetricRange=*/true);
                if (infoLabel) {
                    QString text = infoLabel->text().section(u"\nOverlaid model: "_s, 0, 0);
                    if (!infoLabel->property("fitShown").toBool())
                        text.clear();
                    infoLabel->setText((text.isEmpty() ? QString() : text + u"\n"_s)
                                     + u"Overlaid model: "_s + line);
                    infoLabel->setProperty("fitShown", true);
                }
                this->statusBar()->showMessage(u"Overlaid model: "_s + line, 8000);
            };
            if (this->m_kinInstalledGeneration == generation) {
                publish();
            } else {
                this->m_kinPendingResidual = publish;
                this->m_kinPendingResidualGeneration = generation;
            }
        });
        rw->setFuture(QtConcurrent::run([=]() {
            BackendClient c(url, token);
            c.setSessionId(session);
            return c.kinematicResidual(dataset, mask, o[0], o[1], o[2], d[0], d[1], d[2], x0, y0,
                                       vsys, lineSigma, pa, incl, vrot, rmax, rings, sigma);
        }));
    }
    });   // ← Overlay handler

    // Tall enough for the parameters and a few rings without scrolling, never
    // taller than the screen it opens on.
    const QRect avail = this->screen() ? this->screen()->availableGeometry() : QRect(0, 0, 1280, 800);
    dlgPtr->resize(460, std::min(760, avail.height() - 80));
    dlgPtr->show();
    dlgPtr->raise();
    dlgPtr->activateWindow();
}

void vtkWindowCube::refreshKinematicFitAvailability()
{
    if (!m_kinFitBtn || !m_kinFitInfo)
        return;
    const bool haveSelection = !this->m_lassoMaskBytes.isEmpty();
    const bool growing = this->m_lassoSelectWatcher.isRunning()
            || this->m_lassoLevelsWatcher.isRunning() || this->m_lassoPendingRegrow;
    m_kinFitBtn->setEnabled(haveSelection && !growing && !m_kinFitRunning);
    // Keep a fit's summary; replace only the instructions.
    if (!m_kinFitInfo->property("fitShown").toBool())
        m_kinFitInfo->setText(haveSelection
                ? u"Fills in the parameters below from the selection on screen; they stay "
                  u"editable before you overlay the model."_s
                : u"Select the source with Tools › Sources & Kinematics › Kinematic Lasso, "
                  u"then fit it here, or type the parameters."_s);
}

QString vtkWindowCube::describeKinematicFit(const BackendKinematicFitResult &r)
{
    const auto f = [](double v, int prec = 1) {
        return std::isfinite(v) ? QString::number(v, 'f', prec) : u"-"_s;
    };
    QStringList lines;
    lines << u"Measured on %1 velocity-field pixels (S/N ≥ 3, σ = %2, %3); velocities %4."_s
                     .arg(r.nVelocityField).arg(r.sigma, 0, 'g', 4).arg(r.sigmaSource,
                                                                       r.velocityConvention);
    lines << u"Flux %1 %2 · w20 %3, w50 %4 km/s · V_sys (w20 midpoint) %5 km/s"_s
                     .arg(f(r.flux, 2), r.fluxIsJy ? u"Jy km/s"_s : u"(BUNIT km/s)"_s,
                          f(r.w20), f(r.w50), f(r.vsysW20));
    lines << u"Moment 0: centre (%1, %2) px, PA %3°, axis ratio %4 → i %5°, r_max %6 px"_s
                     .arg(f(r.x0), f(r.y0), f(r.paMorph), f(r.axisRatio, 2), f(r.incl), f(r.rmax));
    lines << u"Flat disc: PA %1°, V_sys %2, V_rot %3 (V_rot sin i %4) km/s; "
             u"|v − v_model| median %5, p90 %6 km/s over %7 px"_s
                     .arg(f(r.pa), f(r.vsys), f(r.vrot), f(r.vrotSini),
                          f(r.residualFlat.median), f(r.residualFlat.p90))
                     .arg(r.residualFlat.nPix);
    if (r.residualWarp.valid)
        lines << u"%1 rings: |v − v_model| median %2, p90 %3 km/s"_s
                         .arg(r.rings.size()).arg(f(r.residualWarp.median), f(r.residualWarp.p90));
    else
        lines << u"%1 populated ring(s): too few for a warped model."_s.arg(r.rings.size());
    lines << u"A kinematic footprint, not convolved with the beam: inner rings are beam-smeared."_s;
    return lines.join(u"\n"_s);
}

void vtkWindowCube::publishKinematicFit(const BackendKinematicFitResult &r,
                                        const QString &selectionDescription)
{
    const auto f = [](double v, int prec = 1) {
        return std::isfinite(v) ? QString::number(v, 'f', prec) : u"-"_s;
    };
    QVariantMap common;
    common.insert(u"Selection"_s, selectionDescription);
    common.insert(u"Noise σ"_s, u"%1 (%2)"_s.arg(r.sigma, 0, 'g', 4).arg(r.sigmaSource));
    common.insert(u"Velocities"_s, u"km/s, %1"_s.arg(r.velocityConvention));

    QVariantMap vf = common;
    vf.insert(u"Cut"_s, u"S/N ≥ 3 per pixel (σ × √channels)"_s);
    vf.insert(u"Pixels"_s, r.nVelocityField);
    if (auto img = fitPlaneImage(r.velocityField, r.width, r.height))
        this->addImageProduct(u"kinematics"_s, u"kinfit-vfield"_s, u"Velocity field (S/N ≥ 3)"_s,
                              img, u"km/s"_s, vf, u"CoolWarm"_s);

    QVariantMap flat = common;
    flat.insert(u"Centre"_s, u"(%1, %2) px"_s.arg(f(r.x0), f(r.y0)));
    flat.insert(u"PA"_s, u"%1°"_s.arg(f(r.pa)));
    flat.insert(u"Inclination"_s, u"%1° (moment-0 axis ratio %2)"_s.arg(f(r.incl), f(r.axisRatio, 2)));
    flat.insert(u"V_sys"_s, u"%1 km/s"_s.arg(f(r.vsys)));
    flat.insert(u"V_rot"_s, u"%1 km/s"_s.arg(f(r.vrot)));
    flat.insert(u"r_max"_s, u"%1 px"_s.arg(f(r.rmax)));
    if (auto img = fitPlaneImage(r.modelFlat, r.width, r.height))
        this->addImageProduct(u"kinematics"_s, u"kinfit-model-flat"_s,
                              u"Model velocity field (flat disc)"_s, img, u"km/s"_s, flat,
                              u"CoolWarm"_s);
    QVariantMap rf = flat;
    rf.insert(u"|v − v_model|"_s, u"median %1, p90 %2 km/s over %3 px"_s
                                          .arg(f(r.residualFlat.median), f(r.residualFlat.p90))
                                          .arg(r.residualFlat.nPix));
    if (auto img = fitPlaneImage(fitResidualPlane(r.velocityField, r.modelFlat), r.width, r.height))
        this->addImageProduct(u"kinematics"_s, u"kinfit-resid-flat"_s,
                              u"Residual v − v_model (flat disc)"_s, img, u"km/s"_s, rf,
                              u"CoolWarm"_s, /*symmetricRange=*/true);

    if (r.modelWarp.isEmpty()) {
        // No warped model this time: the previous fit's must not stay as if it were this one's.
        this->removeImageProduct(u"kinfit-model-warp"_s);
        this->removeImageProduct(u"kinfit-resid-warp"_s);
    } else {
        QVariantMap warp = common;
        QStringList ringText;
        for (const BackendKinematicRing &ring : r.rings)
            ringText << u"r %1: PA %2°, i %3°, V_rot %4 (%5 px)"_s
                                .arg(f(ring.r), f(ring.pa), f(ring.incl), f(ring.vrot))
                                .arg(ring.nPix);
        warp.insert(u"Rings"_s, ringText.join(u"; "_s));
        warp.insert(u"V_sys"_s, u"%1 km/s"_s.arg(f(r.vsys)));
        if (auto img = fitPlaneImage(r.modelWarp, r.width, r.height))
            this->addImageProduct(u"kinematics"_s, u"kinfit-model-warp"_s,
                                  u"Model velocity field (rings)"_s, img, u"km/s"_s, warp,
                                  u"CoolWarm"_s);
        warp.insert(u"|v − v_model|"_s, u"median %1, p90 %2 km/s over %3 px"_s
                                                .arg(f(r.residualWarp.median), f(r.residualWarp.p90))
                                                .arg(r.residualWarp.nPix));
        if (auto img = fitPlaneImage(fitResidualPlane(r.velocityField, r.modelWarp),
                                     r.width, r.height))
            this->addImageProduct(u"kinematics"_s, u"kinfit-resid-warp"_s,
                                  u"Residual v − v_model (rings)"_s, img, u"km/s"_s, warp,
                                  u"CoolWarm"_s, /*symmetricRange=*/true);
    }

    if (!r.spectrum.isEmpty() && r.spectrum.size() == r.spectrumVelocity.size()) {
        QVariantMap sp = common;
        sp.insert(u"Flux"_s, u"%1 %2"_s.arg(f(r.flux, 2),
                                            r.fluxIsJy ? u"Jy km/s"_s : u"(BUNIT km/s)"_s));
        this->addSpectrumProduct(u"kinematics"_s, u"kinfit-spectrum"_s,
                                 u"Integrated spectrum (selection)"_s, r.spectrumVelocity,
                                 r.spectrum, u"Velocity (km/s)"_s,
                                 r.fluxIsJy ? u"Flux density (Jy)"_s : u"Sum (BUNIT)"_s, sp);
    }

    QVariantMap props = common;
    props.insert(u"Flux"_s, u"%1 %2"_s.arg(f(r.flux, 2),
                                           r.fluxIsJy ? u"Jy km/s"_s : u"(BUNIT km/s)"_s));
    props.insert(u"w20 / w50"_s, u"%1 / %2 km/s"_s.arg(f(r.w20), f(r.w50)));
    props.insert(u"V_sys (w20 midpoint)"_s, u"%1 km/s"_s.arg(f(r.vsysW20)));
    props.insert(u"PA (moment 0)"_s, u"%1°"_s.arg(f(r.paMorph)));
    props.insert(u"Axis ratio → inclination"_s, u"%1 → %2°"_s.arg(f(r.axisRatio, 2), f(r.incl)));
    props.insert(u"Kinematic PA / V_sys / V_rot"_s,
                 u"%1° / %2 / %3 km/s"_s.arg(f(r.pa), f(r.vsys), f(r.vrot)));
    this->registerAnalysisProduct(u"kinematics"_s,
                                  u"Source properties: %1 %2"_s.arg(f(r.flux, 1),
                                          r.fluxIsJy ? u"Jy km/s"_s : u"(BUNIT km/s)"_s),
                                  props, /*window=*/nullptr, this->scopedProductId(u"kinfit-props"_s),
                                  /*renderer=*/QString(), /*revealProvenance=*/false,
                                  /*origin=*/{}, /*selectInSession=*/true,
                                  /*endpoint=*/u"/v1/cube/kinematic_fit_bin"_s);
    this->statusBar()->showMessage(
            u"Kinematic fit: PA %1°, i %2°, V_rot %3 km/s; maps and measures in Session Data."_s
                    .arg(f(r.pa), f(r.incl), f(r.vrot)),
            8000);
}

void vtkWindowCube::onKinematicModelReady()
{
    if (this->m_actKinematicModel)
        this->m_actKinematicModel->setEnabled(true);

    this->m_kinematicBusy = false;
    visivo::closeToolProgressSheet(this->m_kinematicSheet);
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
                u"Spectral mode: %1 (unit: %2). Set Vsys/Vrot/σ in that unit - "
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
                u"Cube display changed while building, re-run the kinematic overlay."_s, 6000);
        return;
    }

    // Replace any previous overlay so re-running never stacks surfaces, then keep
    // the model cube + contour for live restyling from the panel.
    this->clearKinematicModelOverlay();
    this->m_kinInstalledGeneration = this->m_kinBuildGeneration;
    if (this->m_kinPendingResidual
        && this->m_kinPendingResidualGeneration == this->m_kinInstalledGeneration) {
        auto publish = std::move(this->m_kinPendingResidual);
        this->m_kinPendingResidual = nullptr;
        QTimer::singleShot(0, this, publish);   // after the overlay is in place
    }
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
    if (this->m_kinematicSettings && this->lassoInspectorPanel())
        this->claimToolSettings(u"Kinematic Model"_s, this->m_kinematicSettings);
    else if (this->m_kinematicPanel) {
        this->m_kinematicPanel->show();
        this->m_kinematicPanel->raise();
    }
    // Draw the 2-D channel-map contour for the current channel (default-on).
    this->updateKinematic2dContour();

    this->statusBar()->showMessage(
            u"Kinematic model overlaid (%1 vertices, mode %2 [%3]), restyle it in the panel."_s
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
    if (this->m_kinematicSettings)
        return;   // built once; reused across overlays

    auto *body = new QWidget(this);
    auto *form = new QFormLayout(body);
    form->setContentsMargins(12, 10, 12, 10);   // as the lasso's block
    // A row that does not fit the Inspector's width puts its control under the
    // label instead of widening the column past the window.
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);

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
    rep->addItems({ u"Wireframe"_s, u"Surface"_s });
    rep->setItemData(0, u"Wireframe: the data stays visible through the model"_s, Qt::ToolTipRole);
    rep->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    rep->setMinimumContentsLength(9);
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

    this->m_kinematicSettings = body;
    if (!this->lassoInspectorPanel()) {
        // No workspace chrome (a bare cube window): a dock of its own, so the
        // overlay can still be restyled.
        auto *dock = new QDockWidget(u"Kinematic Model"_s, this);
        dock->setObjectName(u"kinematicModelDock"_s);
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        dock->setWidget(body);
        this->addDockWidget(Qt::RightDockWidgetArea, dock);
        if (ui->menuView)
            ui->menuView->addAction(dock->toggleViewAction());
        this->m_kinematicPanel = dock;
    }
}

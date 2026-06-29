#include "vtkWindowVectorField.h"

#include "ColorMaps.h"
#include "ColormapIcon.h"

#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkArrowSource.h>
#include <vtkAxesActor.h>
#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCamera.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkFloatArray.h>
#include <vtkGlyph3DMapper.h>
#include <vtkImageData.h>
#include <vtkIntArray.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
#include <vtkMaskPoints.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPointData.h>
#include <vtkPointSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkScalarBarActor.h>
#include <vtkStreamTracer.h>
#include <vtkTubeFilter.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

vtkWindowVectorField::vtkWindowVectorField(const QString &path, BackendVelocityGridResult grid,
                                           const QString &datasetId, const QString &backendUrl,
                                           const QString &backendToken, const QString &sessionId,
                                           QWidget *parent)
    : QMainWindow(parent), m_path(path), m_grid(std::move(grid)), m_datasetId(datasetId),
      m_backendUrl(backendUrl), m_backendToken(backendToken), m_sessionId(sessionId)
{
    this->setWindowTitle(u"Velocity Field — %1"_s.arg(QFileInfo(path).fileName()));
    this->lut = vtkSmartPointer<vtkLookupTable>::New();
    this->axesWidget = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
    this->scalarBar = vtkSmartPointer<vtkScalarBarActor>::New();

    this->setupUi();
    this->setupRenderer();
    this->applyColorMap(u"Inferno"_s);
    this->rebuildLodActors();
    this->resetCamera();
}

// Per-axis full-resolution cell size (point-centred): box>0 → box/(n-1), else 1.
void vtkWindowVectorField::fullCells(double &sx, double &sy, double &sz) const
{
    const int fnx = std::max(2, m_grid.fullNx);
    const int fny = std::max(2, m_grid.fullNy);
    const int fnz = std::max(2, m_grid.fullNz);
    sx = (m_boxMpc > 0.0) ? m_boxMpc / (fnx - 1) : 1.0;
    sy = (m_boxMpc > 0.0) ? m_boxMpc / (fny - 1) : 1.0;
    sz = (m_boxMpc > 0.0) ? m_boxMpc / (fnz - 1) : 1.0;
}

// Centred origin per axis: -0.5*fullCell*(fullN-1)  (== -box/2 when box set).
void vtkWindowVectorField::originVec(double &ox, double &oy, double &oz) const
{
    double sx, sy, sz;
    this->fullCells(sx, sy, sz);
    ox = -0.5 * sx * (std::max(2, m_grid.fullNx) - 1);
    oy = -0.5 * sy * (std::max(2, m_grid.fullNy) - 1);
    oz = -0.5 * sz * (std::max(2, m_grid.fullNz) - 1);
}

void vtkWindowVectorField::setupUi()
{
    auto *central = new QWidget(this);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);

    this->vtkWidget = new QVTKOpenGLNativeWidget(central);
    this->vtkWidget->setMinimumSize(700, 520);
    root->addWidget(this->vtkWidget, 1);

    auto *side = new QFrame(central);
    side->setFrameShape(QFrame::StyledPanel);
    side->setMaximumWidth(300);
    auto *sl = new QVBoxLayout(side);

    this->summaryLabel = new QLabel(side);
    this->summaryLabel->setWordWrap(true);
    sl->addWidget(this->summaryLabel);

    auto *form = new QFormLayout;

    this->chkGlyphs = new QCheckBox(side);
    this->chkGlyphs->setChecked(true);
    form->addRow(u"Arrows"_s, this->chkGlyphs);

    this->chkStreamlines = new QCheckBox(side);
    this->chkStreamlines->setChecked(true);
    form->addRow(u"Streamlines"_s, this->chkStreamlines);

    this->spinMaxArrows = new QSpinBox(side);
    this->spinMaxArrows->setRange(1000, 500000);
    this->spinMaxArrows->setSingleStep(10000);
    this->spinMaxArrows->setValue(50000);
    this->spinMaxArrows->setToolTip(
            u"Max arrows drawn (uniformly sampled) — caps cost on large grids"_s);
    form->addRow(u"Max arrows"_s, this->spinMaxArrows);

    this->spinGlyphScale = new QDoubleSpinBox(side);
    this->spinGlyphScale->setRange(0.0, 100.0);
    this->spinGlyphScale->setDecimals(3);
    this->spinGlyphScale->setSingleStep(0.05);
    this->spinGlyphScale->setValue(1.0);
    form->addRow(u"Arrow scale"_s, this->spinGlyphScale);

    this->spinSeeds = new QSpinBox(side);
    this->spinSeeds->setRange(10, 5000);
    this->spinSeeds->setValue(200);
    form->addRow(u"Seeds"_s, this->spinSeeds);

    this->spinBoxSize = new QDoubleSpinBox(side);
    this->spinBoxSize->setRange(0.0, 100000.0);
    this->spinBoxSize->setDecimals(1);
    this->spinBoxSize->setSingleStep(10.0);
    this->spinBoxSize->setValue(0.0);
    this->spinBoxSize->setSpecialValueText(u"grid units"_s);
    this->spinBoxSize->setToolTip(u"Physical box size in Mpc (0 = grid units)"_s);
    form->addRow(u"Box size (Mpc)"_s, this->spinBoxSize);

    this->comboLut = new QComboBox(side);
    this->comboLut->setIconSize(QSize(80, 14));
    for (const auto &name : ColorMaps::GetColorMapNames()) {
        this->comboLut->addItem(makeColormapIcon(name), QString::fromStdString(name));
    }
    this->comboLut->setCurrentText(u"Inferno"_s);
    form->addRow(u"Colormap"_s, this->comboLut);

    this->chkRoi = new QCheckBox(side);
    this->chkRoi->setToolTip(u"Show an interactive box; drag it to choose a region"_s);
    form->addRow(u"Define ROI"_s, this->chkRoi);

    this->chkBasins = new QCheckBox(side);
    this->chkBasins->setToolTip(
            u"Gravitational basins (watershed): regions whose streamlines converge to the "
            u"same attractor — the Laniakea-style supercluster definition."_s);
    form->addRow(u"Show basins"_s, this->chkBasins);

    sl->addLayout(form);

    this->btnLoadRoi = new QPushButton(u"Load full-res ROI"_s, side);
    this->btnLoadRoi->setEnabled(false);
    this->btnLoadRoi->setToolTip(
            u"Fetch the boxed region at full resolution (the display grid is a downsampled LOD)"_s);
    sl->addWidget(this->btnLoadRoi);

    sl->addStretch(1);
    root->addWidget(side, 0);
    this->setCentralWidget(central);

    // Summary text (incl. LOD note when the display grid is downsampled).
    QString lodNote;
    if (m_grid.downsampleFactor > 1) {
        lodNote = u" · LOD ÷%1 of %2³"_s.arg(m_grid.downsampleFactor)
                          .arg(std::max({ m_grid.fullNx, m_grid.fullNy, m_grid.fullNz }));
    }
    this->summaryLabel->setText(
            u"Grid %1×%2×%3 · speed %4–%5 %6%7"_s.arg(m_grid.nx).arg(m_grid.ny).arg(m_grid.nz)
                    .arg(m_grid.speedMin, 0, 'g', 3).arg(m_grid.speedMax, 0, 'g', 3)
                    .arg(m_grid.unit).arg(lodNote));

    // The ROI (full-res on demand) is only meaningful when the display grid is a
    // downsampled LOD. For an already-full-resolution field it would re-fetch the
    // same data — disable it and say why, so it doesn't mislead.
    if (m_grid.downsampleFactor <= 1) {
        const QString why =
                u"This field is already at full resolution (LOD only kicks in above 128³), "
                u"so a full-res ROI would show the same data."_s;
        this->chkRoi->setEnabled(false);
        this->chkRoi->setToolTip(why);
        this->btnLoadRoi->setToolTip(why);
        if (auto *lbl = qobject_cast<QLabel *>(form->labelForField(this->chkRoi))) {
            lbl->setText(u"Define ROI (n/a — full res)"_s);
        }
    }

    QObject::connect(this->chkGlyphs, &QCheckBox::toggled, this, [this](bool on) {
        if (this->glyphActor) this->glyphActor->SetVisibility(on);
        if (this->roiGlyphActor) this->roiGlyphActor->SetVisibility(on);
        this->renderWindow->Render();
    });
    QObject::connect(this->chkStreamlines, &QCheckBox::toggled, this, [this](bool on) {
        if (this->streamActor) this->streamActor->SetVisibility(on);
        if (this->roiStreamActor) this->roiStreamActor->SetVisibility(on);
        this->renderWindow->Render();
    });
    QObject::connect(this->spinMaxArrows, &QSpinBox::valueChanged, this,
                     [this](int) { this->rebuildLodActors(); this->renderWindow->Render(); });
    QObject::connect(this->spinGlyphScale, &QDoubleSpinBox::valueChanged, this,
                     [this](double) { this->rebuildLodActors(); this->renderWindow->Render(); });
    QObject::connect(this->spinSeeds, &QSpinBox::valueChanged, this,
                     [this](int) { this->rebuildLodActors(); this->renderWindow->Render(); });
    QObject::connect(this->spinBoxSize, &QDoubleSpinBox::valueChanged, this,
                     [this](double mpc) { this->applyBoxSize(mpc); });
    QObject::connect(this->comboLut, &QComboBox::currentTextChanged, this,
                     [this](const QString &name) { this->applyColorMap(name);
                                                   this->renderWindow->Render(); });
    QObject::connect(this->chkRoi, &QCheckBox::toggled, this,
                     [this](bool on) { this->toggleRoiWidget(on); });
    QObject::connect(this->btnLoadRoi, &QPushButton::clicked, this,
                     [this]() { this->loadFullResRoi(); });
    QObject::connect(this->chkBasins, &QCheckBox::toggled, this,
                     [this](bool on) { this->showBasins(on); });
}

void vtkWindowVectorField::setupRenderer()
{
    this->vtkWidget->setRenderWindow(this->renderWindow.Get());
    this->vtkWidget->setEnableTouchEventProcessing(false);
    this->renderer->SetBackground(0.04, 0.04, 0.08);
    this->renderWindow->AddRenderer(this->renderer);

    auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
    this->vtkWidget->interactor()->SetInteractorStyle(style);

    vtkNew<vtkAxesActor> axesActor;
    this->axesWidget->SetOrientationMarker(axesActor);
    this->axesWidget->SetInteractor(this->vtkWidget->interactor());
    this->axesWidget->SetViewport(0.0, 0.0, 0.14, 0.14);
    this->axesWidget->EnabledOn();
    this->axesWidget->InteractiveOff();

    this->scalarBar->SetLookupTable(this->lut);
    this->scalarBar->SetTitle("|v|");
    this->scalarBar->SetMaximumWidthInPixels(90);
    this->renderer->AddActor2D(this->scalarBar);
}

void vtkWindowVectorField::buildImageData(const BackendVelocityGridResult &grid,
                                          vtkSmartPointer<vtkImageData> &out, double sx, double sy,
                                          double sz, double ox, double oy, double oz)
{
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const vtkIdType npts = vtkIdType(nx) * ny * nz;

    out = vtkSmartPointer<vtkImageData>::New();
    out->SetDimensions(nx, ny, nz);
    out->SetSpacing(sx, sy, sz);
    out->SetOrigin(ox, oy, oz);

    vtkNew<vtkFloatArray> vectors;
    vectors->SetName("velocity");
    vectors->SetNumberOfComponents(3);
    vectors->SetNumberOfTuples(npts);
    std::memcpy(vectors->GetVoidPointer(0), grid.vectors.constData(),
                std::size_t(npts) * 3 * sizeof(float));

    const float *vp = static_cast<const float *>(vectors->GetVoidPointer(0));
    vtkNew<vtkFloatArray> speed;
    speed->SetName("speed");
    speed->SetNumberOfComponents(1);
    speed->SetNumberOfTuples(npts);
    float *sp = static_cast<float *>(speed->GetVoidPointer(0));
    for (vtkIdType i = 0; i < npts; ++i) {
        const float vx = vp[3 * i], vy = vp[3 * i + 1], vz = vp[3 * i + 2];
        sp[i] = std::sqrt(vx * vx + vy * vy + vz * vz);
    }
    out->GetPointData()->SetVectors(vectors);
    out->GetPointData()->SetScalars(speed);
}

vtkSmartPointer<vtkActor> vtkWindowVectorField::makeGlyphActor(vtkImageData *img, double cell)
{
    const int maxArrows = this->spinMaxArrows ? this->spinMaxArrows->value() : 50000;
    vtkNew<vtkMaskPoints> mask;
    mask->SetInputData(img);
    mask->RandomModeOn();
    mask->SetRandomModeType(1);                 // uniform spatial sampling
    mask->SetMaximumNumberOfPoints(maxArrows);

    vtkNew<vtkArrowSource> arrow;
    const double speedMax = std::max(1e-12, m_grid.fullSpeedMax);
    const double userScale = this->spinGlyphScale ? this->spinGlyphScale->value() : 1.0;
    const double scaleFactor = userScale * 3.0 * cell / speedMax;

    auto gmapper = vtkSmartPointer<vtkGlyph3DMapper>::New();
    gmapper->SetInputConnection(mask->GetOutputPort());   // uniformly subsampled points
    gmapper->SetSourceConnection(arrow->GetOutputPort());
    gmapper->SetOrientationArray("velocity");
    gmapper->SetOrientationModeToDirection();
    gmapper->SetScaleArray("velocity");
    gmapper->SetScaleModeToScaleByMagnitude();
    gmapper->SetScaleFactor(scaleFactor);
    gmapper->SetLookupTable(this->lut);
    gmapper->SetScalarRange(m_grid.fullSpeedMin, m_grid.fullSpeedMax);
    gmapper->SelectColorArray("speed");
    gmapper->SetScalarModeToUsePointFieldData();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(gmapper);
    actor->SetVisibility(this->chkGlyphs ? this->chkGlyphs->isChecked() : true);
    return actor;
}

vtkSmartPointer<vtkActor> vtkWindowVectorField::makeStreamActor(vtkImageData *img)
{
    double bounds[6];
    img->GetBounds(bounds);
    const double cx = 0.5 * (bounds[0] + bounds[1]);
    const double cy = 0.5 * (bounds[2] + bounds[3]);
    const double cz = 0.5 * (bounds[4] + bounds[5]);
    const double ext = std::max({ bounds[1] - bounds[0], bounds[3] - bounds[2],
                                  bounds[5] - bounds[4] });

    vtkNew<vtkPointSource> seeds;
    seeds->SetCenter(cx, cy, cz);
    seeds->SetRadius(0.3 * ext);
    seeds->SetNumberOfPoints(this->spinSeeds ? this->spinSeeds->value() : 200);

    vtkNew<vtkStreamTracer> tracer;
    tracer->SetInputData(img);
    tracer->SetSourceConnection(seeds->GetOutputPort());
    tracer->SetIntegratorTypeToRungeKutta45();
    tracer->SetIntegrationDirectionToBoth();
    tracer->SetMaximumPropagation(2.0 * ext);
    tracer->SetMaximumNumberOfSteps(2000);
    tracer->SetComputeVorticity(false);

    vtkNew<vtkTubeFilter> tube;
    tube->SetInputConnection(tracer->GetOutputPort());
    tube->SetRadius(0.0025 * ext);
    tube->SetNumberOfSides(4);

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(tube->GetOutputPort());
    mapper->SetLookupTable(this->lut);
    mapper->SetScalarRange(m_grid.fullSpeedMin, m_grid.fullSpeedMax);
    mapper->SelectColorArray("speed");
    mapper->SetScalarModeToUsePointFieldData();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->SetVisibility(this->chkStreamlines ? this->chkStreamlines->isChecked() : true);
    return actor;
}

void vtkWindowVectorField::rebuildLodActors()
{
    if (this->glyphActor) this->renderer->RemoveActor(this->glyphActor);
    if (this->streamActor) this->renderer->RemoveActor(this->streamActor);

    double fsx, fsy, fsz, ox, oy, oz;
    this->fullCells(fsx, fsy, fsz);
    this->originVec(ox, oy, oz);
    const int factor = std::max(1, m_grid.downsampleFactor);
    const double dsx = fsx * factor, dsy = fsy * factor, dsz = fsz * factor;  // LOD cell
    this->buildImageData(m_grid, this->imageData, dsx, dsy, dsz, ox, oy, oz);

    const double cell = std::min({ dsx, dsy, dsz });   // glyph scale uses smallest cell
    this->glyphActor = this->makeGlyphActor(this->imageData, cell);
    this->streamActor = this->makeStreamActor(this->imageData);
    this->renderer->AddActor(this->glyphActor);
    this->renderer->AddActor(this->streamActor);
}

void vtkWindowVectorField::applyBoxSize(double boxMpc)
{
    this->m_boxMpc = boxMpc;
    // The ROI box widget's world bounds are stale under a new scale — turn it off
    // (the user re-defines it) and drop any loaded full-res ROI.
    if (this->chkRoi && this->chkRoi->isChecked()) {
        this->chkRoi->setChecked(false);   // triggers toggleRoiWidget(false)
    }
    if (this->roiGlyphActor) { this->renderer->RemoveActor(this->roiGlyphActor); this->roiGlyphActor = nullptr; }
    if (this->roiStreamActor) { this->renderer->RemoveActor(this->roiStreamActor); this->roiStreamActor = nullptr; }
    this->rebuildLodActors();
    // Re-place basins at the new scale (labels are scale-independent, cached).
    if (this->basinActor && m_basins.valid) {
        this->placeBasins();
    }
    this->resetCamera();
    this->renderWindow->Render();
}

void vtkWindowVectorField::applyColorMap(const QString &name)
{
    ColorMaps::SetColorMap(this->lut, name.toStdString());
    this->lut->SetTableRange(m_grid.fullSpeedMin, m_grid.fullSpeedMax);
    this->lut->Build();
}

void vtkWindowVectorField::resetCamera()
{
    this->renderer->ResetCamera();
    this->renderWindow->Render();
}

void vtkWindowVectorField::toggleRoiWidget(bool on)
{
    if (on) {
        if (!this->roiWidget) {
            this->roiWidget = vtkSmartPointer<vtkBoxWidget2>::New();
            this->roiWidget->SetInteractor(this->vtkWidget->interactor());
            auto rep = vtkSmartPointer<vtkBoxRepresentation>::New();
            this->roiWidget->SetRepresentation(rep);
        }
        // Place the box on the central half of the display grid.
        double b[6];
        this->imageData->GetBounds(b);
        const double mx = 0.25 * (b[1] - b[0]), my = 0.25 * (b[3] - b[2]),
                     mz = 0.25 * (b[5] - b[4]);
        double place[6] = { b[0] + mx, b[1] - mx, b[2] + my, b[3] - my, b[4] + mz, b[5] - mz };
        this->roiWidget->GetRepresentation()->PlaceWidget(place);
        this->roiWidget->On();
    } else if (this->roiWidget) {
        this->roiWidget->Off();
    }
    if (this->btnLoadRoi) this->btnLoadRoi->setEnabled(on);
    this->renderWindow->Render();
}

void vtkWindowVectorField::loadFullResRoi()
{
    if (!this->roiWidget) return;
    auto *rep = vtkBoxRepresentation::SafeDownCast(this->roiWidget->GetRepresentation());
    if (!rep) return;
    const double *bb = rep->GetBounds();   // world bounds of the box (transient ptr)
    if (!bb) return;
    const double b[6] = { bb[0], bb[1], bb[2], bb[3], bb[4], bb[5] };

    double fsx, fsy, fsz, ox, oy, oz;
    this->fullCells(fsx, fsy, fsz);
    this->originVec(ox, oy, oz);
    const auto toIdx = [](double w, double origin, double cell, int n) {
        return std::clamp(int(std::lround((w - origin) / cell)), 0, n - 1);
    };
    const int x0 = toIdx(b[0], ox, fsx, m_grid.fullNx), x1 = toIdx(b[1], ox, fsx, m_grid.fullNx);
    const int y0 = toIdx(b[2], oy, fsy, m_grid.fullNy), y1 = toIdx(b[3], oy, fsy, m_grid.fullNy);
    const int z0 = toIdx(b[4], oz, fsz, m_grid.fullNz), z1 = toIdx(b[5], oz, fsz, m_grid.fullNz);

    BackendClient client(m_backendUrl, m_backendToken);
    client.setSessionId(m_sessionId);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const BackendVelocityGridResult roi =
            client.fetchVelocitySubgrid(m_datasetId, x0, x1 + 1, y0, y1 + 1, z0, z1 + 1);
    QApplication::restoreOverrideCursor();
    if (!roi.valid) {
        QMessageBox::warning(this, u"ROI"_s,
                             u"Full-res ROI fetch failed: %1"_s.arg(roi.error));
        return;
    }

    if (this->roiGlyphActor) this->renderer->RemoveActor(this->roiGlyphActor);
    if (this->roiStreamActor) this->renderer->RemoveActor(this->roiStreamActor);

    // Place the full-res sub-grid at the world position of its start index
    // (full-res spacing, per axis) so it lands exactly inside the LOD context.
    const double rox = ox + roi.roiX0 * fsx;
    const double roy = oy + roi.roiY0 * fsy;
    const double roz = oz + roi.roiZ0 * fsz;
    this->buildImageData(roi, this->roiImageData, fsx, fsy, fsz, rox, roy, roz);

    const double roiCell = std::min({ fsx, fsy, fsz });
    this->roiGlyphActor = this->makeGlyphActor(this->roiImageData, roiCell);
    this->roiStreamActor = this->makeStreamActor(this->roiImageData);
    this->renderer->AddActor(this->roiGlyphActor);
    this->renderer->AddActor(this->roiStreamActor);
    this->renderWindow->Render();
    this->statusBar()->showMessage(
            u"Full-res ROI: %1×%2×%3 at [%4,%5,%6]"_s.arg(roi.nx).arg(roi.ny).arg(roi.nz)
                    .arg(roi.roiX0).arg(roi.roiY0).arg(roi.roiZ0), 5000);
}

void vtkWindowVectorField::showBasins(bool on)
{
    if (!on) {
        if (this->basinActor) { this->renderer->RemoveActor(this->basinActor); this->basinActor = nullptr; }
        this->m_basins = BackendVelocityBasinsResult{};
        this->renderWindow->Render();
        return;
    }

    BackendClient client(m_backendUrl, m_backendToken);
    client.setSessionId(m_sessionId);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    this->statusBar()->showMessage(u"Computing gravitational basins…"_s);
    this->m_basins = client.fetchVelocityBasins(m_datasetId, 64, 1);
    QApplication::restoreOverrideCursor();
    if (!m_basins.valid || m_basins.nBasins <= 0) {
        QMessageBox::warning(this, u"Basins"_s,
                             m_basins.valid ? u"No basins found."_s
                                            : u"Basin computation failed: %1"_s.arg(m_basins.error));
        this->m_basins = BackendVelocityBasinsResult{};
        if (this->chkBasins) { QSignalBlocker blk(this->chkBasins); this->chkBasins->setChecked(false); }
        return;
    }
    this->placeBasins();
}

void vtkWindowVectorField::placeBasins()
{
    if (!m_basins.valid || m_basins.nBasins <= 0) {
        return;
    }
    const BackendVelocityBasinsResult &b = this->m_basins;

    // Basin grid spans the same physical box as the display; place it per axis.
    double ox, oy, oz;
    this->originVec(ox, oy, oz);
    const double ex = -2.0 * ox, ey = -2.0 * oy, ez = -2.0 * oz;   // per-axis extent
    const double cx = (b.nx > 1) ? ex / (b.nx - 1) : 1.0;
    const double cy = (b.ny > 1) ? ey / (b.ny - 1) : 1.0;
    const double cz = (b.nz > 1) ? ez / (b.nz - 1) : 1.0;

    vtkNew<vtkImageData> img;
    img->SetDimensions(b.nx, b.ny, b.nz);
    img->SetSpacing(cx, cy, cz);
    img->SetOrigin(ox, oy, oz);
    const vtkIdType npts = vtkIdType(b.nx) * b.ny * b.nz;
    vtkNew<vtkIntArray> labels;
    labels->SetName("basin");
    labels->SetNumberOfComponents(1);
    labels->SetNumberOfTuples(npts);
    std::memcpy(labels->GetVoidPointer(0), b.labels.constData(),
                std::size_t(npts) * sizeof(vtkTypeInt32));
    img->GetPointData()->SetScalars(labels);

    vtkNew<vtkDiscreteFlyingEdges3D> surf;   // one closed surface per basin label
    surf->SetInputData(img);
    surf->GenerateValues(b.nBasins, 1, b.nBasins);

    // Categorical colours; label 0 (unassigned) is fully transparent.
    this->basinLut = vtkSmartPointer<vtkLookupTable>::New();
    this->basinLut->SetNumberOfTableValues(b.nBasins + 1);
    this->basinLut->SetTableRange(0, b.nBasins);
    this->basinLut->Build();
    this->basinLut->SetTableValue(0, 0.0, 0.0, 0.0, 0.0);
    for (int i = 1; i <= b.nBasins; ++i) {
        const double hue = std::fmod(0.61 + 0.137 * (i - 1), 1.0);   // golden-angle hue spread
        const QColor c = QColor::fromHsvF(hue, 0.65, 0.95);
        this->basinLut->SetTableValue(i, c.redF(), c.greenF(), c.blueF(), 1.0);
    }

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(surf->GetOutputPort());
    mapper->SetLookupTable(this->basinLut);
    mapper->SetScalarRange(0, b.nBasins);

    if (this->basinActor) this->renderer->RemoveActor(this->basinActor);
    this->basinActor = vtkSmartPointer<vtkActor>::New();
    this->basinActor->SetMapper(mapper);
    this->basinActor->GetProperty()->SetOpacity(0.35);   // see streamlines through the basins
    this->renderer->AddActor(this->basinActor);
    this->renderWindow->Render();
    this->statusBar()->showMessage(u"%1 gravitational basins"_s.arg(b.nBasins), 5000);
}

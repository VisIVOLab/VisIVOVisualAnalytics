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
#include <vtkDataObject.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGradientFilter.h>
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
#include <QSlider>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <vector>

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

    // ── Flow: glyphs / streamlines / display scale ──────────────────────────
    auto *flowGroup = new QGroupBox(u"Flow"_s, side);
    auto *form = new QFormLayout(flowGroup);

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
    // Commit only when the user finishes typing (Enter / focus-out) or steps the
    // arrows — not on every digit, so the scene isn't rebuilt at "2" → "20" → …
    this->spinBoxSize->setKeyboardTracking(false);
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
    sl->addWidget(flowGroup);

    // ── Structure: ROI + derived cosmography (basins, density) ──────────────
    auto *structGroup = new QGroupBox(u"Structure"_s, side);
    auto *structForm = new QFormLayout(structGroup);

    this->chkRoi = new QCheckBox(side);
    this->chkRoi->setToolTip(u"Show an interactive box; drag it to choose a region"_s);
    structForm->addRow(u"Define ROI"_s, this->chkRoi);

    this->chkBasins = new QCheckBox(side);
    this->chkBasins->setToolTip(
            u"Gravitational basins (watershed): regions whose streamlines converge to the "
            u"same attractor — the Laniakea-style supercluster definition."_s);
    structForm->addRow(u"Show basins"_s, this->chkBasins);

    this->comboBasinDir = new QComboBox(side);
    this->comboBasinDir->addItem(u"Attraction (superclusters)"_s);
    this->comboBasinDir->addItem(u"Repulsion (voids)"_s);
    this->comboBasinDir->setToolTip(
            u"Basins of attraction (where flows converge → superclusters) or of "
            u"repulsion (where flows diverge → voids / repellers)."_s);
    structForm->addRow(u"Basin type"_s, this->comboBasinDir);

    this->chkFieldDensity = new QCheckBox(side);
    this->chkFieldDensity->setToolTip(
            u"Overdensity proxy from the velocity divergence (δ ∝ −∇·v, linear "
            u"theory): an isosurface of the convergent (∇·v < 0) cores. Best on a "
            u"structured field — a uniform-divergence flow has no isosurface."_s);
    structForm->addRow(u"Overdensity (−∇·v)"_s, this->chkFieldDensity);
    this->chkFieldVoid = new QCheckBox(side);
    this->chkFieldVoid->setToolTip(
            u"Void proxy from the velocity divergence: an isosurface of the divergent "
            u"(∇·v > 0) cores — underdensities / repellers (the Local Void, Dipole "
            u"Repeller). Needs a diverging region (a flow source)."_s);
    structForm->addRow(u"Void (+∇·v)"_s, this->chkFieldVoid);
    this->sliderFieldDensityLevel = new QSlider(Qt::Horizontal, side);
    this->sliderFieldDensityLevel->setRange(5, 95);
    this->sliderFieldDensityLevel->setValue(35);
    this->sliderFieldDensityLevel->setToolTip(
            u"∇·v isosurface level, as % of the peak convergence/divergence (higher = "
            u"tighter cores). Applies to both Overdensity and Void."_s);
    structForm->addRow(u"∇·v level %"_s, this->sliderFieldDensityLevel);

    this->btnLoadRoi = new QPushButton(u"Load full-res ROI"_s, side);
    this->btnLoadRoi->setEnabled(false);
    this->btnLoadRoi->setToolTip(
            u"Fetch the boxed region at full resolution (the display grid is a downsampled LOD)"_s);
    structForm->addRow(this->btnLoadRoi);
    sl->addWidget(structGroup);

    // ── Galaxy overlay (CosmicFlows combo) ──────────────────────────────────
    auto *ovGroup = new QGroupBox(u"Galaxy overlay"_s, side);
    auto *ovLay = new QVBoxLayout(ovGroup);
    this->btnOverlayCatalogue = new QPushButton(u"Overlay galaxy catalogue…"_s, side);
    this->btnOverlayCatalogue->setToolTip(
            u"Load a Cartesian (X/Y/Z Mpc) catalogue and draw its galaxies as points in "
            u"this velocity field's frame — 'where galaxies sit in the flow'. Both must "
            u"share the same coordinate system/origin to align."_s);
    ovLay->addWidget(this->btnOverlayCatalogue);
    this->btnRemoveCatalogue = new QPushButton(u"Remove overlay"_s, side);
    this->btnRemoveCatalogue->setEnabled(false);
    this->btnRemoveCatalogue->setToolTip(u"Remove the overlaid galaxies, density and legend."_s);
    ovLay->addWidget(this->btnRemoveCatalogue);

    auto *overlayForm = new QFormLayout;
    overlayForm->setContentsMargins(0, 0, 0, 0);
    this->chkShowCatalogue = new QCheckBox(side);
    this->chkShowCatalogue->setChecked(true);
    this->chkShowCatalogue->setEnabled(false);   // enabled once a catalogue loads
    overlayForm->addRow(u"Show galaxies"_s, this->chkShowCatalogue);
    this->spinCataloguePointSize = new QDoubleSpinBox(side);
    this->spinCataloguePointSize->setRange(1.0, 12.0);
    this->spinCataloguePointSize->setSingleStep(0.5);
    this->spinCataloguePointSize->setValue(3.0);
    this->spinCataloguePointSize->setEnabled(false);
    overlayForm->addRow(u"Galaxy size"_s, this->spinCataloguePointSize);
    this->comboGalaxyColor = new QComboBox(side);
    this->comboGalaxyColor->addItem(u"Uniform"_s);
    this->comboGalaxyColor->setEnabled(false);
    this->comboGalaxyColor->setToolTip(
            u"Colour the galaxies by a catalogue attribute (numeric column, or Speed |v| "
            u"from vx/vy/vz) instead of a flat colour."_s);
    overlayForm->addRow(u"Galaxy color"_s, this->comboGalaxyColor);
    this->comboGalaxyShape = new QComboBox(side);
    this->comboGalaxyShape->addItem(u"Points"_s);
    this->comboGalaxyShape->setEnabled(false);
    this->comboGalaxyShape->setToolTip(
            u"Draw galaxies as points, or — when the catalogue carries vx/vy/vz — as "
            u"velocity arrows (each galaxy's peculiar velocity), or — with a field "
            u"loaded — residual arrows (v_galaxy − v_field: the unexplained motion). "
            u"Both arrow modes share one physical scale, so they're comparable."_s);
    overlayForm->addRow(u"Galaxy shape"_s, this->comboGalaxyShape);
    this->chkGalaxyDensity = new QCheckBox(side);
    this->chkGalaxyDensity->setEnabled(false);
    this->chkGalaxyDensity->setToolTip(
            u"Bin the galaxies into a density field and show an overdensity isosurface — "
            u"where large-scale structure (filaments/clusters) sits in the flow."_s);
    overlayForm->addRow(u"Galaxy density"_s, this->chkGalaxyDensity);
    this->sliderDensityLevel = new QSlider(Qt::Horizontal, side);
    this->sliderDensityLevel->setRange(5, 95);
    this->sliderDensityLevel->setValue(30);
    this->sliderDensityLevel->setEnabled(false);
    this->sliderDensityLevel->setToolTip(u"Isosurface level as a percentage of peak density."_s);
    overlayForm->addRow(u"Density level %"_s, this->sliderDensityLevel);
    ovLay->addLayout(overlayForm);

    this->btnBulkFlow = new QPushButton(u"Bulk flow vs radius…"_s, side);
    this->btnBulkFlow->setEnabled(false);
    this->btnBulkFlow->setToolTip(
            u"Plot the bulk-flow amplitude |⟨v⟩| in spheres of growing radius — the "
            u"galaxies vs the reconstructed field. The Watkins–Feldman–Hudson curve "
            u"(its amplitude vs scale tests ΛCDM)."_s);
    ovLay->addWidget(this->btnBulkFlow);
    sl->addWidget(ovGroup);

    sl->addStretch(1);
    // The grouped panel can be taller than the window — make it scroll.
    auto *sideScroll = new QScrollArea(central);
    sideScroll->setWidget(side);
    sideScroll->setWidgetResizable(true);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setMaximumWidth(320);
    sideScroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(sideScroll, 0);
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
        if (auto *lbl = qobject_cast<QLabel *>(structForm->labelForField(this->chkRoi))) {
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
    // valueChanged now fires only on commit (Enter / focus-out / arrow step)
    // because keyboardTracking is off — so this no longer rebuilds per keystroke.
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
    QObject::connect(this->comboBasinDir, &QComboBox::currentIndexChanged, this,
                     [this](int) { if (this->chkBasins && this->chkBasins->isChecked())
                                       this->showBasins(true); });
    QObject::connect(this->chkFieldDensity, &QCheckBox::toggled, this,
                     [this](bool on) { this->showFieldDensity(on); });
    QObject::connect(this->chkFieldVoid, &QCheckBox::toggled, this,
                     [this](bool on) { this->showFieldVoid(on); });
    QObject::connect(this->sliderFieldDensityLevel, &QSlider::valueChanged, this,
                     [this](int) {
                         if (this->chkFieldDensity && this->chkFieldDensity->isChecked())
                             this->updateFieldDensityContour();
                         if (this->chkFieldVoid && this->chkFieldVoid->isChecked())
                             this->updateFieldVoidContour();
                     });
    QObject::connect(this->btnOverlayCatalogue, &QPushButton::clicked, this,
                     [this]() { this->overlayCatalogue(); });
    QObject::connect(this->btnRemoveCatalogue, &QPushButton::clicked, this,
                     [this]() { this->clearCatalogueOverlay(); });
    QObject::connect(this->btnBulkFlow, &QPushButton::clicked, this,
                     [this]() { this->showBulkFlowProfile(); });
    QObject::connect(this->chkShowCatalogue, &QCheckBox::toggled, this,
                     [this](bool on) { this->setCatalogueVisible(on); });
    QObject::connect(this->spinCataloguePointSize, &QDoubleSpinBox::valueChanged, this,
                     [this](double sz) {
                         if (this->catalogueActor) {
                             this->catalogueActor->GetProperty()->SetPointSize(sz);
                             this->renderWindow->Render();
                         }
                     });
    QObject::connect(this->comboGalaxyColor, &QComboBox::currentTextChanged, this,
                     [this](const QString &key) { this->recolourCatalogue(key); });
    QObject::connect(this->comboGalaxyShape, &QComboBox::currentTextChanged, this,
                     [this](const QString &mode) { this->setCatalogueShape(mode); });
    QObject::connect(this->chkGalaxyDensity, &QCheckBox::toggled, this,
                     [this](bool on) { this->showCatalogueDensity(on); });
    QObject::connect(this->sliderDensityLevel, &QSlider::valueChanged, this,
                     [this](int) {
                         if (this->chkGalaxyDensity && this->chkGalaxyDensity->isChecked())
                             this->updateCatalogueDensityContour();
                     });
    QObject::connect(&this->m_catalogueWatcher,
                     &QFutureWatcher<CatalogueOverlayData>::finished, this,
                     [this]() { this->onCatalogueOverlayReady(); });
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
    // The ∇·v density/void depends on the grid geometry (spacing) — invalidate
    // and, if either is showing, recompute at the new scale.
    this->divergenceGrid = nullptr;
    const bool densOn = this->chkFieldDensity && this->chkFieldDensity->isChecked();
    const bool voidOn = this->chkFieldVoid && this->chkFieldVoid->isChecked();
    if (densOn || voidOn) {
        this->buildFieldDensity();
        if (densOn)
            this->updateFieldDensityContour();
        if (voidOn)
            this->updateFieldVoidContour();
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

    // Clear any existing basin surface before (re)fetching, so a failed re-fetch
    // (e.g. switching attraction↔repulsion) never leaves a stale surface behind.
    if (this->basinActor) { this->renderer->RemoveActor(this->basinActor); this->basinActor = nullptr; }

    BackendClient client(m_backendUrl, m_backendToken);
    client.setSessionId(m_sessionId);
    const int direction = (this->comboBasinDir && this->comboBasinDir->currentIndex() == 1) ? -1 : 1;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    this->statusBar()->showMessage(u"Computing gravitational basins…"_s);
    this->m_basins = client.fetchVelocityBasins(m_datasetId, 64, direction);
    QApplication::restoreOverrideCursor();
    if (!m_basins.valid || m_basins.nBasins <= 0) {
        QMessageBox::warning(this, u"Basins"_s,
                             m_basins.valid ? u"No basins found."_s
                                            : u"Basin computation failed: %1"_s.arg(m_basins.error));
        this->m_basins = BackendVelocityBasinsResult{};
        if (this->chkBasins) { QSignalBlocker blk(this->chkBasins); this->chkBasins->setChecked(false); }
        this->renderWindow->Render();
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

    // Per-basin volume (voxel count) → fraction of the assigned (non-zero) volume,
    // sorted largest first, so the user sees which supercluster/void dominates.
    const vtkTypeInt32 *lab = static_cast<const vtkTypeInt32 *>(labels->GetVoidPointer(0));
    std::vector<vtkIdType> sizes(std::size_t(b.nBasins) + 1, 0);
    for (vtkIdType i = 0; i < npts; ++i) {
        const int v = lab[i];
        if (v >= 0 && v <= b.nBasins) ++sizes[std::size_t(v)];
    }
    std::vector<vtkIdType> basinSizes(sizes.begin() + 1, sizes.end());
    const vtkIdType assigned = std::accumulate(basinSizes.begin(), basinSizes.end(), vtkIdType(0));
    std::sort(basinSizes.begin(), basinSizes.end(), std::greater<>());
    QStringList top;
    for (std::size_t i = 0; i < basinSizes.size() && i < 4; ++i) {
        top << u"%1%"_s.arg(100.0 * basinSizes[i] / std::max<vtkIdType>(1, assigned), 0, 'f', 0);
    }
    const QString kind = (this->comboBasinDir && this->comboBasinDir->currentIndex() == 1)
            ? u"repulsion (voids)"_s : u"attraction (superclusters)"_s;
    this->statusBar()->showMessage(
            u"%1 basins of %2 · %3% of volume assigned · largest: %4"_s
                    .arg(b.nBasins).arg(kind)
                    .arg(100.0 * assigned / std::max<vtkIdType>(1, npts), 0, 'f', 0)
                    .arg(top.join(u", "_s)));
}

bool vtkWindowVectorField::buildFieldDensityFromBackend()
{
    if (this->m_datasetId.isEmpty())
        return false;
    BackendClient client(this->m_backendUrl, this->m_backendToken);
    if (!this->m_sessionId.isEmpty())
        client.setSessionId(this->m_sessionId);
    // Density at up to 256³ — always finer than the 128³ display LOD (full-res
    // for fields ≤256³; 256³ for larger), capped to bound the transfer (~64 MB).
    const BackendVelocityDensityResult d = client.fetchVelocityDensity(this->m_datasetId, 256);
    if (!d.valid || d.nx <= 0 || d.ny <= 0 || d.nz <= 0)
        return false;
    const vtkIdType npts = vtkIdType(d.nx) * d.ny * d.nz;
    if (d.divergence.size() != npts * qint64(sizeof(float)))
        return false;

    // Place the density grid at the CLIENT's box geometry (like placeBasins) — the
    // backend values are the divergence *structure*; the isosurface must sit in
    // the display frame, which the user scales client-side via Box size.
    double ox, oy, oz;
    this->originVec(ox, oy, oz);
    const double ex = -2.0 * ox, ey = -2.0 * oy, ez = -2.0 * oz;
    vtkNew<vtkImageData> img;
    img->SetDimensions(d.nx, d.ny, d.nz);
    img->SetSpacing((d.nx > 1) ? ex / (d.nx - 1) : 1.0,
                    (d.ny > 1) ? ey / (d.ny - 1) : 1.0,
                    (d.nz > 1) ? ez / (d.nz - 1) : 1.0);
    img->SetOrigin(ox, oy, oz);
    vtkNew<vtkFloatArray> div;
    div->SetName("Divergence");
    div->SetNumberOfComponents(1);
    div->SetNumberOfTuples(npts);
    std::memcpy(div->GetVoidPointer(0), d.divergence.constData(),
                std::size_t(npts) * sizeof(float));
    img->GetPointData()->SetScalars(div);

    this->divergenceGrid = img;
    double r[2] = { 0.0, 0.0 };
    div->GetRange(r);
    this->m_divMin = r[0];
    this->m_divMax = r[1];
    return true;
}

void vtkWindowVectorField::buildFieldDensity()
{
    if (!this->imageData)
        return;
    // For a downsampled (large) field, the display LOD is too coarse to resolve
    // the ∇·v cores — get a higher-res divergence from the backend. For a
    // full-res display, compute locally (no round-trip).
    if (this->m_grid.downsampleFactor > 1 && !this->m_datasetId.isEmpty()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = this->buildFieldDensityFromBackend();
        QApplication::restoreOverrideCursor();
        if (ok)
            return;
        // else fall through to the local computation
    }
    // ∇·v on the display grid. Converging flow (∇·v < 0) marks overdensities
    // (δ ∝ −∇·v in linear theory); we contour the strongly-convergent cores.
    vtkNew<vtkGradientFilter> grad;
    grad->SetInputData(this->imageData);
    grad->SetInputArrayToProcess(0, 0, 0, vtkDataObject::FIELD_ASSOCIATION_POINTS, "velocity");
    grad->ComputeGradientOff();
    grad->ComputeDivergenceOn();
    grad->SetDivergenceArrayName("Divergence");
    grad->Update();

    auto *out = vtkImageData::SafeDownCast(grad->GetOutput());
    vtkDataArray *div = out ? out->GetPointData()->GetArray("Divergence") : nullptr;
    if (!div) {
        this->divergenceGrid = nullptr;
        this->m_divMin = 0.0;
        this->m_divMax = 0.0;
        return;
    }
    out->GetPointData()->SetScalars(div);   // contour on the divergence scalar
    this->divergenceGrid = out;
    double r[2] = { 0.0, 0.0 };
    div->GetRange(r);
    this->m_divMin = r[0];   // most negative ∇·v = strongest convergence (overdensity)
    this->m_divMax = r[1];   // most positive ∇·v = strongest divergence (void)
}

void vtkWindowVectorField::updateFieldDensityContour()
{
    // Drop any previous surface first, so a rebuild that yields nothing (e.g. a
    // near-uniform divergence field) can't leave a stale actor to be re-shown.
    if (this->divergenceActor) {
        this->renderer->RemoveActor(this->divergenceActor);
        this->divergenceActor = nullptr;
    }
    if (!this->divergenceGrid || this->m_divMin >= 0.0) {
        this->statusBar()->showMessage(
                u"Density (∇·v): no convergent structure in this field."_s, 5000);
        this->renderWindow->Render();
        return;
    }
    const double frac =
            (this->sliderFieldDensityLevel ? this->sliderFieldDensityLevel->value() : 35) / 100.0;
    const double level = frac * this->m_divMin;   // < 0; higher % → tighter, denser core

    vtkNew<vtkFlyingEdges3D> surf;
    surf->SetInputData(this->divergenceGrid);
    surf->SetValue(0, level);
    surf->ComputeNormalsOn();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(surf->GetOutputPort());
    mapper->ScalarVisibilityOff();

    // (the previous actor was already removed + nulled at the top)
    this->divergenceActor = vtkSmartPointer<vtkActor>::New();
    this->divergenceActor->SetMapper(mapper);
    this->divergenceActor->GetProperty()->SetColor(0.95, 0.55, 0.25);  // warm orange
    this->divergenceActor->GetProperty()->SetOpacity(0.30);   // see the flow through it
    this->renderer->AddActor(this->divergenceActor);
    this->renderWindow->Render();
}

void vtkWindowVectorField::showFieldDensity(bool on)
{
    if (!on) {
        if (this->divergenceActor)
            this->divergenceActor->VisibilityOff();
        this->renderWindow->Render();
        return;
    }
    if (!this->imageData)
        return;
    if (!this->divergenceGrid)
        this->buildFieldDensity();
    this->updateFieldDensityContour();
    if (this->divergenceActor)
        this->divergenceActor->VisibilityOn();
}

void vtkWindowVectorField::updateFieldVoidContour()
{
    if (this->voidActor) {
        this->renderer->RemoveActor(this->voidActor);
        this->voidActor = nullptr;
    }
    if (!this->divergenceGrid || this->m_divMax <= 0.0) {
        this->statusBar()->showMessage(
                u"Void (∇·v): no divergent structure in this field."_s, 5000);
        this->renderWindow->Render();
        return;
    }
    const double frac =
            (this->sliderFieldDensityLevel ? this->sliderFieldDensityLevel->value() : 35) / 100.0;
    const double level = frac * this->m_divMax;   // > 0; higher % → tighter void core

    vtkNew<vtkFlyingEdges3D> surf;
    surf->SetInputData(this->divergenceGrid);
    surf->SetValue(0, level);
    surf->ComputeNormalsOn();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(surf->GetOutputPort());
    mapper->ScalarVisibilityOff();

    this->voidActor = vtkSmartPointer<vtkActor>::New();
    this->voidActor->SetMapper(mapper);
    this->voidActor->GetProperty()->SetColor(0.35, 0.55, 0.95);  // cool blue (underdense)
    this->voidActor->GetProperty()->SetOpacity(0.25);
    this->renderer->AddActor(this->voidActor);
    this->renderWindow->Render();
}

void vtkWindowVectorField::showFieldVoid(bool on)
{
    if (!on) {
        if (this->voidActor)
            this->voidActor->VisibilityOff();
        this->renderWindow->Render();
        return;
    }
    if (!this->imageData)
        return;
    if (!this->divergenceGrid)
        this->buildFieldDensity();
    this->updateFieldVoidContour();
    if (this->voidActor)
        this->voidActor->VisibilityOn();
}

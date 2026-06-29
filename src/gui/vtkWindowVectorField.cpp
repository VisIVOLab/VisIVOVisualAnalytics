#include "vtkWindowVectorField.h"

#include "ColorMaps.h"
#include "ColormapIcon.h"

#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkArrowSource.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkFloatArray.h>
#include <vtkGlyph3D.h>
#include <vtkImageData.h>
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

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

vtkWindowVectorField::vtkWindowVectorField(const QString &path,
                                           BackendVelocityGridResult grid, QWidget *parent)
    : QMainWindow(parent), m_path(path), m_grid(std::move(grid))
{
    this->setWindowTitle(u"Velocity Field — %1"_s.arg(QFileInfo(path).fileName()));
    this->lut = vtkSmartPointer<vtkLookupTable>::New();
    this->axesWidget = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
    this->scalarBar = vtkSmartPointer<vtkScalarBarActor>::New();

    this->setupUi();
    this->setupRenderer();
    this->buildImageData();
    this->applyColorMap(u"Inferno"_s);
    this->rebuildGlyphs();
    this->rebuildStreamlines();
    this->resetCamera();
}

void vtkWindowVectorField::setupUi()
{
    auto *central = new QWidget(this);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);

    this->vtkWidget = new QVTKOpenGLNativeWidget(central);
    this->vtkWidget->setMinimumSize(700, 520);
    root->addWidget(this->vtkWidget, 1);

    // ── Sidebar ───────────────────────────────────────────────────────────────
    auto *side = new QFrame(central);
    side->setFrameShape(QFrame::StyledPanel);
    side->setMaximumWidth(280);
    auto *sl = new QVBoxLayout(side);

    this->summaryLabel = new QLabel(side);
    this->summaryLabel->setWordWrap(true);
    this->summaryLabel->setText(
            u"Grid %1×%2×%3 · speed %4–%5 %6"_s.arg(m_grid.nx).arg(m_grid.ny).arg(m_grid.nz)
                    .arg(m_grid.speedMin, 0, 'g', 3).arg(m_grid.speedMax, 0, 'g', 3)
                    .arg(m_grid.unit));
    sl->addWidget(this->summaryLabel);

    auto *form = new QFormLayout;

    this->chkGlyphs = new QCheckBox(side);
    this->chkGlyphs->setChecked(true);
    form->addRow(u"Arrows"_s, this->chkGlyphs);

    this->chkStreamlines = new QCheckBox(side);
    this->chkStreamlines->setChecked(true);
    form->addRow(u"Streamlines"_s, this->chkStreamlines);

    this->spinStride = new QSpinBox(side);
    this->spinStride->setRange(1, 64);
    this->spinStride->setValue(4);
    this->spinStride->setToolTip(u"Show one arrow every N grid points"_s);
    form->addRow(u"Arrow stride"_s, this->spinStride);

    this->spinGlyphScale = new QDoubleSpinBox(side);
    this->spinGlyphScale->setRange(0.0, 100.0);
    this->spinGlyphScale->setDecimals(3);
    this->spinGlyphScale->setSingleStep(0.05);
    this->spinGlyphScale->setValue(1.0);
    form->addRow(u"Arrow scale"_s, this->spinGlyphScale);

    this->spinSeeds = new QSpinBox(side);
    this->spinSeeds->setRange(10, 5000);
    this->spinSeeds->setValue(400);
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

    sl->addLayout(form);
    sl->addStretch(1);
    root->addWidget(side, 0);
    this->setCentralWidget(central);

    // ── Connections ─────────────────────────────────────────────────────────
    QObject::connect(this->chkGlyphs, &QCheckBox::toggled, this, [this](bool on) {
        if (this->glyphActor) this->glyphActor->SetVisibility(on);
        this->renderWindow->Render();
    });
    QObject::connect(this->chkStreamlines, &QCheckBox::toggled, this, [this](bool on) {
        if (this->streamActor) this->streamActor->SetVisibility(on);
        this->renderWindow->Render();
    });
    QObject::connect(this->spinStride, &QSpinBox::valueChanged, this,
                     [this](int) { this->rebuildGlyphs(); this->renderWindow->Render(); });
    QObject::connect(this->spinGlyphScale, &QDoubleSpinBox::valueChanged, this,
                     [this](double) { this->rebuildGlyphs(); this->renderWindow->Render(); });
    QObject::connect(this->spinSeeds, &QSpinBox::valueChanged, this,
                     [this](int) { this->rebuildStreamlines(); this->renderWindow->Render(); });
    QObject::connect(this->spinBoxSize, &QDoubleSpinBox::valueChanged, this,
                     [this](double mpc) { this->applyBoxSize(mpc); });
    QObject::connect(this->comboLut, &QComboBox::currentTextChanged, this,
                     [this](const QString &name) { this->applyColorMap(name);
                                                   this->renderWindow->Render(); });
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

void vtkWindowVectorField::buildImageData()
{
    const int nx = m_grid.nx, ny = m_grid.ny, nz = m_grid.nz;
    const vtkIdType npts = vtkIdType(nx) * ny * nz;

    this->imageData = vtkSmartPointer<vtkImageData>::New();
    this->imageData->SetDimensions(nx, ny, nz);
    this->imageData->SetSpacing(m_grid.sx, m_grid.sy, m_grid.sz);
    this->imageData->SetOrigin(m_grid.ox, m_grid.oy, m_grid.oz);

    const float *src = reinterpret_cast<const float *>(m_grid.vectors.constData());

    vtkNew<vtkFloatArray> vectors;
    vectors->SetName("velocity");
    vectors->SetNumberOfComponents(3);
    vectors->SetNumberOfTuples(npts);
    std::memcpy(vectors->GetVoidPointer(0), src, std::size_t(npts) * 3 * sizeof(float));

    vtkNew<vtkFloatArray> speed;
    speed->SetName("speed");
    speed->SetNumberOfComponents(1);
    speed->SetNumberOfTuples(npts);
    for (vtkIdType i = 0; i < npts; ++i) {
        const float vx = src[3 * i], vy = src[3 * i + 1], vz = src[3 * i + 2];
        speed->SetValue(i, std::sqrt(vx * vx + vy * vy + vz * vz));
    }

    this->imageData->GetPointData()->SetVectors(vectors);
    this->imageData->GetPointData()->SetScalars(speed);
}

void vtkWindowVectorField::rebuildGlyphs()
{
    if (!this->imageData) return;
    if (this->glyphActor) this->renderer->RemoveActor(this->glyphActor);

    vtkNew<vtkMaskPoints> mask;
    mask->SetInputData(this->imageData);
    mask->SetOnRatio(std::max(1, this->spinStride ? this->spinStride->value() : 4));
    mask->RandomModeOff();

    vtkNew<vtkArrowSource> arrow;

    // Scale factor: a fraction of the cell size divided by the speed range so the
    // longest arrows span ~a few cells; multiplied by the user scale.
    double cell = std::min({ m_grid.sx, m_grid.sy, m_grid.sz });
    if (this->m_boxMpc > 0.0) cell = m_boxMpc / std::max(1, std::max({ m_grid.nx, m_grid.ny, m_grid.nz }));
    const double speedMax = std::max(1e-12, m_grid.speedMax);
    const double userScale = this->spinGlyphScale ? this->spinGlyphScale->value() : 1.0;
    const double scaleFactor = userScale * 3.0 * cell / speedMax;

    vtkNew<vtkGlyph3D> glyph;
    glyph->SetInputConnection(mask->GetOutputPort());
    glyph->SetSourceConnection(arrow->GetOutputPort());
    glyph->SetVectorModeToUseVector();
    glyph->SetScaleModeToScaleByVector();
    glyph->SetScaleFactor(scaleFactor);
    glyph->SetColorModeToColorByScalar();
    glyph->OrientOn();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(glyph->GetOutputPort());
    mapper->SetLookupTable(this->lut);
    mapper->SetScalarRange(m_grid.speedMin, m_grid.speedMax);
    mapper->SetScalarModeToUsePointData();

    this->glyphActor = vtkSmartPointer<vtkActor>::New();
    this->glyphActor->SetMapper(mapper);
    this->glyphActor->SetVisibility(this->chkGlyphs ? this->chkGlyphs->isChecked() : true);
    this->renderer->AddActor(this->glyphActor);
}

void vtkWindowVectorField::rebuildStreamlines()
{
    if (!this->imageData) return;
    if (this->streamActor) this->renderer->RemoveActor(this->streamActor);

    double bounds[6];
    this->imageData->GetBounds(bounds);
    const double cx = 0.5 * (bounds[0] + bounds[1]);
    const double cy = 0.5 * (bounds[2] + bounds[3]);
    const double cz = 0.5 * (bounds[4] + bounds[5]);
    const double ext = std::max({ bounds[1] - bounds[0], bounds[3] - bounds[2],
                                  bounds[5] - bounds[4] });

    vtkNew<vtkPointSource> seeds;
    seeds->SetCenter(cx, cy, cz);
    seeds->SetRadius(0.3 * ext);
    seeds->SetNumberOfPoints(this->spinSeeds ? this->spinSeeds->value() : 400);

    vtkNew<vtkStreamTracer> tracer;
    tracer->SetInputData(this->imageData);
    tracer->SetSourceConnection(seeds->GetOutputPort());
    tracer->SetIntegratorTypeToRungeKutta45();
    tracer->SetIntegrationDirectionToBoth();
    tracer->SetMaximumPropagation(4.0 * ext);

    vtkNew<vtkTubeFilter> tube;
    tube->SetInputConnection(tracer->GetOutputPort());
    tube->SetRadius(0.0025 * ext);
    tube->SetNumberOfSides(6);

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(tube->GetOutputPort());
    mapper->SetLookupTable(this->lut);
    mapper->SetScalarRange(m_grid.speedMin, m_grid.speedMax);
    mapper->SelectColorArray("speed");
    mapper->SetScalarModeToUsePointFieldData();

    this->streamActor = vtkSmartPointer<vtkActor>::New();
    this->streamActor->SetMapper(mapper);
    this->streamActor->SetVisibility(this->chkStreamlines ? this->chkStreamlines->isChecked()
                                                          : true);
    this->renderer->AddActor(this->streamActor);
}

void vtkWindowVectorField::applyBoxSize(double boxMpc)
{
    this->m_boxMpc = boxMpc;
    if (!this->imageData) return;
    const int n = std::max(1, std::max({ m_grid.nx, m_grid.ny, m_grid.nz }));
    if (boxMpc > 0.0) {
        const double s = boxMpc / n;
        this->imageData->SetSpacing(s, s, s);
        this->imageData->SetOrigin(-0.5 * boxMpc, -0.5 * boxMpc, -0.5 * boxMpc);
    } else {
        this->imageData->SetSpacing(m_grid.sx, m_grid.sy, m_grid.sz);
        this->imageData->SetOrigin(m_grid.ox, m_grid.oy, m_grid.oz);
    }
    this->rebuildGlyphs();
    this->rebuildStreamlines();
    this->resetCamera();
    this->renderWindow->Render();
}

void vtkWindowVectorField::applyColorMap(const QString &name)
{
    ColorMaps::SetColorMap(this->lut, name.toStdString());
    this->lut->SetTableRange(m_grid.speedMin, m_grid.speedMax);
    this->lut->Build();
}

void vtkWindowVectorField::resetCamera()
{
    this->renderer->ResetCamera();
    if (this->renderWindow->GetInteractor()) {
        this->renderWindow->Render();
    }
}

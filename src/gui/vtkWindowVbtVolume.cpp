#include "vtkWindowVbtVolume.h"
#include "app/BackendClient.h"
#include "app/ThemeManager.h"
#include "DiagnosticsWindow.h"
#include "theme/VisivoTheme.h"
#include <QFileInfo>
#include <QHeaderView>
#include <QTableWidget>
#include <QDialogButtonBox>
#include <QDialog>
#include "WorkspaceChrome.h"
#include "InspectorPanel.h"
#include <QDockWidget>
#include <QToolButton>

#include <QApplication>
#include <QKeySequence>
#include <QMetaObject>

#include <QVTKOpenGLNativeWidget.h>

#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCubeAxesActor.h>
#include <vtkProperty.h>
#include <vtkImageData.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkPointData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkScalarBarActor.h>
#include <vtkTextProperty.h>

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include "AssistantPanel.h"
#include "CopilotActionRouter.h"
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <QSysInfo>
#include <array>
#include <cstring>
#include <limits>

using namespace Qt::StringLiterals;

double vtkWindowVbtVolume::percentileFromSortedSample(const std::vector<double> &values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const double scaled = clamped * static_cast<double>(values.size() - 1);
    const std::size_t low = static_cast<std::size_t>(std::floor(scaled));
    const std::size_t high = static_cast<std::size_t>(std::ceil(scaled));
    if (low == high) {
        return values[low];
    }
    const double t = scaled - static_cast<double>(low);
    return values[low] * (1.0 - t) + values[high] * t;
}

vtkWindowVbtVolume::vtkWindowVbtVolume(BackendVbtOpenResult openResult,
                                       BackendVbtSubsetResult initialSubset,
                                       const QString &backendUrl,
                                       const QString &backendToken,
                                       const QString &sessionId,
                                       const QString &headerPath,
                                       QWidget *parent)
    : QMainWindow(parent)
    , openResult(std::move(openResult))
    , m_headerPath(headerPath)
    , backendUrl(backendUrl)
    , backendToken(backendToken)
    , sessionId(sessionId)
{
    qDebug().noquote()
            << QStringLiteral("[vbt-volume] type=%1 fields=%2 dims=%3x%4x%5 spacing=%6,%7,%8 selected_field=%9")
                       .arg(this->openResult.scalarType)
                       .arg(this->openResult.fields.size())
                       .arg(this->openResult.dimensions[0])
                       .arg(this->openResult.dimensions[1])
                       .arg(this->openResult.dimensions[2])
                       .arg(this->openResult.spacing[0], 0, 'g', 6)
                       .arg(this->openResult.spacing[1], 0, 'g', 6)
                       .arg(this->openResult.spacing[2], 0, 'g', 6)
                       .arg(this->openResult.fields.empty() ? QStringLiteral("<none>")
                                                            : this->openResult.fields[0].name);
    this->setupUi();
    this->setupViewerChrome();
    this->setupRenderer();
    // Apply the initial subset received at open time without an extra round-trip.
    // rebuildVolumeData() will contact the backend on subsequent field changes.
    this->applySubsetResult(std::move(initialSubset));
    this->updateSummary();
}

void vtkWindowVbtVolume::setupUi()
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(QStringLiteral("VBT Volume Viewer, %1 x %2 x %3")
                                 .arg(this->openResult.dimensions[0])
                                 .arg(this->openResult.dimensions[1])
                                 .arg(this->openResult.dimensions[2]));
    this->resize(1280, 800);

    // ── Central widget: sidebar + VTK ─────────────────────────────────────────
    auto *central = new QWidget(this);
    this->setCentralWidget(central);
    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setSpacing(0);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    this->setupSidebarUi();

    this->vtkWidget = new QVTKOpenGLNativeWidget(central);
    this->vtkWidget->setMinimumSize(700, 500);
    rootLayout->addWidget(this->vtkWidget, 1);

    // ── Signal connections ────────────────────────────────────────────────────
    QObject::connect(this->comboScalarField, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->rebuildVolumeData(); });
    QObject::connect(this->comboBlendMode, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateTransferFunctions(); });
    QObject::connect(this->sliderOpacity, &QSlider::valueChanged, this,
                     [this](int) { this->updateTransferFunctions(); });
    QObject::connect(this->comboScaleMode, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateTransferFunctions(); });
    QObject::connect(this->spinScaleGamma, &QDoubleSpinBox::valueChanged, this,
                     [this](double) { this->updateTransferFunctions(); });
    QObject::connect(this->buttonAutoscale, &QPushButton::clicked, this, [this]() {
        // The SAME range the field is loaded with, not the raw extremes.
        // Autoscale used to set min..max, and on a simulation volume — where a
        // handful of voxels are orders of magnitude above the rest — that put
        // essentially every voxel at the bottom of the colour and opacity ramps
        // and the volume disappeared (user report: "autoscale fa sparire
        // tutto", on a 512³ CDM density field spanning 5e-12 … 4e-4). The raw
        // extremes are still one edit away in the two fields above.
        this->applyAutoColorRange();
        this->updateTransferFunctions();
    });
    QObject::connect(this->comboBackground, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->applyBackground(); });
    QObject::connect(this->spinRangeMin, &QDoubleSpinBox::valueChanged, this, [this](double) {
        if (this->updatingRangeControls) {
            return;
        }
        this->setColorRange(this->spinRangeMin->value(), this->spinRangeMax->value());
        this->updateTransferFunctions();
    });
    QObject::connect(this->spinRangeMax, &QDoubleSpinBox::valueChanged, this, [this](double) {
        if (this->updatingRangeControls) {
            return;
        }
        this->setColorRange(this->spinRangeMin->value(), this->spinRangeMax->value());
        this->updateTransferFunctions();
    });

    // ── View menu ─────────────────────────────────────────────────────────────
    auto *viewMenu = this->menuBar()->addMenu(u"View"_s);
    this->m_actionSessionPanel = new QAction(u"Session Data Panel"_s, this);
    this->m_actionSessionPanel->setCheckable(true);
    this->m_actionSessionPanel->setChecked(true);
    viewMenu->addAction(this->m_actionSessionPanel);
    QObject::connect(this->m_actionSessionPanel, &QAction::toggled, this, [this](bool checked) {
        if (this->m_workspaceChrome)
            this->m_workspaceChrome->setSessionCollapsed(!checked);
    });
    this->actionShowControlsSidebar = new QAction(u"Inspector Panel"_s, this);
    this->actionShowControlsSidebar->setCheckable(true);
    this->actionShowControlsSidebar->setChecked(true);
    viewMenu->addAction(this->actionShowControlsSidebar);
    QObject::connect(this->actionShowControlsSidebar, &QAction::toggled, this,
                     [this](bool checked) {
                         if (this->m_workspaceChrome)
                             this->m_workspaceChrome->setInspectorCollapsed(!checked);
                     });

    // ── Diagnostics + Command Palette (replicated across windows) ────────────
    viewMenu->addSeparator();
    auto *actionDiagnostics = new QAction(u"Diagnostics"_s, this);
    actionDiagnostics->setToolTip(u"Open the application-wide diagnostics log window."_s);
    viewMenu->addAction(actionDiagnostics);
    QObject::connect(actionDiagnostics, &QAction::triggered, this,
                     []() { DiagnosticsWindow::showSingleton(); });

    auto *actionCommandPalette = new QAction(u"Command Palette…"_s, this);
    actionCommandPalette->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    actionCommandPalette->setShortcutContext(Qt::ApplicationShortcut);
    viewMenu->addAction(actionCommandPalette);
    QObject::connect(actionCommandPalette, &QAction::triggered, this, [this]() {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->inherits("MainWindow")) {
                QMetaObject::invokeMethod(w, "openCommandPalette", Qt::QueuedConnection);
                return;
            }
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Sidebar implementation
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Fill a form layout with muted key / mono value rows — the same pairing the
/// image and cube viewers use for their info grids. A five-line label in one
/// colour is not readable at a glance the way aligned pairs are, and it was the
/// only place in the application that presented dataset facts that way.
void setInfoRows(QFormLayout *grid, const QList<QPair<QString, QString>> &rows)
{
    if (!grid)
        return;
    while (grid->rowCount() > 0)
        grid->removeRow(0);
    for (const auto &row : rows) {
        auto *key = new QLabel(row.first);
        key->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
        auto *value = new QLabel(row.second);
        value->setStyleSheet(VisivoTheme::infoValueStyleSheet());
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        grid->addRow(key, value);
    }
}

}   // namespace

void vtkWindowVbtVolume::setupSidebarUi()
{
    // ── Shared style helpers ──────────────────────────────────────────────────
    // Card scopes to QFrame#card so only volume cards pick up the tint.
    const QLatin1StringView cardTint = VisivoTheme::isDark()
            ? "rgba(255,255,255,0.04)"_L1
            : "rgba(0,0,0,0.04)"_L1;
    const QLatin1StringView cardBorder = VisivoTheme::isDark()
            ? "rgba(255,255,255,0.06)"_L1
            : "rgba(0,0,0,0.10)"_L1;
    const QString cardStyle =
            QStringLiteral("QFrame#card { background: %1; border-radius: 6px;"
                           "  border: 1px solid %2; }")
                    .arg(cardTint, cardBorder);
    const QString hdrStyle =
            QStringLiteral("color: %1; font-size: 10px; font-weight: 600;"
                           " padding-bottom: 4px; border-bottom: 1px solid rgba(4,138,191,0.20);")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

    const auto makeCard = [&](const QString &title, QWidget *content) -> QFrame* {
        auto *card = new QFrame;
        card->setObjectName(u"card"_s);
        card->setStyleSheet(cardStyle);
        auto *vl = new QVBoxLayout(card);
        vl->setSpacing(6);
        vl->setContentsMargins(10, 8, 10, 10);
        auto *lbl = new QLabel(title);
        lbl->setStyleSheet(hdrStyle);
        vl->addWidget(lbl);
        vl->addWidget(content);
        return card;
    };

    // ── Page 0: Volume controls ───────────────────────────────────────────────
    auto *pageVolume = new QWidget;
    auto *pageVolumeLayout = new QVBoxLayout(pageVolume);
    pageVolumeLayout->setContentsMargins(8, 8, 8, 8);
    pageVolumeLayout->setSpacing(8);

    // Card: Source
    {
        auto *inner = new QWidget;
        auto *fl = new QFormLayout(inner);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);

        this->comboScalarField = new QComboBox(inner);
        for (const auto &field : this->openResult.fields)
            this->comboScalarField->addItem(field.name, field.name);

        this->comboBlendMode = new QComboBox(inner);
        this->comboBlendMode->addItems(
                { u"Composite"_s, u"MIP (Max Intensity)"_s, u"MinIP (Min Intensity)"_s });
        this->comboBlendMode->setToolTip(
                u"Composite: full volume ray-casting with opacity transfer function.\n"
                u"MIP: show the maximum scalar value along each ray (ignores opacity TF).\n"
                u"MinIP: show the minimum scalar value along each ray."_s);

        this->comboScaleMode = new QComboBox(inner);
        for (const char *label : LutScale::modeLabels()) {
            this->comboScaleMode->addItem(QString::fromUtf8(label));
        }

        this->spinScaleGamma = new QDoubleSpinBox(inner);
        this->spinScaleGamma->setRange(0.1, 10.0);
        this->spinScaleGamma->setSingleStep(0.1);
        this->spinScaleGamma->setDecimals(2);
        this->spinScaleGamma->setValue(2.0);
        this->spinScaleGamma->setToolTip(u"Exponent γ for the Power stretch (t^γ)."_s);
        this->spinScaleGamma->setEnabled(false);  // enabled only in Power mode

        fl->addRow(u"Scalar"_s, this->comboScalarField);
        fl->addRow(u"Mode"_s, this->comboBlendMode);
        fl->addRow(u"Scale"_s, this->comboScaleMode);
        fl->addRow(u"Power γ"_s, this->spinScaleGamma);
        pageVolumeLayout->addWidget(makeCard(u"SOURCE"_s, inner));
    }

    // Card: Color / Range
    {
        auto *inner = new QWidget;
        auto *fl = new QFormLayout(inner);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);

        this->spinRangeMin = new QDoubleSpinBox(inner);
        this->spinRangeMax = new QDoubleSpinBox(inner);
        this->spinRangeMin->setDecimals(12);
        this->spinRangeMax->setDecimals(12);
        this->spinRangeMin->setRange(-1e30, 1e30);
        this->spinRangeMax->setRange(-1e30, 1e30);
        this->buttonAutoscale = new QPushButton(u"Autoscale"_s, inner);
        this->buttonAutoscale->setToolTip(
                u"Reset the colour range to the 2nd–99.9th percentile of the data — "
                u"what the viewer opens with. The raw minimum and maximum are rarely "
                u"a useful range for a density field."_s);

        fl->addRow(u"Range min"_s, this->spinRangeMin);
        fl->addRow(u"Range max"_s, this->spinRangeMax);
        fl->addRow(this->buttonAutoscale);
        pageVolumeLayout->addWidget(makeCard(u"COLOR RANGE"_s, inner));
    }

    // Card: Rendering
    {
        auto *inner = new QWidget;
        auto *fl = new QFormLayout(inner);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);

        this->sliderOpacity = new QSlider(Qt::Horizontal, inner);
        this->sliderOpacity->setRange(1, 100);
        this->sliderOpacity->setValue(28);
        this->sliderOpacity->setToolTip(
                u"Composite: scales the opacity ramp, higher = more opaque.\n"
                u"MIP/MinIP: raises the threshold above which voxels are projection candidates."_s);

        this->comboBackground = new QComboBox(inner);
        this->comboBackground->addItems({ u"Black"_s, u"Dark gray"_s, u"White"_s });

        fl->addRow(u"Opacity"_s, this->sliderOpacity);
        fl->addRow(u"Background"_s, this->comboBackground);
        pageVolumeLayout->addWidget(makeCard(u"RENDERING"_s, inner));
    }

    // No SUMMARY card here any more. It described the DATASET — dimensions,
    // spacing, fields, scalar type — in the column that holds the CONTROLS, and
    // said the same things the Inspector's Properties tab says two panels away
    // (user report). What is on screen belongs on the right; what changes what
    // is on screen belongs here. The one row that was not a duplicate, the
    // active scalar, is a live row in Properties now.
    pageVolumeLayout->addStretch(1);

    // ── Page 1: Info ──────────────────────────────────────────────────────────
    auto *pageInfo = new QWidget;
    auto *pageInfoLayout = new QVBoxLayout(pageInfo);
    pageInfoLayout->setContentsMargins(8, 8, 8, 8);
    pageInfoLayout->setSpacing(8);

    {
        auto *inner = new QWidget;
        auto *vl = new QVBoxLayout(inner);
        vl->setContentsMargins(0, 0, 0, 0);
        this->metadataGrid = new QFormLayout;
        this->metadataGrid->setContentsMargins(0, 0, 0, 0);
        this->metadataGrid->setSpacing(4);
        this->metadataGrid->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        vl->addLayout(this->metadataGrid);
        pageInfoLayout->addWidget(makeCard(u"DATASET INFO"_s, inner));
    }
    {
        auto *inner = new QWidget;
        auto *vl = new QVBoxLayout(inner);
        vl->setContentsMargins(0, 0, 0, 0);
        this->fieldList = new QListWidget(inner);
        for (const auto &field : this->openResult.fields)
            this->fieldList->addItem(QStringLiteral("%1 [%2]").arg(field.name, field.type));
        vl->addWidget(this->fieldList);
        pageInfoLayout->addWidget(makeCard(u"FIELDS"_s, inner));
    }
    pageInfoLayout->addStretch(1);

    // ── Scientific copilot page ───────────────────────────────────────────────
    // The pages go into the two docks, not into a rail of this window's own —
    // see setupWorkspaceDocks(), called right after this from the constructor.
    this->m_pageVolume = pageVolume;
    this->m_pageInfo = pageInfo;
    pageVolume->setParent(this);
    pageVolume->hide();
    pageInfo->setParent(this);
    pageInfo->hide();
}

void vtkWindowVbtVolume::setupRenderer()
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

    this->volumeMapper->SetBlendModeToComposite();
    this->volumeMapper->SetRequestedRenderModeToGPU();
    this->volumeProperty->ShadeOff();
    this->volumeProperty->SetInterpolationTypeToLinear();
    this->volumeProperty->SetColor(this->colorTransfer.Get());
    this->volumeProperty->SetScalarOpacity(this->opacityTransfer.Get());
    this->volumeActor->SetMapper(this->volumeMapper.Get());
    this->volumeActor->SetProperty(this->volumeProperty.Get());
    this->renderer->AddVolume(this->volumeActor.Get());

    this->boxActor->SetCamera(this->renderer->GetActiveCamera());
    this->boxActor->SetXTitle("X");
    this->boxActor->SetYTitle("Y");
    this->boxActor->SetZTitle("Z");
    this->boxActor->SetFlyModeToOuterEdges();
    this->boxActor->DrawXGridlinesOff();
    this->boxActor->DrawYGridlinesOff();
    this->boxActor->DrawZGridlinesOff();
    this->applyBoxActorColors(true); // default: dark background
    this->boxActor->SetVisibility(1);
    this->renderer->AddActor(this->boxActor.Get());

    this->scalarBar->SetLookupTable(this->colorTransfer.Get());
    this->scalarBar->SetMaximumWidthInPixels(100);
    this->scalarBar->SetPosition(0.88, 0.1);
    this->scalarBar->GetTitleTextProperty()->SetColor(1.0, 1.0, 1.0);
    this->scalarBar->GetLabelTextProperty()->SetColor(1.0, 1.0, 1.0);
    this->scalarBar->SetVisibility(1);
    this->renderer->AddViewProp(this->scalarBar.Get());

    // Loading placeholder — visible until applySubsetResult() finishes.
    {
        auto *prop = this->m_loadingActor->GetTextProperty();
        prop->SetFontSize(16);
        double r, g, b;
        ThemeManager::instance().vtkTextColor(r, g, b);
        prop->SetColor(r, g, b);
        prop->SetJustificationToCentered();
        prop->SetVerticalJustificationToCentered();
        prop->BoldOff();
        prop->ShadowOff();
        this->m_loadingActor->SetInput("Loading\xe2\x80\xa6");
        this->m_loadingActor->GetPositionCoordinate()
                ->SetCoordinateSystemToNormalizedViewport();
        this->m_loadingActor->SetPosition(0.5, 0.5);
        this->m_loadingActor->VisibilityOn();
        this->renderer->AddActor2D(this->m_loadingActor.Get());

        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                         this, [this](EffectiveTheme) {
                             double tr, tg, tb;
                             ThemeManager::instance().vtkTextColor(tr, tg, tb);
                             this->m_loadingActor->GetTextProperty()->SetColor(tr, tg, tb);
                         });
    }

    qDebug().noquote()
            << QStringLiteral("[vbt-volume] renderer setup mapper=%1 volume_actor=1 box_actor=1 scalar_bar=1 props=%2")
                       .arg(this->volumeMapper->GetClassName())
                       .arg(this->renderer->GetViewProps()->GetNumberOfItems());
}

void vtkWindowVbtVolume::rebuildVolumeData()
{
    const QString fieldName = this->comboScalarField ? this->comboScalarField->currentData().toString()
                                                     : QString();
    BackendClient client(this->backendUrl, this->backendToken);
    client.setSessionId(this->sessionId);
    const BackendVbtSubsetResult subset =
            client.requestVbtSubset(this->openResult.datasetId, 0, fieldName);
    if (!subset.valid) {
        qWarning().noquote() << QStringLiteral("[vbt-volume] subset request failed: %1").arg(subset.error);
        return;
    }
    this->applySubsetResult(subset);
}

void vtkWindowVbtVolume::applySubsetResult(const BackendVbtSubsetResult &subset)
{
    const auto &dims = this->openResult.dimensions;
    const auto &spacing = this->openResult.spacing;
    const std::size_t voxelCount = static_cast<std::size_t>(this->openResult.numRows);
    // Backend always sends float32-LE; on BE hosts byte-swap each value.
    const bool needsSwap = (QSysInfo::ByteOrder == QSysInfo::BigEndian);

    this->currentImageData = vtkSmartPointer<vtkImageData>::New();
    this->currentImageData->SetDimensions(dims[0], dims[1], dims[2]);
    this->currentImageData->SetSpacing(spacing[0], spacing[1], spacing[2]);
    this->currentImageData->SetOrigin(0.0, 0.0, 0.0);
    this->currentImageData->AllocateScalars(VTK_FLOAT, 1);

    auto *scalars = static_cast<float *>(this->currentImageData->GetScalarPointer());
    const float *src = reinterpret_cast<const float *>(subset.block.constData());
    const std::size_t blockFloats =
            static_cast<std::size_t>(subset.block.size()) / sizeof(float);
    const std::size_t count = std::min(voxelCount, blockFloats);

    double minValue = std::numeric_limits<double>::max();
    double maxValue = std::numeric_limits<double>::lowest();
    constexpr std::size_t targetSampleCount = 200000;
    const std::size_t sampleStride = std::max<std::size_t>(1, count / targetSampleCount);
    this->cachedSampledValues.clear();
    this->cachedPositiveSampledValues.clear();
    this->cachedSampledValues.reserve(std::min(count, targetSampleCount));
    this->cachedPositiveSampledValues.reserve(std::min(count, targetSampleCount));

    for (std::size_t i = 0; i < count; ++i) {
        float v = src[i];
        if (needsSwap) {
            auto *b = reinterpret_cast<unsigned char *>(&v);
            std::reverse(b, b + sizeof(float));
        }
        scalars[i] = v;
        const double value = static_cast<double>(v);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        if ((i % sampleStride) == 0 && std::isfinite(value)) {
            this->cachedSampledValues.push_back(value);
            if (value > 0.0) {
                this->cachedPositiveSampledValues.push_back(value);
            }
        }
    }

    if (!(minValue < maxValue)) {
        maxValue = minValue + 1.0;
    }

    this->volumeProperty->SetScalarOpacityUnitDistance(
            std::max(1e-6, (spacing[0] + spacing[1] + spacing[2]) / 3.0));

    this->volumeMapper->SetInputData(this->currentImageData);
    this->updateBoundsContext();
    std::sort(this->cachedSampledValues.begin(), this->cachedSampledValues.end());
    std::sort(this->cachedPositiveSampledValues.begin(), this->cachedPositiveSampledValues.end());
    this->dataScalarMin = minValue;
    this->dataScalarMax = maxValue;
    this->cachedPositiveFloor = this->positiveFloor();
    if (this->cachedPositiveSampledValues.size() >= 16) {
        const double opacityLow = percentileFromSortedSample(this->cachedPositiveSampledValues, 0.92);
        const double opacityMidLow = percentileFromSortedSample(this->cachedPositiveSampledValues, 0.975);
        const double opacityMidHigh = percentileFromSortedSample(this->cachedPositiveSampledValues, 0.992);
        const double opacityHigh = percentileFromSortedSample(this->cachedPositiveSampledValues, 0.999);
        this->cachedOpacityLow = std::max(this->cachedPositiveFloor, opacityLow);
        this->cachedOpacityMidLow = std::max(this->cachedOpacityLow, opacityMidLow);
        this->cachedOpacityMidHigh = std::max(this->cachedOpacityMidLow, opacityMidHigh);
        this->cachedOpacityHigh = std::max(this->cachedOpacityMidHigh, opacityHigh);
    } else {
        const double positiveMax = this->cachedPositiveSampledValues.empty()
                ? std::max(this->cachedPositiveFloor * 10.0, maxValue)
                : this->cachedPositiveSampledValues.back();
        this->cachedOpacityLow = this->cachedPositiveFloor;
        this->cachedOpacityMidLow = this->cachedPositiveFloor * 10.0;
        this->cachedOpacityMidHigh = std::sqrt(this->cachedPositiveFloor * positiveMax);
        this->cachedOpacityHigh = positiveMax;
    }
    this->applyAutoColorRange();

    double imageRange[2]{ 0.0, 0.0 };
    this->currentImageData->GetScalarRange(imageRange);
    double bounds[6]{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    this->currentImageData->GetBounds(bounds);
    qDebug().noquote()
            << QStringLiteral("[vbt-volume] field=%1 dims=%2x%3x%4 spacing=%5,%6,%7 image_range=%8..%9 sampled=%10 positive_sampled=%11")
                       .arg(subset.fieldName)
                       .arg(dims[0]).arg(dims[1]).arg(dims[2])
                       .arg(spacing[0], 0, 'g', 6)
                       .arg(spacing[1], 0, 'g', 6)
                       .arg(spacing[2], 0, 'g', 6)
                       .arg(imageRange[0], 0, 'g', 8)
                       .arg(imageRange[1], 0, 'g', 8)
                       .arg(this->cachedSampledValues.size())
                       .arg(this->cachedPositiveSampledValues.size());

    auto *props = this->renderer->GetViewProps();
    const double *visibleBounds = this->renderer->ComputeVisiblePropBounds();
    const double *position = this->renderer->GetActiveCamera()->GetPosition();
    const double *focalPoint = this->renderer->GetActiveCamera()->GetFocalPoint();
    double clippingRange[2]{ 0.0, 0.0 };
    this->renderer->GetActiveCamera()->GetClippingRange(clippingRange);
    qDebug().noquote()
            << QStringLiteral("[vbt-volume] renderer props=%1 visible_bounds=[%2,%3]x[%4,%5]x[%6,%7] camera_pos=%8,%9,%10 focal=%11,%12,%13 clip=%14,%15")
                       .arg(props ? props->GetNumberOfItems() : 0)
                       .arg(visibleBounds ? visibleBounds[0] : 0.0, 0, 'g', 8)
                       .arg(visibleBounds ? visibleBounds[1] : 0.0, 0, 'g', 8)
                       .arg(visibleBounds ? visibleBounds[2] : 0.0, 0, 'g', 8)
                       .arg(visibleBounds ? visibleBounds[3] : 0.0, 0, 'g', 8)
                       .arg(visibleBounds ? visibleBounds[4] : 0.0, 0, 'g', 8)
                       .arg(visibleBounds ? visibleBounds[5] : 0.0, 0, 'g', 8)
                       .arg(position[0], 0, 'g', 8)
                       .arg(position[1], 0, 'g', 8)
                       .arg(position[2], 0, 'g', 8)
                       .arg(focalPoint[0], 0, 'g', 8)
                       .arg(focalPoint[1], 0, 'g', 8)
                       .arg(focalPoint[2], 0, 'g', 8)
                       .arg(clippingRange[0], 0, 'g', 8)
                       .arg(clippingRange[1], 0, 'g', 8);

    if (!visibleBounds || !(visibleBounds[0] < visibleBounds[1])
        || !(visibleBounds[2] < visibleBounds[3]) || !(visibleBounds[4] < visibleBounds[5])) {
        qDebug().noquote() << QStringLiteral("[vbt-volume] visible bounds invalid, applying known-good fallback");
        this->applyKnownGoodVisibleFallback(minValue, maxValue, bounds);
    }

    this->updateTransferFunctions();
    this->resetView();
    this->m_loadingActor->VisibilityOff();
    this->renderWindow->Render();
    this->updateSummary();
}

void vtkWindowVbtVolume::applyAutoColorRange()
{
    // A density field is not uniform: its top percentile can be four orders of
    // magnitude above its median, so the extremes are a bad colour range and a
    // worse opacity one. The 2nd-to-99.9th percentile of the sampled voxels is
    // what shows the structure, and it is what the viewer opens with.
    if (this->cachedSampledValues.size() >= 16) {
        this->setColorRange(percentileFromSortedSample(this->cachedSampledValues, 0.02),
                            percentileFromSortedSample(this->cachedSampledValues, 0.999));
        return;
    }
    this->setColorRange(this->dataScalarMin, this->dataScalarMax);
}

void vtkWindowVbtVolume::updateTransferFunctions()
{
    if (!this->currentImageData) {
        return;
    }

    // ── Blend mode ────────────────────────────────────────────────────────────
    // 0 = Composite  1 = MIP  2 = MinIP
    const int blendIdx = this->comboBlendMode ? this->comboBlendMode->currentIndex() : 0;
    const bool isMIP   = (blendIdx == 1);
    const bool isMinIP = (blendIdx == 2);
    if (isMIP) {
        this->volumeMapper->SetBlendModeToMaximumIntensity();
    } else if (isMinIP) {
        this->volumeMapper->SetBlendModeToMinimumIntensity();
    } else {
        this->volumeMapper->SetBlendModeToComposite();
    }

    // ── Colour/opacity range ──────────────────────────────────────────────────
    const LutScale::Mode scaleMode = this->comboScaleMode
            ? static_cast<LutScale::Mode>(std::clamp(this->comboScaleMode->currentIndex(), 0, 4))
            : LutScale::Mode::Linear;
    const double scaleGamma = this->spinScaleGamma ? this->spinScaleGamma->value() : 2.0;
    if (this->spinScaleGamma) {
        this->spinScaleGamma->setEnabled(scaleMode == LutScale::Mode::Power);
    }
    const bool logMode = (scaleMode == LutScale::Mode::Log);
    double colorMin = this->activeColorMin;
    double colorMax = this->activeColorMax;
    if (!(colorMin < colorMax)) {
        colorMin = this->dataScalarMin;
        colorMax = this->dataScalarMax;
    }

    const double positiveFloor = this->positiveFloor();
    if (logMode) {
        if (this->cachedPositiveSampledValues.empty()) {
            colorMin = positiveFloor;
            colorMax = std::max(positiveFloor * 10.0, this->dataScalarMax);
        } else {
            colorMin = std::max(colorMin, positiveFloor);
            colorMax = std::max(colorMax, std::max(colorMin * 1.01, this->cachedPositiveSampledValues.back()));
        }
    }
    if (!(colorMin < colorMax)) {
        colorMax = colorMin + 1.0;
    }

    this->colorTransfer->RemoveAllPoints();
    this->opacityTransfer->RemoveAllPoints();

    // ── Colour transfer function (identical for all blend modes) ──────────────
    if (logMode) {
        const double mid1 = std::sqrt(colorMin * std::max(colorMin, colorMax));
        const double mid2 = std::sqrt(mid1 * std::max(mid1, colorMax));
        this->colorTransfer->AddRGBPoint(positiveFloor, 0.01, 0.01, 0.02);
        this->colorTransfer->AddRGBPoint(colorMin,      0.03, 0.05, 0.12);
        this->colorTransfer->AddRGBPoint(mid1,          0.08, 0.25, 0.70);
        this->colorTransfer->AddRGBPoint(mid2,          0.90, 0.55, 0.12);
        this->colorTransfer->AddRGBPoint(colorMax,      1.00, 0.97, 0.88);
    } else {
        const double range = colorMax - colorMin;
        const double mid1  = colorMin + range * 0.55;
        const double mid2  = colorMin + range * 0.82;
        this->colorTransfer->AddRGBPoint(colorMin, 0.02, 0.02, 0.04);
        this->colorTransfer->AddRGBPoint(mid1,     0.08, 0.25, 0.70);
        this->colorTransfer->AddRGBPoint(mid2,     0.90, 0.55, 0.12);
        this->colorTransfer->AddRGBPoint(colorMax, 1.00, 0.97, 0.88);
    }

    // Sqrt/Square/Power: redistribute the linear gradient just built through the
    // stretch curve (Log keeps its dedicated log-spaced nodes above).
    if (scaleMode == LutScale::Mode::Sqrt || scaleMode == LutScale::Mode::Square
        || scaleMode == LutScale::Mode::Power) {
        LutScale::applyToColorTransferFunction(this->colorTransfer, scaleMode, colorMin, colorMax,
                                               scaleGamma);
    }

    // ── Opacity transfer function ─────────────────────────────────────────────
    const double opacityScale = (this->sliderOpacity ? this->sliderOpacity->value() : 28) / 100.0;

    if (isMIP || isMinIP) {
        // In MIP/MinIP modes the opacity TF acts as a voxel-inclusion mask:
        // voxels with opacity > 0 are candidates for the max/min search.
        // Opacity scale controls the lower threshold — higher = fewer (brighter)
        // voxels included; lower = entire range included.
        //
        // Strategy: exclude the very bottom opacityScale-fraction of the range
        // so that background/noise voxels do not win the projection.  The bulk
        // of the interesting data (above the threshold) gets opacity = 1.0.
        const double range      = colorMax - colorMin;
        const double threshold  = logMode
                ? std::max(positiveFloor, colorMin * std::pow(colorMax / std::max(colorMin, positiveFloor), opacityScale))
                : colorMin + range * opacityScale;
        const double rampEnd    = std::min(threshold + range * 0.01, colorMax);
        if (logMode) {
            this->opacityTransfer->AddPoint(positiveFloor, 0.0);
        }
        this->opacityTransfer->AddPoint(colorMin, 0.0);
        this->opacityTransfer->AddPoint(threshold, 0.0);
        this->opacityTransfer->AddPoint(rampEnd, 1.0);
        this->opacityTransfer->AddPoint(colorMax, 1.0);
    } else {
        // Composite: sophisticated percentile-based opacity ramp, scaled by slider.
        const double opacityLow     = logMode ? std::max(positiveFloor, this->cachedOpacityLow)
                                              : std::max(colorMin, this->cachedOpacityLow);
        const double opacityMidLow  = std::max(opacityLow,     this->cachedOpacityMidLow);
        const double opacityMidHigh = std::max(opacityMidLow,  this->cachedOpacityMidHigh);
        const double opacityHigh    = std::max(opacityMidHigh, std::max(colorMax, this->cachedOpacityHigh));

        if (logMode) {
            this->opacityTransfer->AddPoint(positiveFloor, 0.0);
            this->opacityTransfer->AddPoint(opacityLow,     0.0);
            this->opacityTransfer->AddPoint(opacityMidLow,  0.015 * opacityScale);
            this->opacityTransfer->AddPoint(opacityMidHigh, 0.08  * opacityScale);
            this->opacityTransfer->AddPoint(opacityHigh,    0.35  * opacityScale);
            this->opacityTransfer->AddPoint(std::max(opacityHigh, colorMax), 0.65 * opacityScale);
        } else {
            this->opacityTransfer->AddPoint(colorMin,       0.0);
            this->opacityTransfer->AddPoint(opacityLow,     0.0);
            this->opacityTransfer->AddPoint(opacityMidLow,  0.01 * opacityScale);
            this->opacityTransfer->AddPoint(opacityMidHigh, 0.05 * opacityScale);
            this->opacityTransfer->AddPoint(opacityHigh,    0.20 * opacityScale);
            this->opacityTransfer->AddPoint(std::max(opacityHigh, colorMax), 0.55 * opacityScale);
        }
    }

    // ── Scalar bar LUT (mirrors the colour TF for display) ────────────────────
    this->scalarBarLut->SetNumberOfTableValues(256);
    if (logMode) {
        this->scalarBarLut->SetScaleToLog10();
        this->scalarBarLut->SetTableRange(std::max(colorMin, positiveFloor),
                                          std::max(colorMax, colorMin * 1.01));
    } else {
        this->scalarBarLut->SetScaleToLinear();
        this->scalarBarLut->SetTableRange(colorMin, colorMax);
    }
    for (int i = 0; i < 256; ++i) {
        const double t = static_cast<double>(i) / 255.0;
        double rgb[3]{ 0.0, 0.0, 0.0 };
        const double value = logMode
                ? std::exp(std::log(std::max(colorMin, positiveFloor))
                           + t * (std::log(std::max(colorMax, colorMin * 1.01))
                                  - std::log(std::max(colorMin, positiveFloor))))
                : (colorMin + t * (colorMax - colorMin));
        this->colorTransfer->GetColor(value, rgb);
        this->scalarBarLut->SetTableValue(i, rgb[0], rgb[1], rgb[2], 1.0);
    }
    this->scalarBarLut->Build();
    this->scalarBar->SetLookupTable(this->scalarBarLut.Get());
    this->updateScalarBar();

    static const char *kBlendNames[] = { "composite", "MIP", "MinIP" };
    qDebug().noquote()
            << QStringLiteral("[vbt-volume] tf blend=%1 scale=%2 image_range=%3..%4 color_range=%5..%6 opacity_scale=%7 positive_floor=%8")
                       .arg(kBlendNames[std::clamp(blendIdx, 0, 2)])
                       .arg(logMode ? QStringLiteral("log") : QStringLiteral("linear"))
                       .arg(this->dataScalarMin, 0, 'g', 8)
                       .arg(this->dataScalarMax, 0, 'g', 8)
                       .arg(colorMin, 0, 'g', 8)
                       .arg(colorMax, 0, 'g', 8)
                       .arg(opacityScale, 0, 'f', 2)
                       .arg(positiveFloor, 0, 'g', 8);
    this->renderWindow->Render();
}

void vtkWindowVbtVolume::updateBoundsContext()
{
    if (!this->currentImageData) {
        return;
    }
    double bounds[6]{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    this->currentImageData->GetBounds(bounds);
    this->boxActor->SetBounds(bounds);
}

void vtkWindowVbtVolume::updateScalarBar()
{
    const QString activeField = this->comboScalarField ? this->comboScalarField->currentText() : QString();
    this->scalarBar->SetTitle(activeField.toStdString().c_str());
    this->scalarBar->SetVisibility(this->comboScalarField && this->comboScalarField->count() > 0 ? 1 : 0);
}

void vtkWindowVbtVolume::resetView()
{
    this->renderer->ResetCamera();
    this->renderer->ResetCameraClippingRange();
}

void vtkWindowVbtVolume::applyCopilotActions(const QList<QJsonObject> &actions)
{
    // Which directives can this volume view act on?
    bool batchSetsScale = false, batchSetsBlend = false;
    bool batchSetsColorField = false, batchResetsCamera = false;
    for (const QJsonObject &a : actions) {
        const QString t = a.value(u"type"_s).toString();
        if (t == u"set_scaling"_s)          batchSetsScale = true;
        else if (t == u"set_blend_mode"_s)  batchSetsBlend = true;
        else if (t == u"set_color_field"_s) batchSetsColorField = true;
        else if (t == u"reset_camera"_s)    batchResetsCamera = true;
    }
    if (!batchSetsScale && !batchSetsBlend && !batchSetsColorField && !batchResetsCamera)
        return;   // nothing applicable — leave any prior Undo point untouched

    // Fresh snapshot of only what this batch touches (per-batch "Undo latest").
    vtkCamera *camera = this->renderer ? this->renderer->GetActiveCamera() : nullptr;
    this->m_copilotUndo = CopilotUndo{};
    if (batchSetsScale && this->comboScaleMode) {
        this->m_copilotUndo.scaleText = this->comboScaleMode->currentText();
        this->m_copilotUndo.scaleCaptured = true;
    }
    if (batchSetsBlend && this->comboBlendMode) {
        this->m_copilotUndo.blendIndex = this->comboBlendMode->currentIndex();
        this->m_copilotUndo.blendCaptured = true;
    }
    if (batchSetsColorField && this->comboScalarField) {
        this->m_copilotUndo.colorFieldIndex = this->comboScalarField->currentIndex();
        this->m_copilotUndo.colorFieldCaptured = true;
    }
    // Capture the camera when the batch resets it OR changes the colour field —
    // switching field rebuilds the volume and reframes the camera as a side
    // effect, so Undo must restore the user's pre-copilot view. undo restores the
    // camera LAST (after the field restore's own reframing) so it wins.
    if ((batchResetsCamera || batchSetsColorField) && camera) {
        camera->GetPosition(this->m_copilotUndo.position);
        camera->GetFocalPoint(this->m_copilotUndo.focalPoint);
        camera->GetViewUp(this->m_copilotUndo.viewUp);
        this->m_copilotUndo.parallelScale = camera->GetParallelScale();
        this->m_copilotUndo.cameraCaptured = true;
    }

    for (const QJsonObject &a : actions) {
        const QString type = a.value(u"type"_s).toString();
        if (type == u"set_scaling"_s && this->comboScaleMode) {
            const int idx = this->comboScaleMode->findText(
                    a.value(u"scale"_s).toString().trimmed(), Qt::MatchFixedString);
            if (idx >= 0)
                this->comboScaleMode->setCurrentIndex(idx);   // triggers re-render
        } else if (type == u"set_blend_mode"_s && this->comboBlendMode) {
            const QString m = a.value(u"mode"_s).toString().trimmed().toLower();
            const int idx = (m == u"mip"_s) ? 1 : (m == u"minip"_s) ? 2
                          : (m == u"composite"_s) ? 0 : -1;
            if (idx >= 0 && idx < this->comboBlendMode->count())
                this->comboBlendMode->setCurrentIndex(idx);
        } else if (type == u"set_color_field"_s && this->comboScalarField) {
            const QString field = a.value(u"field"_s).toString().trimmed();
            int idx = -1;
            for (int i = 0; i < this->comboScalarField->count(); ++i) {
                if (this->comboScalarField->itemText(i).compare(field, Qt::CaseInsensitive) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx >= 0)
                this->comboScalarField->setCurrentIndex(idx);
        } else if (type == u"reset_camera"_s && camera) {
            this->resetView();
            if (this->renderWindow)
                this->renderWindow->Render();
        }
    }
}

void vtkWindowVbtVolume::undoCopilotActions()
{
    if (this->m_copilotUndo.scaleCaptured && this->comboScaleMode) {
        const int idx = this->comboScaleMode->findText(this->m_copilotUndo.scaleText,
                                                       Qt::MatchFixedString);
        if (idx >= 0)
            this->comboScaleMode->setCurrentIndex(idx);
        this->m_copilotUndo.scaleCaptured = false;
    }
    if (this->m_copilotUndo.blendCaptured && this->comboBlendMode) {
        const int idx = this->m_copilotUndo.blendIndex;
        if (idx >= 0 && idx < this->comboBlendMode->count())
            this->comboBlendMode->setCurrentIndex(idx);
        this->m_copilotUndo.blendCaptured = false;
    }
    if (this->m_copilotUndo.colorFieldCaptured && this->comboScalarField) {
        const int idx = this->m_copilotUndo.colorFieldIndex;
        if (idx >= 0 && idx < this->comboScalarField->count())
            this->comboScalarField->setCurrentIndex(idx);
        this->m_copilotUndo.colorFieldCaptured = false;
    }
    vtkCamera *camera = this->renderer ? this->renderer->GetActiveCamera() : nullptr;
    if (camera && this->m_copilotUndo.cameraCaptured) {
        camera->SetPosition(this->m_copilotUndo.position);
        camera->SetFocalPoint(this->m_copilotUndo.focalPoint);
        camera->SetViewUp(this->m_copilotUndo.viewUp);
        camera->SetParallelScale(this->m_copilotUndo.parallelScale);
        this->m_copilotUndo.cameraCaptured = false;
        this->renderer->ResetCameraClippingRange();
        if (this->renderWindow)
            this->renderWindow->Render();
    }
}

void vtkWindowVbtVolume::applyBackground()
{
    if (!this->comboBackground) return;
    const QString bg = this->comboBackground->currentText();
    const bool dark = (bg != u"White"_s);
    if (bg == u"White"_s) {
        this->renderer->SetBackground(0.98, 0.98, 0.98);
    } else if (bg == u"Dark gray"_s) {
        this->renderer->SetBackground(0.16, 0.17, 0.19);
    } else {
        this->renderer->SetBackground(0.04, 0.04, 0.08);
    }
    this->applyBoxActorColors(dark);
    // Update scalar bar text colour
    const double textColor = dark ? 1.0 : 0.1;
    this->scalarBar->GetTitleTextProperty()->SetColor(textColor, textColor, textColor);
    this->scalarBar->GetLabelTextProperty()->SetColor(textColor, textColor, textColor);
    this->renderWindow->Render();
}

void vtkWindowVbtVolume::applyBoxActorColors(bool darkBackground)
{
    const double textTitle = darkBackground ? 0.85 : 0.10;
    const double textLabel = darkBackground ? 0.75 : 0.20;
    const double lineColor = darkBackground ? 0.80 : 0.15;
    for (int axis = 0; axis < 3; ++axis) {
        this->boxActor->GetTitleTextProperty(axis)->SetColor(textTitle, textTitle, textTitle);
        this->boxActor->GetLabelTextProperty(axis)->SetColor(textLabel, textLabel, textLabel);
    }
    this->boxActor->GetXAxesLinesProperty()->SetColor(lineColor, lineColor, lineColor);
    this->boxActor->GetYAxesLinesProperty()->SetColor(lineColor, lineColor, lineColor);
    this->boxActor->GetZAxesLinesProperty()->SetColor(lineColor, lineColor, lineColor);
}

void vtkWindowVbtVolume::applyKnownGoodVisibleFallback(double minValue, double maxValue,
                                                       const double bounds[6])
{
    if (!(minValue < maxValue)) {
        maxValue = minValue + 1.0;
    }
    const double range = maxValue - minValue;
    this->colorTransfer->RemoveAllPoints();
    this->colorTransfer->AddRGBPoint(minValue, 0.0, 0.0, 0.0);
    this->colorTransfer->AddRGBPoint(minValue + range * 0.5, 0.8, 0.4, 0.1);
    this->colorTransfer->AddRGBPoint(maxValue, 1.0, 1.0, 1.0);

    this->opacityTransfer->RemoveAllPoints();
    this->opacityTransfer->AddPoint(minValue, 0.0);
    this->opacityTransfer->AddPoint(minValue + range * 0.1, 0.05);
    this->opacityTransfer->AddPoint(minValue + range * 0.4, 0.2);
    this->opacityTransfer->AddPoint(maxValue, 0.9);

    this->boxActor->SetVisibility(1);
    this->scalarBar->SetVisibility(1);
    this->boxActor->SetBounds(bounds);
    this->renderer->ResetCamera(bounds);
    this->renderer->ResetCameraClippingRange(bounds);
}

void vtkWindowVbtVolume::setColorRange(double minValue, double maxValue)
{
    if (!(minValue < maxValue)) {
        minValue = this->dataScalarMin;
        maxValue = this->dataScalarMax;
        if (!(minValue < maxValue)) {
            maxValue = minValue + 1.0;
        }
    }
    this->activeColorMin = minValue;
    this->activeColorMax = maxValue;
    this->updatingRangeControls = true;
    if (this->spinRangeMin) {
        this->spinRangeMin->setValue(minValue);
    }
    if (this->spinRangeMax) {
        this->spinRangeMax->setValue(maxValue);
    }
    this->updatingRangeControls = false;
}

double vtkWindowVbtVolume::positiveFloor() const
{
    if (!this->cachedPositiveSampledValues.empty()) {
        const double sampledFloor =
                percentileFromSortedSample(this->cachedPositiveSampledValues, 0.01);
        if (std::isfinite(sampledFloor) && sampledFloor > 0.0) {
            return sampledFloor;
        }
        return this->cachedPositiveSampledValues.front();
    }
    if (this->dataScalarMax > 0.0) {
        return std::max(1e-12, this->dataScalarMax * 1e-12);
    }
    return 1e-12;
}

void vtkWindowVbtVolume::updateSummary()
{
    const QString activeField = this->comboScalarField ? this->comboScalarField->currentText() : QString();
    const auto &dims = this->openResult.dimensions;
    const auto &spacing = this->openResult.spacing;
    Q_UNUSED(dims);
    // The scalar being rendered follows the Scalar selector on the left, so it
    // is the one line of the old SUMMARY card that had to stay live.
    if (this->m_propActiveScalar)
        InspectorPanel::applyValueStyle(this->m_propActiveScalar, activeField);
    setInfoRows(this->metadataGrid,
                { { u"Kind"_s, this->openResult.kind },
                  { u"Spacing"_s, QStringLiteral("%1, %2, %3")
                                          .arg(spacing[0]).arg(spacing[1]).arg(spacing[2]) },
                  { u"Fields"_s, QString::number(this->openResult.fields.size()) },
                  { u"Voxels"_s, QString::number(this->openResult.numRows) },
                  { u"Dataset ID"_s, this->openResult.datasetId } });
}

// ── Shared viewer chrome ─────────────────────────────────────────────────────
//
// Same command bar and status rail as the image and cube viewers: the path of
// the file on screen, a tag saying what it is, ⌘K, and the live backend chip —
// plus the bottom rail with health, queue, cache and session. This window is a
// remote dataset viewer like the others and had none of it.

void vtkWindowVbtVolume::setupViewerChrome()
{
    VisivoTheme::ViewerToolbarSpec spec;
    spec.filePath = this->m_headerPath;
    spec.primaryTag = QStringLiteral("VBT · %1×%2×%3")
                              .arg(this->openResult.dimensions[0])
                              .arg(this->openResult.dimensions[1])
                              .arg(this->openResult.dimensions[2]);
    spec.secondaryTag = QStringLiteral("remote · %1").arg(this->openResult.scalarType);

    VisivoTheme::ViewerToolbarWidgets widgets;
    this->m_workspaceChrome = WorkspaceChrome::installCommandBar(
            this, spec, this->backendUrl, this->backendToken, this->sessionId, &widgets);
    if (!this->m_workspaceChrome)
        return;
    if (widgets.primaryTag) {
        widgets.primaryTag->setToolTip(QStringLiteral("The VBT header: fields, type, geometry."));
        QObject::connect(widgets.primaryTag, &QToolButton::clicked, this,
                         [this]() { this->showVbtHeaderDialog(); });
    }
    this->m_workspaceChrome->installStatusRail();
    this->setupWorkspaceDocks();
    this->m_workspaceChrome->start();
}

void vtkWindowVbtVolume::setupWorkspaceDocks()
{
    WorkspaceChrome *chrome = this->m_workspaceChrome;
    if (!chrome)
        return;
    const QFileInfo info(this->m_headerPath);
    chrome->installSessionDock(this->m_headerPath, info.fileName(), u"vbt-volume"_s);
    if (this->m_pageVolume) {
        this->m_pageVolume->show();
        chrome->addSessionLayers(this->m_pageVolume);
    }

    InspectorPanel *panel = chrome->installInspectorDock();
    if (!panel)
        return;
    panel->clearProperties();
    panel->addPropertyRow(u"Dataset"_s, info.fileName());
    panel->addPropertyRow(u"Type"_s, u"VBT VOLUME"_s);
    panel->addPropertyRow(u"Dimensions"_s, QStringLiteral("%1 × %2 × %3")
                                                   .arg(this->openResult.dimensions[0])
                                                   .arg(this->openResult.dimensions[1])
                                                   .arg(this->openResult.dimensions[2]));
    panel->addPropertyRow(u"Scalar type"_s, this->openResult.scalarType);
    // Live: the scalar being rendered follows the Scalar selector on the left.
    this->m_propActiveScalar = new QLabel;
    InspectorPanel::applyValueStyle(
            this->m_propActiveScalar,
            this->comboScalarField ? this->comboScalarField->currentText() : QString());
    panel->addPropertyRow(u"Active scalar"_s, this->m_propActiveScalar);
    panel->addPropertyRow(u"Path"_s, this->m_headerPath);
    panel->addPropertyRow(u"Session"_s, this->sessionId);
    panel->addPropertyRow(u"Backend"_s, this->backendUrl);
    if (this->m_pageInfo) {
        this->m_pageInfo->show();
        panel->addPropertiesSection(this->m_pageInfo);
    }

    if (!this->backendUrl.isEmpty()) {
        this->m_copilot = chrome->installCopilot(this->backendUrl, this->backendToken,
                                                 this->openResult.datasetId);
        if (this->m_copilot) {
            this->m_copilot->setSuggestions({
                    u"How many points and what fields does this table have?"_s,
                    u"Show it in log scale"_s,
                    u"Use MIP blending"_s });
            CopilotActionRouter::instance()->registerTarget(
                    this->openResult.datasetId, this,
                    [this](const QList<QJsonObject> &acts) { this->applyCopilotActions(acts); },
                    [this]() { this->undoCopilotActions(); });
        }
    }

    if (this->m_actionSessionPanel) {
        QSignalBlocker block(this->m_actionSessionPanel);
        this->m_actionSessionPanel->setChecked(!chrome->sessionCollapsed());
    }
    if (this->actionShowControlsSidebar) {
        QSignalBlocker block(this->actionShowControlsSidebar);
        this->actionShowControlsSidebar->setChecked(!chrome->inspectorCollapsed());
    }
    QObject::connect(chrome, &WorkspaceChrome::dockCollapseChanged, this, [this]() {
        if (!this->m_workspaceChrome)
            return;
        if (this->m_actionSessionPanel) {
            QSignalBlocker block(this->m_actionSessionPanel);
            this->m_actionSessionPanel->setChecked(!this->m_workspaceChrome->sessionCollapsed());
        }
        if (this->actionShowControlsSidebar) {
            QSignalBlocker block(this->actionShowControlsSidebar);
            this->actionShowControlsSidebar->setChecked(
                    !this->m_workspaceChrome->inspectorCollapsed());
        }
    });
}

void vtkWindowVbtVolume::showVbtHeaderDialog()
{
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setWindowTitle(QStringLiteral("VBT Header - %1")
                                .arg(QFileInfo(this->m_headerPath).fileName()));
    dlg->resize(520, 420);

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *header = new QLabel(QStringLiteral("VBT Header"), dlg);
    header->setStyleSheet(VisivoTheme::sidebarHeaderStyleSheet());
    layout->addWidget(header);

    auto *summary = new QLabel(
            QStringLiteral("%1 × %2 × %3 voxels · spacing %4, %5, %6\n%7 · %8 field(s)")
                    .arg(this->openResult.dimensions[0])
                    .arg(this->openResult.dimensions[1])
                    .arg(this->openResult.dimensions[2])
                    .arg(this->openResult.spacing[0], 0, 'g', 6)
                    .arg(this->openResult.spacing[1], 0, 'g', 6)
                    .arg(this->openResult.spacing[2], 0, 'g', 6)
                    .arg(this->openResult.scalarType)
                    .arg(this->openResult.fields.size()),
            dlg);
    summary->setStyleSheet(VisivoTheme::infoValueStyleSheet());
    layout->addWidget(summary);

    auto *table = new QTableWidget(static_cast<int>(this->openResult.fields.size()), 1, dlg);
    table->setHorizontalHeaderLabels({ QStringLiteral("Field") });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    for (int row = 0; row < static_cast<int>(this->openResult.fields.size()); ++row)
        table->setItem(row, 0, new QTableWidgetItem(this->openResult.fields[row].name));
    layout->addWidget(table, 1);

    auto *path = new QLabel(this->m_headerPath, dlg);
    path->setStyleSheet(VisivoTheme::infoKeyStyleSheet());
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    layout->addWidget(path);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg->show();
}

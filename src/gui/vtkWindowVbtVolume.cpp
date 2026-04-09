#include "vtkWindowVbtVolume.h"
#include "app/BackendClient.h"

#include <QVTKOpenGLNativeWidget.h>

#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCubeAxesActor.h>
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
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
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
                                       QWidget *parent)
    : QMainWindow(parent)
    , openResult(std::move(openResult))
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
    this->setupRenderer();
    // Apply the initial subset received at open time without an extra round-trip.
    // rebuildVolumeData() will contact the backend on subsequent field changes.
    this->applySubsetResult(std::move(initialSubset));
    this->updateSummary();
}

void vtkWindowVbtVolume::setupUi()
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(QStringLiteral("VBT Volume Viewer — %1 x %2 x %3")
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

    this->setupSidebarUi(central);
    rootLayout->addWidget(this->sidebarContainer);

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
    QObject::connect(this->buttonAutoscale, &QPushButton::clicked, this, [this]() {
        this->setColorRange(this->dataScalarMin, this->dataScalarMax);
        this->updateTransferFunctions();
    });
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
    this->actionShowControlsSidebar = new QAction(u"Controls Sidebar"_s, this);
    this->actionShowControlsSidebar->setCheckable(true);
    this->actionShowControlsSidebar->setChecked(true);
    viewMenu->addAction(this->actionShowControlsSidebar);
    QObject::connect(this->actionShowControlsSidebar, &QAction::toggled, this,
                     [this](bool checked) { setSidebarExpanded(checked); });
}

// ─────────────────────────────────────────────────────────────────────────────
// Sidebar implementation
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowVbtVolume::setupSidebarUi(QWidget *parent)
{
    // ── Sidebar container ─────────────────────────────────────────────────────
    this->sidebarContainer = new QWidget(parent);
    this->sidebarContainer->setObjectName(u"sidebarContainer"_s);
    this->sidebarContainer->setMinimumWidth(48);
    this->sidebarContainer->setMaximumWidth(320);

    auto *containerLayout = new QHBoxLayout(this->sidebarContainer);
    containerLayout->setSpacing(0);
    containerLayout->setContentsMargins(0, 0, 0, 0);

    // ── Rail ──────────────────────────────────────────────────────────────────
    this->railWidget = new QWidget(this->sidebarContainer);
    this->railWidget->setObjectName(u"railWidget"_s);
    this->railWidget->setFixedWidth(48);

    auto *railLayout = new QVBoxLayout(this->railWidget);
    railLayout->setSpacing(2);
    railLayout->setContentsMargins(4, 8, 4, 8);

    this->btnToggleSidebar = new QToolButton(this->railWidget);
    this->btnToggleSidebar->setToolTip(u"Toggle sidebar"_s);
    this->btnToggleSidebar->setCheckable(true);
    this->btnToggleSidebar->setChecked(true);
    this->btnToggleSidebar->setAutoRaise(true);

    this->railBtnVolume = new QToolButton(this->railWidget);
    this->railBtnVolume->setToolTip(u"Volume Settings"_s);
    this->railBtnVolume->setCheckable(true);
    this->railBtnVolume->setChecked(true);
    this->railBtnVolume->setAutoRaise(true);

    this->railBtnMetadata = new QToolButton(this->railWidget);
    this->railBtnMetadata->setToolTip(u"VBT Metadata"_s);
    this->railBtnMetadata->setCheckable(true);
    this->railBtnMetadata->setAutoRaise(true);

    railLayout->addWidget(this->btnToggleSidebar);
    railLayout->addWidget(this->railBtnVolume);
    railLayout->addWidget(this->railBtnMetadata);
    railLayout->addStretch(1);

    containerLayout->addWidget(this->railWidget);

    // ── Scroll area + stack ───────────────────────────────────────────────────
    this->sidebarScrollArea = new QScrollArea(this->sidebarContainer);
    this->sidebarScrollArea->setObjectName(u"sidebarScrollArea"_s);
    this->sidebarScrollArea->setWidgetResizable(true);
    this->sidebarScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->sidebarScrollArea->setFrameShape(QFrame::NoFrame);

    auto *scrollContents = new QWidget(this->sidebarScrollArea);
    auto *scrollLayout = new QVBoxLayout(scrollContents);
    scrollLayout->setSpacing(0);
    scrollLayout->setContentsMargins(0, 0, 0, 0);

    this->sidebarStack = new QStackedWidget(scrollContents);
    scrollLayout->addWidget(this->sidebarStack);
    this->sidebarScrollArea->setWidget(scrollContents);
    containerLayout->addWidget(this->sidebarScrollArea);

    // ── Page 0: Volume controls ───────────────────────────────────────────────
    auto *pageVolume = new QWidget;
    auto *pageVolumeLayout = new QVBoxLayout(pageVolume);
    pageVolumeLayout->setContentsMargins(8, 8, 8, 8);
    pageVolumeLayout->setSpacing(8);

    auto *hdrVolume = new QLabel(u"VOLUME SETTINGS"_s);
    pageVolumeLayout->addWidget(hdrVolume);

    auto *displayGroup = new QGroupBox(u"Volume"_s, pageVolume);
    auto *displayLayout = new QFormLayout(displayGroup);
    this->comboScalarField = new QComboBox(displayGroup);
    for (const auto &field : this->openResult.fields) {
        this->comboScalarField->addItem(field.name, field.name);
    }
    this->comboBlendMode = new QComboBox(displayGroup);
    this->comboBlendMode->addItems(
            { u"Composite"_s, u"MIP (Max Intensity)"_s, u"MinIP (Min Intensity)"_s });
    this->comboBlendMode->setToolTip(
            u"Composite: full volume ray-casting with opacity transfer function.\n"
            "MIP: show the maximum scalar value along each ray (ignores opacity TF).\n"
            "MinIP: show the minimum scalar value along each ray."_s);
    this->comboScaleMode = new QComboBox(displayGroup);
    this->comboScaleMode->addItems({ u"Linear"_s, u"Log"_s });
    this->sliderOpacity = new QSlider(Qt::Horizontal, displayGroup);
    this->sliderOpacity->setRange(1, 100);
    this->sliderOpacity->setValue(28);
    this->sliderOpacity->setToolTip(
            u"Composite: scales the opacity ramp — higher = more opaque.\n"
            "MIP/MinIP: raises the threshold above which voxels are projection candidates."_s);
    this->spinRangeMin = new QDoubleSpinBox(displayGroup);
    this->spinRangeMax = new QDoubleSpinBox(displayGroup);
    this->spinRangeMin->setDecimals(12);
    this->spinRangeMax->setDecimals(12);
    this->spinRangeMin->setRange(-1e30, 1e30);
    this->spinRangeMax->setRange(-1e30, 1e30);
    this->buttonAutoscale = new QPushButton(u"Autoscale"_s, displayGroup);
    displayLayout->addRow(u"Scalar"_s, this->comboScalarField);
    displayLayout->addRow(u"Mode"_s, this->comboBlendMode);
    displayLayout->addRow(u"Scale"_s, this->comboScaleMode);
    displayLayout->addRow(u"Range min"_s, this->spinRangeMin);
    displayLayout->addRow(u"Range max"_s, this->spinRangeMax);
    displayLayout->addRow(u"Opacity"_s, this->sliderOpacity);
    displayLayout->addRow(this->buttonAutoscale);
    pageVolumeLayout->addWidget(displayGroup);

    auto *summaryGroup = new QGroupBox(u"Summary"_s, pageVolume);
    auto *summaryLayout = new QVBoxLayout(summaryGroup);
    this->summaryLabel = new QLabel(summaryGroup);
    this->summaryLabel->setWordWrap(true);
    summaryLayout->addWidget(this->summaryLabel);
    pageVolumeLayout->addWidget(summaryGroup);
    pageVolumeLayout->addStretch(1);
    this->sidebarStack->addWidget(pageVolume); // index 0

    // ── Page 1: Metadata ──────────────────────────────────────────────────────
    auto *pageMetadata = new QWidget;
    auto *pageMetadataLayout = new QVBoxLayout(pageMetadata);
    pageMetadataLayout->setContentsMargins(8, 8, 8, 8);
    pageMetadataLayout->setSpacing(8);

    auto *hdrMetadata = new QLabel(u"VBT METADATA"_s);
    pageMetadataLayout->addWidget(hdrMetadata);

    this->metadataLabel = new QLabel(pageMetadata);
    this->metadataLabel->setWordWrap(true);
    pageMetadataLayout->addWidget(this->metadataLabel);

    this->fieldList = new QListWidget(pageMetadata);
    for (const auto &field : this->openResult.fields) {
        this->fieldList->addItem(QStringLiteral("%1 [%2]").arg(field.name, field.type));
    }
    pageMetadataLayout->addWidget(this->fieldList, 1);
    this->sidebarStack->addWidget(pageMetadata); // index 1

    // ── Styles ────────────────────────────────────────────────────────────────
    setupSidebarRail();
    setupRailIcons();
    setupSidebarPages();

    // ── Rail connections ──────────────────────────────────────────────────────
    QObject::connect(this->btnToggleSidebar, &QToolButton::clicked, this,
                     &vtkWindowVbtVolume::toggleSidebar);
    QObject::connect(this->railBtnVolume, &QToolButton::clicked, this,
                     [this]() { switchToPage(0); });
    QObject::connect(this->railBtnMetadata, &QToolButton::clicked, this,
                     [this]() { switchToPage(1); });

    setActiveRailButton(this->railBtnVolume);
    this->sidebarStack->setCurrentIndex(0);
}

void vtkWindowVbtVolume::setupSidebarRail()
{
    const QString railStyle =
            u"QWidget#railWidget { background-color: #0A2236; }"
            u"QToolButton { background: transparent; border: none; border-radius: 8px;"
            u"              color: #8AB8CC; padding: 6px; min-width: 36px; min-height: 36px; }"
            u"QToolButton:hover { background-color: rgba(4,138,191,0.18); }"
            u"QToolButton[active='true'] { background-color: rgba(4,138,191,0.30); color: #FFFFFF; }"_s;
    if (this->railWidget) {
        this->railWidget->setStyleSheet(railStyle);
    }
    if (this->sidebarContainer) {
        this->sidebarContainer->setStyleSheet(
                u"QWidget#sidebarContainer { background-color: #0A2236; }"
                u"QScrollArea#sidebarScrollArea { background-color: transparent; border: none; }"_s);
    }
}

void vtkWindowVbtVolume::setupRailIcons()
{
    struct Entry { QToolButton *btn; QString path; };
    const std::array<Entry, 3> entries = { {
        { this->btnToggleSidebar, u":/icons/rail_slice.svg"_s    },
        { this->railBtnVolume,    u":/icons/rail_display.svg"_s  },
        { this->railBtnMetadata,  u":/icons/rail_metadata.svg"_s },
    } };
    for (const auto &e : entries) {
        if (e.btn) {
            e.btn->setIcon(QIcon(e.path));
            e.btn->setIconSize({ 22, 22 });
            e.btn->setText({});
        }
    }
}

void vtkWindowVbtVolume::setupSidebarPages()
{
    const QString hdrStyle =
            u"QLabel { color: #8AB8CC; font-size: 10px; font-weight: 600;"
            u"         padding-bottom: 4px; border-bottom: 1px solid rgba(4,138,191,0.20); }"_s;
    for (int i = 0; i < this->sidebarStack->count(); ++i) {
        auto *page = this->sidebarStack->widget(i);
        const auto labels = page->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly);
        if (!labels.isEmpty()) {
            labels.first()->setStyleSheet(hdrStyle);
        }
    }
}

void vtkWindowVbtVolume::setActiveRailButton(QToolButton *btn)
{
    for (QToolButton *b : { this->railBtnVolume.data(), this->railBtnMetadata.data() }) {
        if (b) {
            b->setProperty("active", b == btn);
            b->style()->polish(b);
        }
    }
}

void vtkWindowVbtVolume::switchToPage(int index)
{
    if (this->sidebarExpanded) {
        if (this->sidebarStack) {
            this->sidebarStack->setCurrentIndex(index);
        }
        setActiveRailButton(index == 0 ? this->railBtnVolume.data()
                                       : this->railBtnMetadata.data());
    } else {
        this->pendingPageIndex = index;
        setSidebarExpanded(true);
    }
}

void vtkWindowVbtVolume::toggleSidebar()
{
    setSidebarExpanded(!this->sidebarExpanded);
}

void vtkWindowVbtVolume::setSidebarExpanded(bool expanded, bool animated)
{
    if (this->sidebarExpanded == expanded && !animated) {
        return;
    }
    this->sidebarExpanded = expanded;

    if (this->actionShowControlsSidebar) {
        QSignalBlocker blocker(this->actionShowControlsSidebar);
        this->actionShowControlsSidebar->setChecked(expanded);
    }

    const int targetW = expanded ? 320 : 48;

    if (!animated) {
        if (this->sidebarContainer) {
            this->sidebarContainer->setMinimumWidth(targetW);
            this->sidebarContainer->setMaximumWidth(targetW);
        }
        if (this->sidebarScrollArea) {
            this->sidebarScrollArea->setVisible(expanded);
        }
        if (expanded && this->pendingPageIndex >= 0 && this->sidebarStack) {
            this->sidebarStack->setCurrentIndex(this->pendingPageIndex);
            setActiveRailButton(this->pendingPageIndex == 0 ? this->railBtnVolume.data()
                                                            : this->railBtnMetadata.data());
            this->pendingPageIndex = -1;
        }
        return;
    }

    if (this->sidebarScrollArea) {
        this->sidebarScrollArea->setVisible(true);
    }

    if (this->sidebarAnimGroup) {
        this->sidebarAnimGroup->stop();
    }

    auto *group = new QParallelAnimationGroup(this);
    this->sidebarAnimGroup = group;

    auto *animMin = new QPropertyAnimation(this->sidebarContainer, "minimumWidth", group);
    animMin->setDuration(200);
    animMin->setStartValue(this->sidebarContainer ? this->sidebarContainer->minimumWidth() : 48);
    animMin->setEndValue(targetW);

    auto *animMax = new QPropertyAnimation(this->sidebarContainer, "maximumWidth", group);
    animMax->setDuration(200);
    animMax->setStartValue(this->sidebarContainer ? this->sidebarContainer->maximumWidth() : 320);
    animMax->setEndValue(targetW);

    group->addAnimation(animMin);
    group->addAnimation(animMax);

    QObject::connect(group, &QParallelAnimationGroup::finished, this, [this, expanded]() {
        if (this->sidebarScrollArea) {
            this->sidebarScrollArea->setVisible(expanded);
        }
        if (expanded && this->pendingPageIndex >= 0 && this->sidebarStack) {
            this->sidebarStack->setCurrentIndex(this->pendingPageIndex);
            setActiveRailButton(this->pendingPageIndex == 0 ? this->railBtnVolume.data()
                                                            : this->railBtnMetadata.data());
            this->pendingPageIndex = -1;
        }
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
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
    for (int axis = 0; axis < 3; ++axis) {
        this->boxActor->GetTitleTextProperty(axis)->SetColor(0.85, 0.85, 0.85);
        this->boxActor->GetLabelTextProperty(axis)->SetColor(0.75, 0.75, 0.75);
    }
    this->boxActor->SetVisibility(1);
    this->renderer->AddActor(this->boxActor.Get());

    this->scalarBar->SetLookupTable(this->colorTransfer.Get());
    this->scalarBar->SetMaximumWidthInPixels(100);
    this->scalarBar->SetPosition(0.88, 0.1);
    this->scalarBar->GetTitleTextProperty()->SetColor(1.0, 1.0, 1.0);
    this->scalarBar->GetLabelTextProperty()->SetColor(1.0, 1.0, 1.0);
    this->scalarBar->SetVisibility(1);
    this->renderer->AddViewProp(this->scalarBar.Get());

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
    if (this->cachedSampledValues.size() >= 16) {
        this->setColorRange(percentileFromSortedSample(this->cachedSampledValues, 0.02),
                            percentileFromSortedSample(this->cachedSampledValues, 0.999));
    } else {
        this->setColorRange(minValue, maxValue);
    }

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
    this->renderWindow->Render();
    this->updateSummary();
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
    const bool logMode = this->comboScaleMode && this->comboScaleMode->currentIndex() == 1;
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
    if (this->summaryLabel) {
        this->summaryLabel->setText(
                QStringLiteral("Dimensions: %1 x %2 x %3\nSpacing: %4, %5, %6\nFields: %7\nScalar type: %8\nActive scalar: %9")
                        .arg(dims[0]).arg(dims[1]).arg(dims[2])
                        .arg(spacing[0]).arg(spacing[1]).arg(spacing[2])
                        .arg(this->openResult.fields.size())
                        .arg(this->openResult.scalarType)
                        .arg(activeField));
    }
    if (this->metadataLabel) {
        this->metadataLabel->setText(
                QStringLiteral("Kind: %1\nFields: %2\nVoxel count: %3\nDataset ID: %4")
                        .arg(this->openResult.kind)
                        .arg(this->openResult.fields.size())
                        .arg(this->openResult.numRows)
                        .arg(this->openResult.datasetId));
    }
}

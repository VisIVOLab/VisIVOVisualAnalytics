#include "vtkWindowVbt.h"

#include "app/DiagnosticsManager.h"

#include <QVTKOpenGLNativeWidget.h>

#include <vtkActor.h>
#include <vtkAxesActor.h>
#include <vtkCellArray.h>
#include <vtkCubeAxesActor.h>
#include <vtkDoubleArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkScalarBarActor.h>
#include <vtkTextProperty.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QDebug>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
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
#include <QtConcurrent>

#include <optional>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Shared helper – "Add Filter" dialog
// ─────────────────────────────────────────────────────────────────────────────

// existing != nullptr → pre-populate for edit; title changes to "Edit Filter".
static std::optional<BackendTabularFilter> showAddFilterDialog(
        QWidget *parent, const QStringList &fields,
        const BackendTabularFilter *existing = nullptr)
{
    auto *dlg = new QDialog(parent);
    dlg->setWindowTitle(existing ? u"Edit Filter"_s : u"Add Filter"_s);
    dlg->setModal(true);
    dlg->setMinimumWidth(320);
    auto *layout = new QFormLayout(dlg);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *comboField = new QComboBox(dlg);
    comboField->addItems(fields);
    if (existing) {
        comboField->setCurrentText(existing->field);
    }

    auto *comboOp = new QComboBox(dlg);
    comboOp->addItems({ u"="_s, u"!="_s, u">"_s, u">="_s, u"<"_s, u"<="_s,
                        u"contains"_s, u"in"_s });
    if (existing) {
        comboOp->setCurrentText(existing->op);
    }

    auto *lineValue = new QLineEdit(dlg);
    lineValue->setPlaceholderText(u"value (comma-separated for 'in')"_s);
    if (existing) {
        lineValue->setText(existing->value);
    }

    layout->addRow(u"Field"_s, comboField);
    layout->addRow(u"Operator"_s, comboOp);
    layout->addRow(u"Value"_s, lineValue);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);

    if (dlg->exec() != QDialog::Accepted) {
        dlg->deleteLater();
        return std::nullopt;
    }
    const QString field = comboField->currentText().trimmed();
    const QString op = comboOp->currentText();
    const QString value = lineValue->text().trimmed();
    dlg->deleteLater();
    if (field.isEmpty() || value.isEmpty()) {
        return std::nullopt;
    }
    return BackendTabularFilter{ field, op, value };
}

// Detect x/y/z coordinate columns by name (case-insensitive exact match).
static QString findCoordField(const std::vector<BackendVbtFieldInfo> &fields, const QString &name)
{
    for (const auto &f : fields) {
        if (f.name.compare(name, Qt::CaseInsensitive) == 0) {
            return f.name;
        }
    }
    return {};
}

vtkWindowVbt::vtkWindowVbt(BackendVbtOpenResult openResult, BackendVbtSubsetResult subsetResult,
                           const QString &backendUrl, const QString &backendToken,
                           const QString &sessionId, QWidget *parent)
    : QMainWindow(parent)
    , openResult(std::move(openResult))
    , subsetResult(std::move(subsetResult))
    , backendUrl(backendUrl)
    , backendToken(backendToken)
    , sessionId(sessionId)
{
    this->xFieldName = findCoordField(this->openResult.fields, u"x"_s);
    this->yFieldName = findCoordField(this->openResult.fields, u"y"_s);
    this->zFieldName = findCoordField(this->openResult.fields, u"z"_s);
    this->setupUi();
    this->setupRenderer();
    this->buildPointCloud();
    this->updateColorMapping();
    this->updateSummary();

    // ── Connect background filter watcher ─────────────────────────────────────
    QObject::connect(&this->filterWatcher, &QFutureWatcher<VbtFilterTaskResult>::finished, this,
                     [this]() {
                         this->filterBusy = false;
                         if (this->btnFilterApply) {
                             this->btnFilterApply->setEnabled(true);
                         }
                         if (this->btnFilterAdd) {
                             this->btnFilterAdd->setEnabled(true);
                         }
                         if (this->filterStatusLabel) {
                             this->filterStatusLabel->setText({});
                         }

                         const auto result = this->filterWatcher.result();
                         if (!result.valid) {
                             DiagnosticsManager::instance()->publish(
                                     DiagnosticsLevel::Warning, DiagnosticsCategory::Remote,
                                     QStringLiteral("VBT"),
                                     QStringLiteral("Filter failed: %1").arg(result.error),
                                     this->openResult.datasetId, this->sessionId,
                                     QStringLiteral("filter"));
                             QMessageBox::warning(this, u"Filter error"_s, result.error);
                             return;
                         }

                         this->subsetResult = result.subsetResult;

                         // Update pagination state
                         this->m_currentOffset = result.subsetResult.numRows;
                         this->m_totalCount = result.totalRows > 0 ? result.totalRows
                                                                    : result.subsetResult.numRows;
                         const bool hasMore = this->m_currentOffset < this->m_totalCount;
                         if (this->btnLoadMore) {
                             this->btnLoadMore->setVisible(hasMore);
                             this->btnLoadMore->setEnabled(true);
                             this->btnLoadMore->setText(
                                     QStringLiteral("Load more (%1 remaining)")
                                             .arg(this->m_totalCount - this->m_currentOffset));
                         }

                         if (this->filterStatusLabel) {
                             this->filterStatusLabel->setText(result.statusMsg);
                         }

                         if (this->actor) {
                             this->renderer->RemoveActor(this->actor.Get());
                         }
                         this->points->Reset();
                         this->pointScalars->Reset();
                         this->polyData->Reset();

                         this->buildPointCloud();
                         this->updateColorMapping();
                         this->updateSummary();

                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                 QStringLiteral("VBT"),
                                 QStringLiteral("Filter applied: %1 of %2 rows loaded.")
                                         .arg(result.subsetResult.numRows)
                                         .arg(this->m_totalCount),
                                 this->openResult.datasetId, this->sessionId,
                                 QStringLiteral("filter"));

                         this->renderWindow->Render();
                     });

    // ── Connect "load more" watcher ───────────────────────────────────────────
    QObject::connect(&this->m_loadMoreWatcher, &QFutureWatcher<VbtFilterTaskResult>::finished,
                     this, [this]() {
                         this->m_loadingMore = false;

                         const auto result = this->m_loadMoreWatcher.result();
                         if (!result.valid) {
                             if (this->filterStatusLabel) {
                                 this->filterStatusLabel->setText(
                                         u"Load more failed: "_s + result.error);
                             }
                             if (this->btnLoadMore) {
                                 this->btnLoadMore->setEnabled(true);
                                 this->btnLoadMore->setText(
                                         QStringLiteral("Load more (%1 remaining)")
                                                 .arg(this->m_totalCount - this->m_currentOffset));
                             }
                             return;
                         }

                         // Append columns to existing subsetResult
                         const auto &newCols = result.subsetResult.columns;
                         for (const auto &[fieldName, newVec] : newCols) {
                             auto &dest = this->subsetResult.columns[fieldName];
                             dest.insert(dest.end(), newVec.begin(), newVec.end());
                         }
                         this->subsetResult.numRows += result.subsetResult.numRows;
                         this->m_currentOffset = this->subsetResult.numRows;
                         this->m_totalCount = result.totalRows > 0 ? result.totalRows
                                                                    : this->m_totalCount;

                         const bool hasMore = this->m_currentOffset < this->m_totalCount;
                         if (this->btnLoadMore) {
                             this->btnLoadMore->setVisible(hasMore);
                             this->btnLoadMore->setEnabled(true);
                             this->btnLoadMore->setText(
                                     QStringLiteral("Load more (%1 remaining)")
                                             .arg(this->m_totalCount - this->m_currentOffset));
                         }

                         const QString statusMsg =
                                 QStringLiteral("Loaded %1 of %2 rows.")
                                         .arg(this->subsetResult.numRows)
                                         .arg(this->m_totalCount);
                         if (this->filterStatusLabel) {
                             this->filterStatusLabel->setText(statusMsg);
                         }

                         // Rebuild point cloud with all data
                         if (this->actor) {
                             this->renderer->RemoveActor(this->actor.Get());
                         }
                         this->points->Reset();
                         this->pointScalars->Reset();
                         this->polyData->Reset();

                         this->buildPointCloud();
                         this->updateColorMapping();
                         this->updateSummary();
                         this->renderWindow->Render();
                     });
}

void vtkWindowVbt::setupUi()
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(QStringLiteral("VBT Point Viewer — %1 rows, %2 fields")
                                 .arg(this->openResult.numRows)
                                 .arg(this->openResult.fields.size()));
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
    rootLayout->addWidget(this->vtkWidget, 1);

    // ── Signal connections ────────────────────────────────────────────────────
    QObject::connect(this->comboRenderMode, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateRenderMode(); });
    QObject::connect(this->comboColorField, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateColorMapping(); });
    QObject::connect(this->comboColormap, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateColorMapping(); });
    QObject::connect(this->comboBackground, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->applyBackground(); });
    QObject::connect(this->sliderPointSize, &QSlider::valueChanged, this,
                     [this](int) { this->updateRenderMode(); });
    QObject::connect(this->sliderRayIntensity, &QSlider::valueChanged, this,
                     [this](int) { this->updateRenderMode(); });
    QObject::connect(this->checkShowBox, &QCheckBox::toggled, this, [this](bool checked) {
        this->boxActor->SetVisibility(checked ? 1 : 0);
        this->renderWindow->Render();
    });
    QObject::connect(this->checkShowLut, &QCheckBox::toggled, this, [this](bool) {
        this->updateScalarBar();
        this->renderWindow->Render();
    });
    QObject::connect(this->checkShowOrientation, &QCheckBox::toggled, this, [this](bool checked) {
        this->axesWidget->SetEnabled(checked ? 1 : 0);
        this->renderWindow->Render();
    });
    QObject::connect(this->buttonAutoscale, &QPushButton::clicked, this, [this]() {
        this->setScalarRange(this->dataScalarMin, this->dataScalarMax);
        this->updateColorMapping();
    });
    QObject::connect(this->buttonResetView, &QPushButton::clicked, this,
                     [this]() { this->resetView(); });
    QObject::connect(this->spinRangeMin, &QDoubleSpinBox::valueChanged, this, [this](double) {
        if (this->updatingRangeControls) {
            return;
        }
        this->setScalarRange(this->spinRangeMin->value(), this->spinRangeMax->value());
        this->updateColorMapping();
    });
    QObject::connect(this->spinRangeMax, &QDoubleSpinBox::valueChanged, this, [this](double) {
        if (this->updatingRangeControls) {
            return;
        }
        this->setScalarRange(this->spinRangeMin->value(), this->spinRangeMax->value());
        this->updateColorMapping();
    });

    // Filter: Add
    QObject::connect(this->btnFilterAdd, &QPushButton::clicked, this, [this]() {
        QStringList fields;
        for (const auto &f : this->openResult.fields) {
            fields.append(f.name);
        }
        const auto filter = showAddFilterDialog(this, fields);
        if (!filter) {
            return;
        }
        this->activeFilters.push_back(*filter);
        this->filterList->addItem(
                QStringLiteral("%1 %2 %3").arg(filter->field, filter->op, filter->value));
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                QStringLiteral("Filter added: %1 %2 %3").arg(filter->field, filter->op,
                                                               filter->value),
                this->openResult.datasetId, this->sessionId, QStringLiteral("filter"));
    });
    // Filter: Edit on double-click
    QObject::connect(this->filterList, &QListWidget::itemDoubleClicked, this,
                     [this](QListWidgetItem *item) {
                         if (!item || !this->filterList) {
                             return;
                         }
                         const int row = this->filterList->row(item);
                         if (row < 0 || row >= static_cast<int>(this->activeFilters.size())) {
                             return;
                         }
                         QStringList fields;
                         for (const auto &f : this->openResult.fields) {
                             fields.append(f.name);
                         }
                         const auto updated =
                                 showAddFilterDialog(this, fields, &this->activeFilters[row]);
                         if (!updated) {
                             return;
                         }
                         this->activeFilters[row] = *updated;
                         item->setText(QStringLiteral("%1 %2 %3")
                                               .arg(updated->field, updated->op, updated->value));
                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                 QStringLiteral("VBT"),
                                 QStringLiteral("Filter [%1] edited: %2 %3 %4")
                                         .arg(row)
                                         .arg(updated->field, updated->op, updated->value),
                                 this->openResult.datasetId, this->sessionId,
                                 QStringLiteral("filter"));
                     });
    // Filter: Remove selected
    QObject::connect(this->btnFilterRemove, &QPushButton::clicked, this, [this]() {
        if (!this->filterList) {
            return;
        }
        const int row = this->filterList->currentRow();
        if (row < 0 || row >= static_cast<int>(this->activeFilters.size())) {
            return;
        }
        const BackendTabularFilter removed = this->activeFilters[row];
        this->activeFilters.erase(this->activeFilters.begin() + row);
        delete this->filterList->takeItem(row);
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                QStringLiteral("Filter removed: %1 %2 %3").arg(removed.field, removed.op,
                                                                removed.value),
                this->openResult.datasetId, this->sessionId, QStringLiteral("filter"));
    });
    // Filter: Clear all
    QObject::connect(this->btnFilterClearAll, &QPushButton::clicked, this, [this]() {
        const int count = static_cast<int>(this->activeFilters.size());
        this->activeFilters.clear();
        if (this->filterList) {
            this->filterList->clear();
        }
        if (count > 0) {
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                    QStringLiteral("%1 filter(s) cleared.").arg(count),
                    this->openResult.datasetId, this->sessionId, QStringLiteral("filter"));
        }
    });
    // Filter: Apply
    QObject::connect(this->btnFilterApply, &QPushButton::clicked, this, &vtkWindowVbt::applyFilter);

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

void vtkWindowVbt::setupSidebarUi(QWidget *parent)
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

    this->railBtnDisplay = new QToolButton(this->railWidget);
    this->railBtnDisplay->setToolTip(u"Display Settings"_s);
    this->railBtnDisplay->setCheckable(true);
    this->railBtnDisplay->setChecked(true);
    this->railBtnDisplay->setAutoRaise(true);

    this->railBtnFilters = new QToolButton(this->railWidget);
    this->railBtnFilters->setToolTip(u"Filters"_s);
    this->railBtnFilters->setCheckable(true);
    this->railBtnFilters->setAutoRaise(true);

    this->railBtnMetadata = new QToolButton(this->railWidget);
    this->railBtnMetadata->setToolTip(u"VBT Metadata"_s);
    this->railBtnMetadata->setCheckable(true);
    this->railBtnMetadata->setAutoRaise(true);

    railLayout->addWidget(this->btnToggleSidebar);
    railLayout->addWidget(this->railBtnDisplay);
    railLayout->addWidget(this->railBtnFilters);
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

    // ── Page 0: Display ───────────────────────────────────────────────────────
    auto *pageDisplay = new QWidget;
    auto *pageDisplayLayout = new QVBoxLayout(pageDisplay);
    pageDisplayLayout->setContentsMargins(8, 8, 8, 8);
    pageDisplayLayout->setSpacing(8);

    auto *hdrDisplay = new QLabel(u"DISPLAY SETTINGS"_s);
    auto *displayInner = new QWidget;
    auto *displayLayout = new QFormLayout(displayInner);
    displayLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    displayLayout->setContentsMargins(6, 6, 6, 6);
    displayLayout->setSpacing(4);

    this->comboRenderMode = new QComboBox(displayInner);
    this->comboRenderMode->addItems({ u"Points"_s, u"Particle Ray-Casting"_s });

    this->comboColorField = new QComboBox(displayInner);
    this->comboColorField->addItem(u"Solid color"_s, QString());
    for (const auto &field : this->openResult.fields) {
        if (field.name == this->xFieldName || field.name == this->yFieldName
            || field.name == this->zFieldName) {
            continue;
        }
        this->comboColorField->addItem(field.name, field.name);
    }
    if (this->comboColorField->count() > 1) {
        this->comboColorField->setCurrentIndex(1);
    }

    this->comboColormap = new QComboBox(displayInner);
    for (const auto &name : ColorMaps::GetColorMapNames()) {
        this->comboColormap->addItem(QString::fromStdString(name));
    }
    this->comboColormap->setCurrentText(QString::fromStdString(ColorMaps::DefaultColorMap));

    this->comboBackground = new QComboBox(displayInner);
    this->comboBackground->addItems({ u"Black"_s, u"Dark gray"_s, u"White"_s });

    this->sliderPointSize = new QSlider(Qt::Horizontal, displayInner);
    this->sliderPointSize->setRange(1, 12);
    this->sliderPointSize->setValue(3);
    this->sliderRayIntensity = new QSlider(Qt::Horizontal, displayInner);
    this->sliderRayIntensity->setRange(1, 100);
    this->sliderRayIntensity->setValue(35);

    this->checkShowBox = new QCheckBox(u"Show bounding box"_s, displayInner);
    this->checkShowBox->setChecked(true);
    this->checkShowLut = new QCheckBox(u"Show color bar"_s, displayInner);
    this->checkShowLut->setChecked(true);
    this->checkShowOrientation = new QCheckBox(u"Show orientation axes"_s, displayInner);
    this->checkShowOrientation->setChecked(true);

    this->spinRangeMin = new QDoubleSpinBox(displayInner);
    this->spinRangeMax = new QDoubleSpinBox(displayInner);
    this->spinRangeMin->setDecimals(6);
    this->spinRangeMax->setDecimals(6);
    this->spinRangeMin->setRange(-1e30, 1e30);
    this->spinRangeMax->setRange(-1e30, 1e30);

    this->buttonAutoscale = new QPushButton(u"Autoscale"_s, displayInner);
    this->buttonResetView = new QPushButton(u"Reset View"_s, displayInner);

    displayLayout->addRow(u"Mode"_s, this->comboRenderMode);
    displayLayout->addRow(u"Color field"_s, this->comboColorField);
    displayLayout->addRow(u"Colormap"_s, this->comboColormap);
    displayLayout->addRow(u"Range min"_s, this->spinRangeMin);
    displayLayout->addRow(u"Range max"_s, this->spinRangeMax);
    {
        auto *row = new QWidget(displayInner);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->addWidget(this->buttonAutoscale);
        rl->addWidget(this->buttonResetView);
        displayLayout->addRow(row);
    }
    displayLayout->addRow(u"Background"_s, this->comboBackground);
    displayLayout->addRow(u"Point size"_s, this->sliderPointSize);
    displayLayout->addRow(u"Ray intensity"_s, this->sliderRayIntensity);
    displayLayout->addRow(this->checkShowBox);
    displayLayout->addRow(this->checkShowLut);
    displayLayout->addRow(this->checkShowOrientation);

    auto *sep = new QFrame(displayInner);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    displayLayout->addRow(sep);

    this->summaryLabel = new QLabel(displayInner);
    this->summaryLabel->setWordWrap(true);
    this->summaryLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    displayLayout->addRow(this->summaryLabel);

    pageDisplayLayout->addWidget(hdrDisplay);
    pageDisplayLayout->addWidget(displayInner);
    pageDisplayLayout->addStretch(1);
    this->sidebarStack->addWidget(pageDisplay); // index 0

    // ── Page 1: Filters ───────────────────────────────────────────────────────
    auto *pageFilters = new QWidget;
    auto *pageFiltersLayout = new QVBoxLayout(pageFilters);
    pageFiltersLayout->setContentsMargins(8, 8, 8, 8);
    pageFiltersLayout->setSpacing(8);

    auto *hdrFilters = new QLabel(u"FILTERS"_s);
    pageFiltersLayout->addWidget(hdrFilters);

    this->filterList = new QListWidget(pageFilters);
    this->filterList->setFixedHeight(120);
    this->filterList->setSelectionMode(QAbstractItemView::SingleSelection);
    this->filterList->setToolTip(
            u"Active filters combined with AND. Select a row and press Remove to delete it."_s);
    pageFiltersLayout->addWidget(this->filterList);

    {
        auto *btnRow = new QWidget(pageFilters);
        auto *btnLayout = new QHBoxLayout(btnRow);
        btnLayout->setContentsMargins(0, 0, 0, 0);
        btnLayout->setSpacing(4);
        this->btnFilterAdd = new QPushButton(u"Add…"_s, btnRow);
        this->btnFilterRemove = new QPushButton(u"Remove"_s, btnRow);
        this->btnFilterClearAll = new QPushButton(u"Clear All"_s, btnRow);
        this->btnFilterApply = new QPushButton(u"Apply"_s, btnRow);
        btnLayout->addWidget(this->btnFilterAdd);
        btnLayout->addWidget(this->btnFilterRemove);
        btnLayout->addWidget(this->btnFilterClearAll);
        btnLayout->addStretch(1);
        btnLayout->addWidget(this->btnFilterApply);
        pageFiltersLayout->addWidget(btnRow);
    }

    this->filterStatusLabel = new QLabel(pageFilters);
    this->filterStatusLabel->setWordWrap(true);
    pageFiltersLayout->addWidget(this->filterStatusLabel);

    this->btnLoadMore = new QPushButton(u"Load more"_s, pageFilters);
    this->btnLoadMore->setVisible(false);
    this->btnLoadMore->setToolTip(
            u"Fetch the next page of results and append them to the current point cloud."_s);
    pageFiltersLayout->addWidget(this->btnLoadMore);
    QObject::connect(this->btnLoadMore, &QPushButton::clicked, this,
                     &vtkWindowVbt::loadMoreEntries);

    pageFiltersLayout->addStretch(1);
    this->sidebarStack->addWidget(pageFilters); // index 1

    // ── Page 2: Metadata ──────────────────────────────────────────────────────
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
    this->sidebarStack->addWidget(pageMetadata); // index 2

    // ── Styles ────────────────────────────────────────────────────────────────
    setupSidebarRail();
    setupRailIcons();
    setupSidebarPages();

    // ── Rail connections ──────────────────────────────────────────────────────
    QObject::connect(this->btnToggleSidebar, &QToolButton::clicked, this,
                     &vtkWindowVbt::toggleSidebar);
    QObject::connect(this->railBtnDisplay, &QToolButton::clicked, this,
                     [this]() { switchToPage(0); });
    QObject::connect(this->railBtnFilters, &QToolButton::clicked, this,
                     [this]() { switchToPage(1); });
    QObject::connect(this->railBtnMetadata, &QToolButton::clicked, this,
                     [this]() { switchToPage(2); });

    setActiveRailButton(this->railBtnDisplay);
    this->sidebarStack->setCurrentIndex(0);
}

void vtkWindowVbt::setupSidebarRail()
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

void vtkWindowVbt::setupRailIcons()
{
    struct Entry { QToolButton *btn; QString path; };
    const std::array<Entry, 4> entries = { {
        { this->btnToggleSidebar, u":/icons/rail_slice.svg"_s    },
        { this->railBtnDisplay,   u":/icons/rail_display.svg"_s  },
        { this->railBtnFilters,   u":/icons/rail_filter.svg"_s   },
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

void vtkWindowVbt::setupSidebarPages()
{
    const QString hdrStyle =
            u"QLabel { color: #8AB8CC; font-size: 10px; font-weight: 600;"
            u"         padding-bottom: 4px; border-bottom: 1px solid rgba(4,138,191,0.20); }"_s;
    // Apply to page header labels (3 of them, found by iterating pages)
    for (int i = 0; i < this->sidebarStack->count(); ++i) {
        auto *page = this->sidebarStack->widget(i);
        const auto labels = page->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly);
        if (!labels.isEmpty()) {
            labels.first()->setStyleSheet(hdrStyle);
        }
    }
}

void vtkWindowVbt::setActiveRailButton(QToolButton *btn)
{
    for (QToolButton *b : { this->railBtnDisplay.data(), this->railBtnFilters.data(),
                            this->railBtnMetadata.data() }) {
        if (b) {
            b->setProperty("active", b == btn);
            b->style()->polish(b);
        }
    }
}

void vtkWindowVbt::switchToPage(int index)
{
    if (this->sidebarExpanded) {
        if (this->sidebarStack) {
            this->sidebarStack->setCurrentIndex(index);
        }
        QToolButton *btn = nullptr;
        if (index == 0) { btn = this->railBtnDisplay; }
        else if (index == 1) { btn = this->railBtnFilters; }
        else { btn = this->railBtnMetadata; }
        setActiveRailButton(btn);
    } else {
        this->pendingPageIndex = index;
        setSidebarExpanded(true);
    }
}

void vtkWindowVbt::toggleSidebar()
{
    setSidebarExpanded(!this->sidebarExpanded);
}

void vtkWindowVbt::setSidebarExpanded(bool expanded, bool animated)
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
            QToolButton *btn = nullptr;
            if (this->pendingPageIndex == 0) { btn = this->railBtnDisplay; }
            else if (this->pendingPageIndex == 1) { btn = this->railBtnFilters; }
            else { btn = this->railBtnMetadata; }
            setActiveRailButton(btn);
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
            QToolButton *btn = nullptr;
            if (this->pendingPageIndex == 0) { btn = this->railBtnDisplay; }
            else if (this->pendingPageIndex == 1) { btn = this->railBtnFilters; }
            else { btn = this->railBtnMetadata; }
            setActiveRailButton(btn);
            this->pendingPageIndex = -1;
        }
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void vtkWindowVbt::applyFilter()
{
    if (this->openResult.datasetId.isEmpty() || this->filterBusy) {
        return;
    }

    // Reset pagination state for a fresh query
    this->m_currentOffset = 0;
    this->m_totalCount = -1;

    this->filterBusy = true;
    if (this->btnFilterApply) {
        this->btnFilterApply->setEnabled(false);
    }
    if (this->btnFilterAdd) {
        this->btnFilterAdd->setEnabled(false);
    }
    if (this->btnLoadMore) {
        this->btnLoadMore->setVisible(false);
    }
    if (this->filterStatusLabel) {
        this->filterStatusLabel->setText(u"Applying filters…"_s);
    }

    const int nFilters = static_cast<int>(this->activeFilters.size());
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
            nFilters == 0 ? QStringLiteral("Querying dataset (no active filters).")
                          : QStringLiteral("Applying %1 filter(s).").arg(nFilters),
            this->openResult.datasetId, this->sessionId, QStringLiteral("filter"));

    const QString backendUrl = this->backendUrl;
    const QString backendToken = this->backendToken;
    const QString datasetId = this->openResult.datasetId;
    const QString sessionId = this->sessionId;
    const std::vector<BackendTabularFilter> filters = this->activeFilters;
    const int pageSize = this->m_pageSize;

    this->filterWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, datasetId, sessionId, filters, nFilters, pageSize]()
                    -> VbtFilterTaskResult {
                VbtFilterTaskResult out;

                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) {
                    client.setSessionId(sessionId);
                }
                BackendTabularQueryRequest qreq;
                qreq.datasetId = datasetId;
                qreq.filters = filters;
                qreq.limit = pageSize;
                qreq.offset = 0;

                const auto queryResult = client.queryTabularVbt(qreq);
                if (!queryResult.valid) {
                    out.error = queryResult.error;
                    return out;
                }

                out.subsetResult = {};
                out.subsetResult.valid = true;
                out.subsetResult.kind = QStringLiteral("point");
                out.subsetResult.numRows = queryResult.numRows;
                out.subsetResult.fieldNames = queryResult.fieldNames;
                out.subsetResult.columns = queryResult.columns;
                out.totalRows = queryResult.totalRows;

                out.statusMsg =
                        filters.empty()
                                ? QStringLiteral("Loaded %1 of %2 rows.")
                                          .arg(queryResult.returnedRows)
                                          .arg(queryResult.totalRows)
                                : QStringLiteral("%1 filter(s) active — %2 of %3 rows.")
                                          .arg(nFilters)
                                          .arg(queryResult.returnedRows)
                                          .arg(queryResult.totalRows);
                out.valid = true;
                return out;
            }));
}

void vtkWindowVbt::loadMoreEntries()
{
    if (this->openResult.datasetId.isEmpty() || this->m_loadingMore || this->filterBusy) {
        return;
    }
    if (this->m_totalCount >= 0 && this->m_currentOffset >= this->m_totalCount) {
        return;
    }

    this->m_loadingMore = true;
    if (this->btnLoadMore) {
        this->btnLoadMore->setEnabled(false);
        this->btnLoadMore->setText(u"Loading…"_s);
    }
    if (this->filterStatusLabel) {
        this->filterStatusLabel->setText(
                QStringLiteral("Loading rows %1–%2…")
                        .arg(this->m_currentOffset + 1)
                        .arg(this->m_currentOffset + this->m_pageSize));
    }

    const QString backendUrl = this->backendUrl;
    const QString backendToken = this->backendToken;
    const QString datasetId = this->openResult.datasetId;
    const QString sessionId = this->sessionId;
    const std::vector<BackendTabularFilter> filters = this->activeFilters;
    const int offset = this->m_currentOffset;
    const int pageSize = this->m_pageSize;

    this->m_loadMoreWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, datasetId, sessionId, filters, offset, pageSize]()
                    -> VbtFilterTaskResult {
                VbtFilterTaskResult out;

                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) {
                    client.setSessionId(sessionId);
                }
                BackendTabularQueryRequest qreq;
                qreq.datasetId = datasetId;
                qreq.filters = filters;
                qreq.limit = pageSize;
                qreq.offset = offset;

                const auto queryResult = client.queryTabularVbt(qreq);
                if (!queryResult.valid) {
                    out.error = queryResult.error;
                    return out;
                }

                out.subsetResult = {};
                out.subsetResult.valid = true;
                out.subsetResult.kind = QStringLiteral("point");
                out.subsetResult.numRows = queryResult.numRows;
                out.subsetResult.fieldNames = queryResult.fieldNames;
                out.subsetResult.columns = queryResult.columns;
                out.totalRows = queryResult.totalRows;
                out.valid = true;
                return out;
            }));
}

void vtkWindowVbt::setupRenderer()
{
    this->vtkWidget->setRenderWindow(this->renderWindow.Get());
    this->vtkWidget->setEnableTouchEventProcessing(false);
    this->renderer->SetBackground(0.06, 0.06, 0.1);
    this->renderWindow->AddRenderer(this->renderer);

    auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
    this->vtkWidget->interactor()->SetInteractorStyle(style);

    vtkNew<vtkAxesActor> axesActor;
    this->axesWidget->SetOrientationMarker(axesActor);
    this->axesWidget->SetInteractor(this->vtkWidget->interactor());
    this->axesWidget->SetViewport(0.0, 0.0, 0.14, 0.14);
    this->axesWidget->EnabledOn();
    this->axesWidget->InteractiveOff();

    this->scalarBar->SetLookupTable(this->scalarLut.Get());
    this->scalarBar->SetMaximumWidthInPixels(100);
    this->scalarBar->SetPosition(0.88, 0.1);
    this->scalarBar->GetTitleTextProperty()->SetColor(1.0, 1.0, 1.0);
    this->scalarBar->GetLabelTextProperty()->SetColor(1.0, 1.0, 1.0);
    this->scalarBar->VisibilityOff();
    this->renderer->AddViewProp(this->scalarBar.Get());

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
    this->renderer->AddActor(this->boxActor.Get());

    this->gaussianMapper->SetInputData(this->polyData.Get());
    // ── Particle (Gaussian splat) rendering configuration ───────────────────
    //
    // SetEmissive(true): additive blending (GL_SRC_ALPHA, GL_ONE) + glDepthMask(GL_FALSE).
    //
    //   WHY EMISSIVE IS REQUIRED FOR MORPHOLOGY:
    //   With EmissiveOff, depth writes (glDepthMask=GL_TRUE) are active.  The nearest
    //   splat at any screen pixel writes its depth; all particles behind it fail the
    //   depth test → only the front shell of the cloud is visible → collapses to a
    //   "tiny blob" even for a sparse filamentary cloud.
    //
    //   With EmissiveOn, every particle at a given pixel contributes additively to
    //   screen brightness regardless of depth.  Dense regions accumulate to be bright;
    //   sparse filaments remain dim but visible.  This matches the spatial distribution
    //   seen in Points mode.
    //
    //   WHY THIS WORKS ON macOS/QVTKOpenGLNativeWidget:
    //   QVTKOpenGLNativeWidget renders to a *native* OpenGL surface, not a Qt
    //   QOpenGLWidget FBO.  There is no Qt alpha-compositing step that could hide
    //   additive output.  The "emissive invisible" failure in previous attempts was
    //   caused by an empty ScalarOpacityFunction returning opacity=0 for all points,
    //   not by any FBO/alpha-compositing issue.
    //
    this->gaussianMapper->SetEmissive(true);
    //
    // BoundScale=3: render the Gaussian out to 3σ (opacity drops to 1.1% at edge).
    // Smoother falloff than BoundScale=2 (13.5% at edge), reduces the visible "disc"
    // artefact when splats are isolated in sparse regions.
    this->gaussianMapper->SetBoundScale(3.0f);
    this->gaussianMapper->SetLookupTable(this->scalarLut.Get());
    //
    // Per-splat opacity is controlled via the documented VTK API:
    //   SetOpacityArray("VbtScalar") → use the scalar array as the lookup key
    //   SetScalarOpacityFunction     → piecewise function mapping key → per-splat opacity
    // The function is a flat constant configured density-adaptively in updateRenderMode().
    // Using this API path is more reliable than actor->GetProperty()->SetOpacity() which
    // vtkPointGaussianMapper may not honour consistently across VTK builds.
    this->gaussianMapper->SetOpacityArray("VbtScalar");
    this->gaussianMapper->SetScalarOpacityFunction(this->gaussianOpacityFunction.Get());
}

void vtkWindowVbt::buildPointCloud()
{
    const std::size_t rowCount = static_cast<std::size_t>(this->subsetResult.numRows);
    this->points->SetNumberOfPoints(static_cast<vtkIdType>(rowCount));
    this->pointScalars->SetName("VbtScalar");
    this->pointScalars->SetNumberOfTuples(static_cast<vtkIdType>(rowCount));

    static const std::vector<double> emptyCol{};
    const auto &xcol = this->subsetResult.columns.count(this->xFieldName)
            ? this->subsetResult.columns.at(this->xFieldName)
            : emptyCol;
    const auto &ycol = this->subsetResult.columns.count(this->yFieldName)
            ? this->subsetResult.columns.at(this->yFieldName)
            : emptyCol;
    const auto &zcol = this->subsetResult.columns.count(this->zFieldName)
            ? this->subsetResult.columns.at(this->zFieldName)
            : emptyCol;

    vtkNew<vtkCellArray> verts;
    for (vtkIdType row = 0; row < static_cast<vtkIdType>(rowCount); ++row) {
        const auto i = static_cast<std::size_t>(row);
        const double x = i < xcol.size() ? xcol[i] : 0.0;
        const double y = i < ycol.size() ? ycol[i] : 0.0;
        const double z = i < zcol.size() ? zcol[i] : 0.0;
        this->points->SetPoint(row, x, y, z);
        this->pointScalars->SetValue(row, 0.0);
        verts->InsertNextCell(1, &row);
    }

    this->polyData->SetPoints(this->points.Get());
    this->polyData->SetVerts(verts);
    this->polyData->GetPointData()->SetScalars(this->pointScalars.Get());

    this->scalarLut->SetNumberOfTableValues(256);
    this->scalarLut->SetHueRange(0.67, 0.0);
    this->scalarLut->SetSaturationRange(0.85, 0.95);
    this->scalarLut->SetValueRange(0.9, 1.0);
    this->scalarLut->Build();

    this->mapper->SetInputData(this->polyData.Get());
    this->actor->SetMapper(this->mapper.Get());
    this->actor->GetProperty()->SetRepresentationToPoints();
    this->actor->GetProperty()->SetPointSize(this->sliderPointSize->value());
    this->actor->GetProperty()->SetColor(0.8, 0.85, 1.0);
    this->renderer->AddActor(this->actor.Get());
    this->updateBoundsContext();

    // Diagnostic: log point count, bounds and coordinate field mapping
    {
        double bounds[6] = {0., 0., 0., 0., 0., 0.};
        this->polyData->GetBounds(bounds);
        qDebug() << "[VBT] buildPointCloud:" << rowCount << "points"
                 << "| bounds X" << bounds[0] << "-" << bounds[1]
                 << "Y" << bounds[2] << "-" << bounds[3]
                 << "Z" << bounds[4] << "-" << bounds[5]
                 << "| X=" << (this->xFieldName.isEmpty() ? QStringLiteral("(none)") : this->xFieldName)
                 << "Y=" << (this->yFieldName.isEmpty() ? QStringLiteral("(none)") : this->yFieldName)
                 << "Z=" << (this->zFieldName.isEmpty() ? QStringLiteral("(none)") : this->zFieldName);
    }

    this->updateRenderMode();
    this->resetView();
}

void vtkWindowVbt::updateRenderMode()
{
    if (!this->actor) {
        return;
    }
    const bool rayCasting = this->comboRenderMode && this->comboRenderMode->currentIndex() == 1;
    if (rayCasting) {
        // ── Density-adaptive splat sizing ─────────────────────────────────────
        // Goal: every particle contributes to the rendered image at every depth
        // (depth writes are disabled in emissive mode), so the full cloud volume
        // and its filamentary structure are preserved.
        //
        // Radius derivation:
        //   spacing  = maxDim / cbrtN  ← mean inter-particle distance along longest axis
        //   radius   = spacing * sliderVal / 12
        //
        // At default sliderVal=3: radius = spacing/4.
        // Two adjacent filament points separated by spacing/2 (denser-than-average
        // filament) have their splat centres 2×radius apart → Gaussian value at
        // midpoint = exp(-0.5*(d/σ)²) = exp(-0.5*(2)²) = exp(-2) ≈ 0.135 of peak.
        // With additive blending both splats contribute → midpoint = 0.27 of peak,
        // so filaments at ½ global spacing appear as connected structures.
        // At sliderVal=12: radius = spacing → very wide splats that merge clusters;
        // useful for inspecting large-scale density only.
        //
        double bounds[6] = {0., 0., 0., 0., 0., 0.};
        this->polyData->GetBounds(bounds);
        const double dimX = bounds[1] - bounds[0];
        const double dimY = bounds[3] - bounds[2];
        const double dimZ = bounds[5] - bounds[4];
        const double maxDim = std::max({dimX, dimY, dimZ, 1.0e-6});
        const vtkIdType npts = this->polyData->GetNumberOfPoints();
        const double cbrtN = npts > 0 ? std::cbrt(static_cast<double>(npts)) : 1.0;
        const double spacing = maxDim / cbrtN;
        const int sliderVal = this->sliderPointSize ? this->sliderPointSize->value() : 3;
        // slider 1–12: radius = 12%–150% of mean spacing
        // (was /12 → /8: 1.5× larger so adjacent filament points at ≤ 0.75×spacing connect)
        const double radius = spacing * sliderVal / 8.0;

        // ── Per-splat opacity ─────────────────────────────────────────────────
        // Design target: a single isolated particle must be clearly visible, i.e.
        //   1 × perSplatOpacity ≥ 0.15  (15% brightness on the dark background).
        //
        // Previous formula (3.0/cbrtN) was calibrated for the AVERAGE sightline density
        // (N_sightline ≈ 0.375×cbrtN), so perSplatOpacity ≈ 8/cbrtN.  For N=100k
        // (cbrtN=46.4) at default intensity=35%: perSplatOpacity ≈ 0.022.
        // In a filamentary cloud, SPARSE sightlines have 1–3 particles → brightness
        // 0.02–0.07, invisible against the 6% background → "faint central spot".
        //
        // Fix: use baseOpacity = min(0.9, 20.0/cbrtN), which gives perSplatOpacity ≈ 0.15
        // for a single particle at default intensity=35% across the expected dataset range.
        //
        //   N=10k  (cbrtN=21.5): baseOpacity = min(0.9, 0.93) = 0.90; at 35% → 0.315
        //   N=100k (cbrtN=46.4): baseOpacity = min(0.9, 0.43) = 0.43; at 35% → 0.151
        //   N=1M   (cbrtN=100):  baseOpacity = min(0.9, 0.20) = 0.20; at 35% → 0.070
        //
        // Dense cores (N_sightline ≫ 1) saturate at default settings — this is the
        // expected and desirable behaviour for a density/emission renderer.  The intensity
        // slider reduces perSplatOpacity to trade core contrast for cloud-edge brightness.
        //
        // Clamp: lower bound 0.05 ensures sparse pixels always show something;
        //        upper bound 0.9 allows sparse small datasets to render visibly.
        const double intensity = (this->sliderRayIntensity ? this->sliderRayIntensity->value() : 35) / 100.0;
        const double baseOpacity = std::min(0.9, 20.0 / cbrtN);
        const double perSplatOpacity = std::clamp(baseOpacity * intensity, 0.05, 0.9);

        // Flat opacity function: same opacity for every scalar value so that the
        // rendering is uniform across solid-colour mode (VbtScalar=0) and scalar-
        // colour modes (VbtScalar in [min,max]).  Range [-1e15, 1e15] covers any value.
        this->gaussianOpacityFunction->RemoveAllPoints();
        this->gaussianOpacityFunction->AddPoint(-1.0e15, perSplatOpacity);
        this->gaussianOpacityFunction->AddPoint( 1.0e15, perSplatOpacity);

        this->gaussianMapper->SetScaleFactor(radius);
        qDebug().noquote()
                << QStringLiteral(
                           "[VBT] rayCasting: npts=%1 cbrtN=%2 spacing=%3 radius=%4 "
                           "sliderVal=%5 intensity=%6 perSplatOpacity=%7 "
                           "emissive=%8 scalarVis=%9 boundScale=%10")
                           .arg(npts)
                           .arg(cbrtN, 0, 'f', 1)
                           .arg(spacing, 0, 'g', 4)
                           .arg(radius, 0, 'g', 4)
                           .arg(sliderVal)
                           .arg(intensity, 0, 'f', 2)
                           .arg(perSplatOpacity, 0, 'f', 4)
                           .arg(this->gaussianMapper->GetEmissive())
                           .arg(this->gaussianMapper->GetScalarVisibility())
                           .arg(this->gaussianMapper->GetBoundScale());
        this->actor->SetMapper(this->gaussianMapper.Get());
        // Actor property opacity = 1.0: VTK treats the actor as opaque for render-pass
        // selection.  Individual splat opacity is driven entirely by gaussianOpacityFunction.
        this->actor->GetProperty()->SetOpacity(1.0);
    } else {
        this->actor->SetMapper(this->mapper.Get());
        this->actor->GetProperty()->SetOpacity(1.0);
        this->actor->GetProperty()->SetPointSize(this->sliderPointSize ? this->sliderPointSize->value() : 3);
    }
    this->renderWindow->Render();
    this->updateSummary();
}

QString vtkWindowVbt::activeColorFieldName() const
{
    if (!this->comboColorField) {
        return QStringLiteral("solid");
    }
    return this->comboColorField->currentIndex() <= 0
            ? QStringLiteral("solid")
            : this->comboColorField->currentText();
}

QString vtkWindowVbt::activeColormapName() const
{
    return this->comboColormap ? this->comboColormap->currentText()
                               : QString::fromStdString(ColorMaps::DefaultColorMap);
}

void vtkWindowVbt::setScalarRange(double minValue, double maxValue)
{
    if (!(minValue < maxValue)) {
        minValue = this->dataScalarMin;
        maxValue = this->dataScalarMax;
        if (!(minValue < maxValue)) {
            maxValue = minValue + 1.0;
        }
    }
    this->activeScalarMin = minValue;
    this->activeScalarMax = maxValue;
    this->updatingRangeControls = true;
    if (this->spinRangeMin) {
        this->spinRangeMin->setValue(minValue);
    }
    if (this->spinRangeMax) {
        this->spinRangeMax->setValue(maxValue);
    }
    this->updatingRangeControls = false;
}

void vtkWindowVbt::updateColorMapping()
{
    if (!this->comboColorField) {
        return;
    }

    const QVariant data = this->comboColorField->currentData();
    if (!data.isValid() || data.toString().isEmpty()) {
        // Solid-color mode: scalar visibility off but ensure gaussian functions
        // produce a visible constant output so ray-casting mode still renders.
        this->mapper->ScalarVisibilityOff();
        this->gaussianMapper->ScalarVisibilityOff();
        this->actor->GetProperty()->SetColor(0.8, 0.85, 1.0);
        qDebug() << "[VBT] updateColorMapping: solid color mode";
        this->updateScalarBar();
        this->updateRenderMode();
        this->renderWindow->Render();
        this->updateSummary();
        return;
    }

    const QString fieldName = data.toString();
    const auto it = this->subsetResult.columns.find(fieldName);
    if (it == this->subsetResult.columns.end()) {
        // Field missing (e.g. after a filter that dropped the column) —
        // fall back to solid-color gaussian functions so ray-casting still renders.
        this->mapper->ScalarVisibilityOff();
        this->gaussianMapper->ScalarVisibilityOff();
        qDebug() << "[VBT] updateColorMapping: field" << fieldName << "not found in subset columns";
        this->updateScalarBar();
        this->updateRenderMode();
        this->renderWindow->Render();
        this->updateSummary();
        return;
    }

    qDebug() << "[VBT] updateColorMapping: active field =" << fieldName;

    double minValue = std::numeric_limits<double>::max();
    double maxValue = std::numeric_limits<double>::lowest();
    const auto &column = it->second;
    for (vtkIdType row = 0; row < static_cast<vtkIdType>(column.size()); ++row) {
        const double value = column[static_cast<std::size_t>(row)];
        this->pointScalars->SetValue(row, value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
    }
    if (!(minValue < maxValue)) {
        maxValue = minValue + 1.0;
    }
    this->dataScalarMin = minValue;
    this->dataScalarMax = maxValue;
    // Reset the active range when: the range is invalid, OR the new data extends
    // beyond the current range on either side.  This ensures that after clearing
    // all filters (returning a wider dataset) the opacity function is rebuilt for
    // the full data range so no points are accidentally mapped to opacity = 0.
    const bool newDataFitsCurrent = (this->activeScalarMin < this->activeScalarMax)
                                    && (minValue >= this->activeScalarMin)
                                    && (maxValue <= this->activeScalarMax);
    if (!newDataFitsCurrent) {
        this->setScalarRange(minValue, maxValue);
    }
    this->pointScalars->Modified();
    this->polyData->GetPointData()->SetScalars(this->pointScalars.Get());
    ColorMaps::SetColorMap(this->scalarLut.Get(), this->activeColormapName().toStdString());
    this->scalarLut->SetTableRange(this->activeScalarMin, this->activeScalarMax);
    this->scalarLut->Build();
    this->mapper->SetLookupTable(this->scalarLut.Get());
    this->mapper->SetScalarRange(this->activeScalarMin, this->activeScalarMax);
    this->mapper->ScalarVisibilityOn();
    this->gaussianMapper->SetLookupTable(this->scalarLut.Get());
    this->gaussianMapper->SetScalarRange(this->activeScalarMin, this->activeScalarMax);
    this->gaussianMapper->ScalarVisibilityOn();

    this->updateScalarBar();
    this->updateRenderMode();
    this->renderWindow->Render();
    this->updateSummary();
}

void vtkWindowVbt::updateScalarBar()
{
    const bool visible = this->mapper->GetScalarVisibility()
            && this->checkShowLut && this->checkShowLut->isChecked();
    this->scalarBar->SetVisibility(visible ? 1 : 0);
    if (visible) {
        this->scalarBar->SetTitle(this->activeColorFieldName().toUtf8().constData());
        const bool darkBackground = !this->comboBackground || this->comboBackground->currentText() != u"White"_s;
        const double textColor = darkBackground ? 1.0 : 0.1;
        this->scalarBar->GetTitleTextProperty()->SetColor(textColor, textColor, textColor);
        this->scalarBar->GetLabelTextProperty()->SetColor(textColor, textColor, textColor);
    }
}

void vtkWindowVbt::updateBoundsContext()
{
    double bounds[6] = { 0., 0., 0., 0., 0., 0. };
    this->polyData->GetBounds(bounds);
    this->boxActor->SetBounds(bounds);
    this->boxActor->SetVisibility(this->checkShowBox && this->checkShowBox->isChecked() ? 1 : 0);
}

void vtkWindowVbt::applyBackground()
{
    if (!this->comboBackground) {
        return;
    }
    const QString background = this->comboBackground->currentText();
    if (background == u"White"_s) {
        this->renderer->SetBackground(0.98, 0.98, 0.98);
    } else if (background == u"Dark gray"_s) {
        this->renderer->SetBackground(0.16, 0.17, 0.19);
    } else {
        this->renderer->SetBackground(0.06, 0.06, 0.1);
    }
    this->updateScalarBar();
    this->renderWindow->Render();
}

void vtkWindowVbt::resetView()
{
    this->renderer->ResetCamera();
    this->renderer->ResetCameraClippingRange();
    this->renderWindow->Render();
}

void vtkWindowVbt::updateSummary()
{
    if (this->summaryLabel) {
        this->summaryLabel->setText(
                QStringLiteral("Rows: %1\nFields: %2\nScalar type: %3\nMode: %4\nActive color: %5\nColormap: %6\nRange: [%7, %8]\nPoint size: %9\nRay intensity: %10")
                        .arg(this->openResult.numRows)
                        .arg(this->openResult.fields.size())
                        .arg(this->openResult.scalarType)
                        .arg(this->comboRenderMode ? this->comboRenderMode->currentText() : QStringLiteral("Points"))
                        .arg(this->activeColorFieldName())
                        .arg(this->activeColormapName())
                        .arg(this->activeScalarMin, 0, 'g', 6)
                        .arg(this->activeScalarMax, 0, 'g', 6)
                        .arg(this->sliderPointSize ? this->sliderPointSize->value() : 0)
                        .arg(this->sliderRayIntensity ? this->sliderRayIntensity->value() : 0));
    }
    if (this->metadataLabel) {
        this->metadataLabel->setText(
                QStringLiteral("Kind: %1\nFields: %2\nRows: %3\nScalar type: %4\nCoordinates: X=%5 Y=%6 Z=%7")
                        .arg(this->openResult.kind)
                        .arg(this->openResult.fields.size())
                        .arg(this->openResult.numRows)
                        .arg(this->openResult.scalarType)
                        .arg(this->xFieldName.isEmpty() ? u"(none)"_s : this->xFieldName)
                        .arg(this->yFieldName.isEmpty() ? u"(none)"_s : this->yFieldName)
                        .arg(this->zFieldName.isEmpty() ? u"(none)"_s : this->zFieldName));
    }
}

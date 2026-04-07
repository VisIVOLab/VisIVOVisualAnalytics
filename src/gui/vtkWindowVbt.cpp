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

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QDebug>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <optional>

#include <algorithm>
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
}

void vtkWindowVbt::setupUi()
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(QStringLiteral("VBT Point Viewer — %1 rows, %2 fields")
                                 .arg(this->openResult.numRows)
                                 .arg(this->openResult.fields.size()));
    this->resize(1280, 800);

    // ── Central: full-window VTK viewport ────────────────────────────────────
    this->vtkWidget = new QVTKOpenGLNativeWidget(this);
    this->setCentralWidget(this->vtkWidget);

    // ── Display dock (right) — visualization controls ─────────────────────────
    auto *displayDock = new QDockWidget(u"Display"_s, this);
    displayDock->setObjectName(u"VbtDisplayDock"_s);
    displayDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                             | QDockWidget::DockWidgetClosable);

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

    auto *displayScroll = new QScrollArea;
    displayScroll->setWidgetResizable(true);
    displayScroll->setWidget(displayInner);
    displayScroll->setMinimumWidth(240);
    displayScroll->setMaximumWidth(300);
    displayScroll->setFrameShape(QFrame::NoFrame);
    displayDock->setWidget(displayScroll);
    this->addDockWidget(Qt::RightDockWidgetArea, displayDock);

    // ── Filters dock (bottom) — multi-filter list ─────────────────────────────
    auto *filterDock = new QDockWidget(u"Filters"_s, this);
    filterDock->setObjectName(u"VbtFilterDock"_s);
    filterDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                            | QDockWidget::DockWidgetClosable);

    auto *filterWidget = new QWidget;
    auto *filterVLayout = new QVBoxLayout(filterWidget);
    filterVLayout->setContentsMargins(6, 4, 6, 4);
    filterVLayout->setSpacing(4);

    this->filterList = new QListWidget(filterWidget);
    this->filterList->setFixedHeight(88);
    this->filterList->setSelectionMode(QAbstractItemView::SingleSelection);
    this->filterList->setToolTip(
            u"Active filters combined with AND. Select a row and press Remove to delete it."_s);
    filterVLayout->addWidget(this->filterList);

    {
        auto *btnRow = new QWidget(filterWidget);
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
        filterVLayout->addWidget(btnRow);
    }

    this->filterStatusLabel = new QLabel(filterWidget);
    filterVLayout->addWidget(this->filterStatusLabel);

    filterDock->setWidget(filterWidget);
    this->addDockWidget(Qt::BottomDockWidgetArea, filterDock);

    // ── Metadata dock (tabbed with Filters) ──────────────────────────────────
    this->metadataDock = new QDockWidget(u"VBT Metadata"_s, this);
    this->metadataDock->setObjectName(u"VbtMetadataDock"_s);
    auto *dockWidget = new QWidget(this->metadataDock);
    auto *dockLayout = new QVBoxLayout(dockWidget);
    this->metadataLabel = new QLabel(dockWidget);
    this->metadataLabel->setWordWrap(true);
    this->fieldList = new QListWidget(dockWidget);
    for (const auto &field : this->openResult.fields) {
        this->fieldList->addItem(QStringLiteral("%1 [%2]").arg(field.name, field.type));
    }
    dockLayout->addWidget(this->metadataLabel);
    dockLayout->addWidget(this->fieldList, 1);
    this->metadataDock->setWidget(dockWidget);
    this->addDockWidget(Qt::BottomDockWidgetArea, this->metadataDock);
    this->tabifyDockWidget(filterDock, this->metadataDock);
    filterDock->raise();

    // ── Signal connections ────────────────────────────────────────────────────
    QObject::connect(this->comboRenderMode, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateRenderMode(); });
    QObject::connect(this->comboColorField, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateColorMapping(); });
    QObject::connect(this->comboColormap, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateColorMapping(); });
    QObject::connect(this->comboBackground, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->applyBackground(); });
    QObject::connect(this->sliderPointSize, &QSlider::valueChanged, this, [this](int value) {
        if (this->actor) {
            this->actor->GetProperty()->SetPointSize(value);
            if (this->gaussianMapper) {
                this->gaussianMapper->SetScaleFactor(static_cast<double>(value));
            }
            this->renderWindow->Render();
            this->updateSummary();
        }
    });
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

    // ── View menu — reopen closed docks ──────────────────────────────────────
    auto *viewMenu = this->menuBar()->addMenu(u"View"_s);
    displayDock->toggleViewAction()->setText(u"Display Settings"_s);
    viewMenu->addAction(displayDock->toggleViewAction());
    filterDock->toggleViewAction()->setText(u"Filters"_s);
    viewMenu->addAction(filterDock->toggleViewAction());
    this->metadataDock->toggleViewAction()->setText(u"VBT Metadata"_s);
    viewMenu->addAction(this->metadataDock->toggleViewAction());
}

void vtkWindowVbt::applyFilter()
{
    if (this->openResult.datasetId.isEmpty()) {
        return;
    }
    BackendClient client(this->backendUrl, this->backendToken);
    if (!this->sessionId.isEmpty()) {
        client.setSessionId(this->sessionId);
    }
    BackendTabularQueryRequest qreq;
    qreq.datasetId = this->openResult.datasetId;
    qreq.filters = this->activeFilters; // all rows, AND semantics

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
            this->activeFilters.empty()
                    ? QStringLiteral("Querying dataset (no active filters).")
                    : QStringLiteral("Applying %1 filter(s).").arg(this->activeFilters.size()),
            this->openResult.datasetId, this->sessionId, QStringLiteral("filter"));

    const auto result = client.queryTabularVbt(qreq);
    if (!result.valid) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                QStringLiteral("Filter query failed: %1").arg(result.error),
                this->openResult.datasetId, this->sessionId, QStringLiteral("filter"));
        QMessageBox::warning(this, u"Filter error"_s, result.error);
        return;
    }

    // Rebuild subsetResult from query response
    this->subsetResult = {};
    this->subsetResult.valid = true;
    this->subsetResult.kind = QStringLiteral("point");
    this->subsetResult.numRows = result.numRows;
    this->subsetResult.fieldNames = result.fieldNames;
    this->subsetResult.columns = result.columns;

    // Remove existing point cloud actors; keep axes/scalar bar/box intact
    if (this->actor) {
        this->renderer->RemoveActor(this->actor.Get());
    }
    this->points->Reset();
    this->pointScalars->Reset();
    this->polyData->Reset();

    this->buildPointCloud();
    this->updateColorMapping();
    this->updateSummary();

    const QString statusMsg =
            this->activeFilters.empty()
                    ? QStringLiteral("No filters active — showing all %1 rows.").arg(result.totalRows)
                    : QStringLiteral("%1 filter(s) active — showing %2 of %3 rows.")
                              .arg(this->activeFilters.size())
                              .arg(result.returnedRows)
                              .arg(result.totalRows);
    if (this->filterStatusLabel) {
        this->filterStatusLabel->setText(statusMsg);
    }

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
            QStringLiteral("Filter applied: %1 of %2 rows match.")
                    .arg(result.returnedRows)
                    .arg(result.totalRows),
            this->openResult.datasetId, this->sessionId, QStringLiteral("filter"));

    this->renderWindow->Render();
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
    this->gaussianMapper->SetScaleArray("VbtScalar");
    this->gaussianMapper->SetOpacityArray("VbtScalar");
    this->gaussianMapper->SetScaleFunction(this->gaussianScaleFunction.Get());
    this->gaussianMapper->SetScalarOpacityFunction(this->gaussianOpacityFunction.Get());
    this->gaussianMapper->SetScaleFactor(static_cast<double>(this->sliderPointSize->value()));
    this->gaussianMapper->SetEmissive(true);
    this->gaussianMapper->SetBoundScale(3.0f);
    this->gaussianMapper->SetLookupTable(this->scalarLut.Get());
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
        // Derive splat radius as a fraction of the dataset's bounding-box extent so
        // that the scale is data-independent and always visible regardless of units.
        double bounds[6] = {0., 0., 0., 0., 0., 0.};
        this->polyData->GetBounds(bounds);
        const double maxExt = std::max({bounds[1] - bounds[0],
                                        bounds[3] - bounds[2],
                                        bounds[5] - bounds[4],
                                        1.0e-6});
        const int sliderVal = this->sliderPointSize ? this->sliderPointSize->value() : 6;
        // slider 1-12: maps to 0.1 – 1.2 % of max extent
        this->gaussianMapper->SetScaleFactor(maxExt * sliderVal / 1000.0);

        const double intensity = (this->sliderRayIntensity ? this->sliderRayIntensity->value() : 35) / 100.0;
        this->actor->SetMapper(this->gaussianMapper.Get());
        this->actor->GetProperty()->SetOpacity(std::clamp(0.25 + intensity * 0.75, 0.05, 1.0));
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
        const double intensity = (this->sliderRayIntensity ? this->sliderRayIntensity->value() : 35) / 100.0;
        this->gaussianScaleFunction->RemoveAllPoints();
        this->gaussianScaleFunction->AddPoint(0.0, 1.0);
        this->gaussianScaleFunction->AddPoint(1.0, 1.0);
        this->gaussianOpacityFunction->RemoveAllPoints();
        this->gaussianOpacityFunction->AddPoint(0.0, intensity);
        this->gaussianOpacityFunction->AddPoint(1.0, intensity);
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
        const double intensity = (this->sliderRayIntensity ? this->sliderRayIntensity->value() : 35) / 100.0;
        this->gaussianScaleFunction->RemoveAllPoints();
        this->gaussianScaleFunction->AddPoint(0.0, 1.0);
        this->gaussianScaleFunction->AddPoint(1.0, 1.0);
        this->gaussianOpacityFunction->RemoveAllPoints();
        this->gaussianOpacityFunction->AddPoint(0.0, intensity);
        this->gaussianOpacityFunction->AddPoint(1.0, intensity);
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

    this->gaussianScaleFunction->RemoveAllPoints();
    this->gaussianOpacityFunction->RemoveAllPoints();
    const double range = this->activeScalarMax - this->activeScalarMin;
    const double p20 = this->activeScalarMin + range * 0.20;
    const double p60 = this->activeScalarMin + range * 0.60;
    const double p90 = this->activeScalarMin + range * 0.90;
    const double intensity = (this->sliderRayIntensity ? this->sliderRayIntensity->value() : 35) / 100.0;
    this->gaussianScaleFunction->AddPoint(this->activeScalarMin, 0.6);
    this->gaussianScaleFunction->AddPoint(p60, 1.0);
    this->gaussianScaleFunction->AddPoint(this->activeScalarMax, 1.6);
    this->gaussianOpacityFunction->AddPoint(this->activeScalarMin, 0.0);
    this->gaussianOpacityFunction->AddPoint(p20, 0.02 * intensity);
    this->gaussianOpacityFunction->AddPoint(p60, 0.10 * intensity);
    this->gaussianOpacityFunction->AddPoint(p90, 0.30 * intensity);
    this->gaussianOpacityFunction->AddPoint(this->activeScalarMax, 0.65 * intensity);

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

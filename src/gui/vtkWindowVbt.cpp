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
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
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
#include <QPushButton>
#include "SidebarPanel.h"
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

/// Build a gradient QIcon that previews a named colormap (80×14 px).
static QIcon makeColormapIcon(const std::string &name, int iconW = 80, int iconH = 14)
{
    vtkNew<vtkLookupTable> lut;
    lut->SetNumberOfTableValues(256);
    lut->SetTableRange(0.0, 1.0);
    ColorMaps::SetColorMap(lut.Get(), name);
    lut->Build();
    QImage img(iconW, iconH, QImage::Format_RGB32);
    for (int x = 0; x < iconW; ++x) {
        const int tableIdx = static_cast<int>(x * 255.0 / std::max(1, iconW - 1));
        double rgba[4];
        lut->GetTableValue(tableIdx, rgba);
        const QRgb c = qRgb(static_cast<int>(rgba[0] * 255.0),
                            static_cast<int>(rgba[1] * 255.0),
                            static_cast<int>(rgba[2] * 255.0));
        for (int y = 0; y < iconH; ++y)
            img.setPixel(x, y, c);
    }
    return QIcon(QPixmap::fromImage(img));
}

/// Returns true when @p errorText looks like a backend session-expiry error.
static bool isSessionExpiryError(const QString &errorText)
{
    const QString lower = errorText.toLower();
    return lower.contains(u"session"_s)
        || lower.contains(u"expired"_s)
        || lower.contains(u"not found"_s);
}

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
                           const QString &sessionId, const QString &headerPath,
                           QWidget *parent)
    : QMainWindow(parent)
    , openResult(std::move(openResult))
    , subsetResult(std::move(subsetResult))
    , backendUrl(backendUrl)
    , backendToken(backendToken)
    , sessionId(sessionId)
    , m_headerPath(headerPath)
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
                             if (isSessionExpiryError(result.error)) {
                                 QMessageBox msg(this);
                                 msg.setWindowTitle(u"Session Expired"_s);
                                 msg.setText(
                                         u"The backend session for this dataset has expired.\n"
                                         "The dataset needs to be reopened before continuing."_s);
                                 auto *btnReopen = msg.addButton(
                                         u"Re-open Dataset"_s, QMessageBox::AcceptRole);
                                 msg.addButton(QMessageBox::Cancel);
                                 msg.exec();
                                 if (msg.clickedButton() == btnReopen) {
                                     const QString path = this->m_headerPath;
                                     emit reopenRequested(path);
                                     this->close();
                                 }
                             } else {
                                 QMessageBox::warning(this, u"Filter error"_s, result.error);
                             }
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
                             if (isSessionExpiryError(result.error)) {
                                 QMessageBox msg(this);
                                 msg.setWindowTitle(u"Session Expired"_s);
                                 msg.setText(
                                         u"The backend session for this dataset has expired.\n"
                                         "The dataset needs to be reopened before continuing."_s);
                                 auto *btnReopen = msg.addButton(
                                         u"Re-open Dataset"_s, QMessageBox::AcceptRole);
                                 msg.addButton(QMessageBox::Cancel);
                                 msg.exec();
                                 if (msg.clickedButton() == btnReopen) {
                                     const QString path = this->m_headerPath;
                                     emit reopenRequested(path);
                                     this->close();
                                 }
                             } else {
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

    this->setupSidebarUi();
    rootLayout->addWidget(this->m_sidebar);

    this->vtkWidget = new QVTKOpenGLNativeWidget(central);
    rootLayout->addWidget(this->vtkWidget, 1);

    // ── Signal connections ────────────────────────────────────────────────────
    QObject::connect(this->comboRenderMode, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateRenderMode(); });
    // X / Y / Z mapping combos — rebuild point coordinates when changed
    QObject::connect(this->comboXField, &QComboBox::currentIndexChanged, this, [this](int) {
        this->xFieldName = this->comboXField->currentText();
        this->rebuildCoordinates();
    });
    QObject::connect(this->comboYField, &QComboBox::currentIndexChanged, this, [this](int) {
        this->yFieldName = this->comboYField->currentText();
        this->rebuildCoordinates();
    });
    QObject::connect(this->comboZField, &QComboBox::currentIndexChanged, this, [this](int) {
        this->zFieldName = this->comboZField->currentText();
        this->rebuildCoordinates();
    });
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
    this->actionShowControlsSidebar = this->m_sidebar->createToggleAction(
            u"Controls Sidebar"_s, this);
    viewMenu->addAction(this->actionShowControlsSidebar);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sidebar implementation
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowVbt::setupSidebarUi()
{
    // ── Shared style helpers (same palette as catalogue viewer) ──────────────
    const QString hdrStyle =
            u"QLabel { color: #8AB8CC; font-size: 10px; font-weight: 600;"
            u"         padding-bottom: 4px; border-bottom: 1px solid rgba(4,138,191,0.20); }"_s;
    const QString cardStyle =
            u"QFrame, QGroupBox { background-color: rgba(255,255,255,0.03);"
            u"  border: 1px solid rgba(4,138,191,0.14); border-radius: 6px; }"_s;
    const QString subHdrStyle =
            u"QLabel { color: #8AB8CC; font-size: 9px; font-weight: 600; }"_s;
    const QString keyStyle =
            u"QLabel { color: #607090; font-size: 10px; }"_s;
    const QString valStyle =
            u"QLabel { color: #c0d0e0; font-size: 10px; font-family: monospace; }"_s;

    // makeCard: card frame with a section title header inside (used for Display page).
    const auto makeCard = [&](const QString &title, QWidget *content) -> QFrame* {
        auto *card = new QFrame;
        card->setObjectName(u"card"_s);
        card->setStyleSheet(cardStyle);
        auto *vl = new QVBoxLayout(card);
        vl->setSpacing(6);
        vl->setContentsMargins(10, 8, 10, 10);
        auto *lbl = new QLabel(title);
        lbl->setStyleSheet(
                u"QLabel { color: #8AB8CC; font-size: 10px; font-weight: 600;"
                u"         padding-bottom: 4px; border-bottom: 1px solid rgba(4,138,191,0.20);"
                u"         border-radius: 0; border-left: none; border-right: none;"
                u"         border-top: none; background: transparent; }"_s);
        vl->addWidget(lbl);
        vl->addWidget(content);
        return card;
    };

    // makeValueLabel: right-aligned, selectable, monospace value label.
    const auto makeValLabel = [&valStyle](QWidget *parent = nullptr) -> QLabel* {
        auto *lbl = new QLabel(u"—"_s, parent);
        lbl->setStyleSheet(valStyle);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return lbl;
    };

    // ── Page 0: Display ───────────────────────────────────────────────────────
    auto *pageDisplay = new QWidget;
    auto *pageDisplayLayout = new QVBoxLayout(pageDisplay);
    pageDisplayLayout->setContentsMargins(8, 8, 8, 8);
    pageDisplayLayout->setSpacing(8);

    // Card: Source
    {
        auto *inner = new QWidget;
        auto *fl = new QFormLayout(inner);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);
        this->comboRenderMode = new QComboBox(inner);
        this->comboRenderMode->addItems({ u"Points"_s, u"Particle Ray-Casting"_s });
        fl->addRow(u"Mode"_s, this->comboRenderMode);
        pageDisplayLayout->addWidget(makeCard(u"SOURCE"_s, inner));
    }

    // Card: Mapping
    {
        auto *inner = new QWidget;
        auto *fl = new QFormLayout(inner);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);

        QStringList allFieldNames;
        for (const auto &f : this->openResult.fields) allFieldNames << f.name;

        this->comboXField = new QComboBox(inner);
        this->comboYField = new QComboBox(inner);
        this->comboZField = new QComboBox(inner);
        this->comboXField->addItems(allFieldNames);
        this->comboYField->addItems(allFieldNames);
        this->comboZField->addItems(allFieldNames);
        if (!this->xFieldName.isEmpty()) this->comboXField->setCurrentText(this->xFieldName);
        if (!this->yFieldName.isEmpty()) this->comboYField->setCurrentText(this->yFieldName);
        if (!this->zFieldName.isEmpty()) this->comboZField->setCurrentText(this->zFieldName);

        this->comboColorField = new QComboBox(inner);
        this->comboColorField->addItem(u"Solid color"_s, QString());
        for (const auto &field : this->openResult.fields) {
            if (field.name == this->xFieldName || field.name == this->yFieldName
                || field.name == this->zFieldName)
                continue;
            this->comboColorField->addItem(field.name, field.name);
        }
        if (this->comboColorField->count() > 1)
            this->comboColorField->setCurrentIndex(1);

        fl->addRow(u"X"_s, this->comboXField);
        fl->addRow(u"Y"_s, this->comboYField);
        fl->addRow(u"Z"_s, this->comboZField);
        fl->addRow(u"Scalar"_s, this->comboColorField);
        pageDisplayLayout->addWidget(makeCard(u"MAPPING"_s, inner));
    }

    // Card: Color
    {
        auto *inner = new QWidget;
        auto *fl = new QFormLayout(inner);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);

        this->comboColormap = new QComboBox(inner);
        this->comboColormap->setIconSize(QSize(80, 14));
        for (const auto &name : ColorMaps::GetColorMapNames()) {
            this->comboColormap->addItem(makeColormapIcon(name), QString::fromStdString(name));
        }
        this->comboColormap->setCurrentText(QString::fromStdString(ColorMaps::DefaultColorMap));

        this->spinRangeMin = new QDoubleSpinBox(inner);
        this->spinRangeMax = new QDoubleSpinBox(inner);
        this->spinRangeMin->setDecimals(6);
        this->spinRangeMax->setDecimals(6);
        this->spinRangeMin->setRange(-1e30, 1e30);
        this->spinRangeMax->setRange(-1e30, 1e30);
        this->buttonAutoscale = new QPushButton(u"Autoscale"_s, inner);
        this->buttonResetView = new QPushButton(u"Reset View"_s, inner);

        fl->addRow(u"Colormap"_s, this->comboColormap);
        fl->addRow(u"Range min"_s, this->spinRangeMin);
        fl->addRow(u"Range max"_s, this->spinRangeMax);
        {
            auto *btnRow = new QWidget(inner);
            auto *rl = new QHBoxLayout(btnRow);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->addWidget(this->buttonAutoscale);
            rl->addWidget(this->buttonResetView);
            fl->addRow(btnRow);
        }
        pageDisplayLayout->addWidget(makeCard(u"COLOR"_s, inner));
    }

    // Card: Scene
    {
        auto *inner = new QWidget;
        auto *fl = new QFormLayout(inner);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);

        this->comboBackground = new QComboBox(inner);
        this->comboBackground->addItems({ u"Black"_s, u"Dark gray"_s, u"White"_s });

        this->sliderPointSize = new QSlider(Qt::Horizontal, inner);
        this->sliderPointSize->setRange(1, 12);
        this->sliderPointSize->setValue(3);

        this->sliderRayIntensity = new QSlider(Qt::Horizontal, inner);
        this->sliderRayIntensity->setRange(1, 100);
        this->sliderRayIntensity->setValue(35);
        auto *rayLabel = new QLabel(u"Ray intensity"_s);
        this->m_labelRayIntensity = rayLabel;

        this->checkShowBox = new QCheckBox(u"Show bounding box"_s, inner);
        this->checkShowBox->setChecked(true);
        this->checkShowLut = new QCheckBox(u"Show color bar"_s, inner);
        this->checkShowLut->setChecked(true);
        this->checkShowOrientation = new QCheckBox(u"Show orientation axes"_s, inner);
        this->checkShowOrientation->setChecked(true);

        fl->addRow(u"Background"_s, this->comboBackground);
        fl->addRow(u"Point size"_s, this->sliderPointSize);
        fl->addRow(rayLabel, this->sliderRayIntensity);
        fl->addRow(this->checkShowBox);
        fl->addRow(this->checkShowLut);
        fl->addRow(this->checkShowOrientation);
        pageDisplayLayout->addWidget(makeCard(u"SCENE"_s, inner));
    }

    this->summaryLabel = new QLabel(pageDisplay);
    this->summaryLabel->setWordWrap(true);
    this->summaryLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    this->summaryLabel->setStyleSheet(u"color: #6080a0; font-size: 10px; padding: 4px;"_s);
    pageDisplayLayout->addWidget(this->summaryLabel);
    pageDisplayLayout->addStretch(1);

    // ── Page 1: Filters (same structure as catalogue viewer) ─────────────────
    auto *pageFilters = new QWidget;
    auto *pageFiltersLayout = new QVBoxLayout(pageFilters);
    pageFiltersLayout->setContentsMargins(8, 8, 8, 8);
    pageFiltersLayout->setSpacing(8);

    {
        auto *hdr = new QLabel(u"FILTERS"_s, pageFilters);
        hdr->setStyleSheet(hdrStyle);
        pageFiltersLayout->addWidget(hdr);
    }

    // Card 1: Active Filters
    {
        auto *card = new QFrame(pageFilters);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *listHdr = new QLabel(u"Active filters"_s, card);
        listHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(listHdr);

        this->filterList = new QListWidget(card);
        this->filterList->setSelectionMode(QAbstractItemView::SingleSelection);
        this->filterList->setToolTip(
                u"Active filters combined with AND. Double-click a row to edit it."_s);
        cl->addWidget(this->filterList);

        this->filterStatusLabel = new QLabel(card);
        this->filterStatusLabel->setWordWrap(true);
        this->filterStatusLabel->setStyleSheet(u"QLabel { color: #8AB8CC; font-size: 9px; }"_s);
        cl->addWidget(this->filterStatusLabel);

        this->btnLoadMore = new QPushButton(u"Load more…"_s, card);
        this->btnLoadMore->setVisible(false);
        this->btnLoadMore->setToolTip(
                u"Fetch the next page of results and append them to the current point cloud."_s);
        cl->addWidget(this->btnLoadMore);

        pageFiltersLayout->addWidget(card);
    }
    QObject::connect(this->btnLoadMore, &QPushButton::clicked, this,
                     &vtkWindowVbt::loadMoreEntries);

    // Card 2: Actions
    {
        auto *card = new QFrame(pageFilters);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        this->btnFilterAdd      = new QPushButton(u"Add…"_s,      card);
        this->btnFilterRemove   = new QPushButton(u"Remove"_s,    card);
        this->btnFilterClearAll = new QPushButton(u"Clear All"_s, card);
        this->btnFilterApply    = new QPushButton(u"Apply"_s,     card);
        cl->addWidget(this->btnFilterAdd);
        cl->addWidget(this->btnFilterRemove);
        cl->addWidget(this->btnFilterClearAll);
        cl->addWidget(this->btnFilterApply);

        pageFiltersLayout->addWidget(card);
    }
    pageFiltersLayout->addStretch(1);

    // ── Page 2: Info / Stats ──────────────────────────────────────────────────
    auto *pageInfo = new QWidget;
    auto *pageInfoLayout = new QVBoxLayout(pageInfo);
    pageInfoLayout->setContentsMargins(8, 8, 8, 8);
    pageInfoLayout->setSpacing(8);

    {
        auto *hdr = new QLabel(u"INFO / STATS"_s, pageInfo);
        hdr->setStyleSheet(hdrStyle);
        pageInfoLayout->addWidget(hdr);
    }

    // Card 1: Dataset Summary (static + 2 dynamic rows)
    {
        auto *card = new QFrame(pageInfo);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Dataset Summary"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        auto *inner = new QWidget(card);
        auto *fl = new QFormLayout(inner);
        fl->setContentsMargins(0, 4, 0, 0);
        fl->setSpacing(3);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        // Static rows (set once at construction)
        const QString fileName = this->m_headerPath.isEmpty()
                ? u"—"_s : QFileInfo(this->m_headerPath).fileName();
        auto *fileVal = new QLabel(fileName, inner);
        fileVal->setStyleSheet(valStyle);
        fileVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        fileVal->setTextInteractionFlags(Qt::TextSelectableByMouse);
        fileVal->setWordWrap(true);

        auto *rowsVal = new QLabel(
                QStringLiteral("%1").arg(this->openResult.numRows), inner);
        rowsVal->setStyleSheet(valStyle);
        rowsVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *fieldsVal = new QLabel(
                QStringLiteral("%1").arg(this->openResult.fields.size()), inner);
        fieldsVal->setStyleSheet(valStyle);
        fieldsVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *kindVal = new QLabel(this->openResult.kind, inner);
        kindVal->setStyleSheet(valStyle);
        kindVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        // Dynamic rows (updated by updateSummary)
        this->m_infoModeVal  = makeValLabel(inner);
        this->m_infoColorVal = makeValLabel(inner);

        auto *k1 = new QLabel(u"File"_s, inner); k1->setStyleSheet(keyStyle);
        auto *k2 = new QLabel(u"Rows"_s, inner); k2->setStyleSheet(keyStyle);
        auto *k3 = new QLabel(u"Fields"_s, inner); k3->setStyleSheet(keyStyle);
        auto *k4 = new QLabel(u"Kind"_s, inner); k4->setStyleSheet(keyStyle);
        auto *k5 = new QLabel(u"Mode"_s, inner); k5->setStyleSheet(keyStyle);
        auto *k6 = new QLabel(u"Color field"_s, inner); k6->setStyleSheet(keyStyle);

        fl->addRow(k1, fileVal);
        fl->addRow(k2, rowsVal);
        fl->addRow(k3, fieldsVal);
        fl->addRow(k4, kindVal);
        fl->addRow(k5, this->m_infoModeVal);
        fl->addRow(k6, this->m_infoColorVal);
        cl->addWidget(inner);
        pageInfoLayout->addWidget(card);
    }

    // Card 2: Mapping Summary (all dynamic)
    {
        auto *card = new QFrame(pageInfo);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Mapping"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        auto *inner = new QWidget(card);
        auto *fl = new QFormLayout(inner);
        fl->setContentsMargins(0, 4, 0, 0);
        fl->setSpacing(3);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        this->m_infoXVal      = makeValLabel(inner);
        this->m_infoYVal      = makeValLabel(inner);
        this->m_infoZVal      = makeValLabel(inner);
        this->m_infoScalarVal = makeValLabel(inner);

        auto *kx = new QLabel(u"X"_s, inner); kx->setStyleSheet(keyStyle);
        auto *ky = new QLabel(u"Y"_s, inner); ky->setStyleSheet(keyStyle);
        auto *kz = new QLabel(u"Z"_s, inner); kz->setStyleSheet(keyStyle);
        auto *ks = new QLabel(u"Scalar"_s, inner); ks->setStyleSheet(keyStyle);

        fl->addRow(kx, this->m_infoXVal);
        fl->addRow(ky, this->m_infoYVal);
        fl->addRow(kz, this->m_infoZVal);
        fl->addRow(ks, this->m_infoScalarVal);
        cl->addWidget(inner);
        pageInfoLayout->addWidget(card);
    }

    // Card 3: Data Range / Statistics (all dynamic)
    {
        auto *card = new QFrame(pageInfo);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Data Range / Statistics"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        auto *inner = new QWidget(card);
        auto *fl = new QFormLayout(inner);
        fl->setContentsMargins(0, 4, 0, 0);
        fl->setSpacing(3);
        fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        this->m_infoDataMinVal   = makeValLabel(inner);
        this->m_infoDataMaxVal   = makeValLabel(inner);
        this->m_infoMeanVal      = makeValLabel(inner);
        this->m_infoRangeMinVal  = makeValLabel(inner);
        this->m_infoRangeMaxVal  = makeValLabel(inner);

        auto *r1 = new QLabel(u"Data min"_s,    inner); r1->setStyleSheet(keyStyle);
        auto *r2 = new QLabel(u"Data max"_s,    inner); r2->setStyleSheet(keyStyle);
        auto *r3 = new QLabel(u"Mean"_s,        inner); r3->setStyleSheet(keyStyle);
        auto *r4 = new QLabel(u"Display min"_s, inner); r4->setStyleSheet(keyStyle);
        auto *r5 = new QLabel(u"Display max"_s, inner); r5->setStyleSheet(keyStyle);

        fl->addRow(r1, this->m_infoDataMinVal);
        fl->addRow(r2, this->m_infoDataMaxVal);
        fl->addRow(r3, this->m_infoMeanVal);
        fl->addRow(r4, this->m_infoRangeMinVal);
        fl->addRow(r5, this->m_infoRangeMaxVal);
        cl->addWidget(inner);
        pageInfoLayout->addWidget(card);
    }

    // Card 4: Available Fields
    {
        auto *card = new QFrame(pageInfo);
        card->setStyleSheet(cardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *cardHdr = new QLabel(u"Available Fields"_s, card);
        cardHdr->setStyleSheet(subHdrStyle);
        cl->addWidget(cardHdr);

        this->fieldList = new QListWidget(card);
        this->fieldList->setStyleSheet(
                u"QListWidget { background: transparent; border: none; font-size: 10px; }"_s);
        for (const auto &field : this->openResult.fields)
            this->fieldList->addItem(QStringLiteral("%1  [%2]").arg(field.name, field.type));
        cl->addWidget(this->fieldList);
        pageInfoLayout->addWidget(card);
    }

    // metadataLabel kept for internal use (updateSummary compatibility), hidden
    this->metadataLabel = new QLabel(pageInfo);
    this->metadataLabel->hide();
    pageInfoLayout->addWidget(this->metadataLabel);
    pageInfoLayout->addStretch(1);

    // ── Wire pages into SidebarPanel ──────────────────────────────────────────
    this->m_sidebar = new SidebarPanel(320, this);
    this->m_sidebar->addPage(u":/icons/rail_display.svg"_s, u"Display"_s,    pageDisplay);
    this->m_sidebar->addPage(u":/icons/rail_filter.svg"_s,  u"Filters"_s,   pageFilters);
    this->m_sidebar->addPage(u":/icons/rail_stats.svg"_s,   u"Info / Stats"_s, pageInfo);
    this->m_sidebar->finalize(0);
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

void vtkWindowVbt::rebuildCoordinates()
{
    if (!this->actor) {
        return;
    }
    this->renderer->RemoveActor(this->actor.Get());
    this->points->Reset();
    this->pointScalars->Reset();
    this->polyData->Reset();
    this->buildPointCloud();
    this->updateColorMapping();
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
    this->applyBoxActorColors(true); // default: dark background
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

    // Loading placeholder — visible until buildPointCloud() completes.
    {
        auto *prop = this->m_loadingActor->GetTextProperty();
        prop->SetFontSize(16);
        prop->SetColor(0.72, 0.84, 0.91);
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
    }
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

    this->m_loadingActor->VisibilityOff();
    this->updateRenderMode();
    this->resetView();
}

void vtkWindowVbt::updateRenderMode()
{
    if (!this->actor) {
        return;
    }
    const bool rayCasting = this->comboRenderMode && this->comboRenderMode->currentIndex() == 1;

    // Show Ray Intensity controls only in Particle Ray-Casting mode (MOD 3)
    if (this->m_labelRayIntensity) this->m_labelRayIntensity->setVisible(rayCasting);
    if (this->sliderRayIntensity)  this->sliderRayIntensity->setVisible(rayCasting);
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
        this->m_lastColorField.clear();
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
        this->m_lastColorField.clear();
        this->updateScalarBar();
        this->updateRenderMode();
        this->renderWindow->Render();
        this->updateSummary();
        return;
    }

    qDebug() << "[VBT] updateColorMapping: active field =" << fieldName;

    double minValue = std::numeric_limits<double>::max();
    double maxValue = std::numeric_limits<double>::lowest();
    double sumValue = 0.0;
    const auto &column = it->second;
    for (vtkIdType row = 0; row < static_cast<vtkIdType>(column.size()); ++row) {
        const double value = column[static_cast<std::size_t>(row)];
        this->pointScalars->SetValue(row, value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        sumValue += value;
    }
    if (!(minValue < maxValue)) {
        maxValue = minValue + 1.0;
    }
    this->dataScalarMin = minValue;
    this->dataScalarMax = maxValue;
    this->dataScalarMean = column.empty() ? 0.0 : sumValue / static_cast<double>(column.size());
    // Auto-reset range only when the field changes or the current range is invalid.
    // This allows the user to set a custom (narrower) range via the spinboxes without
    // it being immediately overwritten by the data bounds (MOD 4 fix).
    const bool fieldChanged = (fieldName != this->m_lastColorField);
    const bool rangeInvalid = !(this->activeScalarMin < this->activeScalarMax);
    if (fieldChanged || rangeInvalid) {
        this->setScalarRange(minValue, maxValue);
    }
    this->m_lastColorField = fieldName;
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
    const bool dark = (background != u"White"_s);
    if (background == u"White"_s) {
        this->renderer->SetBackground(0.98, 0.98, 0.98);
    } else if (background == u"Dark gray"_s) {
        this->renderer->SetBackground(0.16, 0.17, 0.19);
    } else {
        this->renderer->SetBackground(0.06, 0.06, 0.1);
    }
    this->applyBoxActorColors(dark);
    this->updateScalarBar();
    this->renderWindow->Render();
}

void vtkWindowVbt::applyBoxActorColors(bool darkBackground)
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

void vtkWindowVbt::resetView()
{
    this->renderer->ResetCamera();
    this->renderer->ResetCameraClippingRange();
    this->renderWindow->Render();
}

void vtkWindowVbt::updateSummary()
{
    // ── Display page: compact status line ────────────────────────────────────
    if (this->summaryLabel) {
        const QString colorField = this->activeColorFieldName();
        this->summaryLabel->setText(
                QStringLiteral("%1 pts  ·  %2  ·  %3")
                        .arg(this->subsetResult.numRows)
                        .arg(this->comboRenderMode ? this->comboRenderMode->currentText()
                                                   : QStringLiteral("Points"))
                        .arg(colorField == u"solid"_s ? u"solid color"_s : colorField));
    }

    // ── Info page: Card 1 — dynamic rows ─────────────────────────────────────
    const QString modeTxt = this->comboRenderMode
            ? this->comboRenderMode->currentText() : QStringLiteral("Points");
    const QString colorTxt = this->activeColorFieldName();

    if (this->m_infoModeVal)  this->m_infoModeVal->setText(modeTxt);
    if (this->m_infoColorVal) this->m_infoColorVal->setText(
            colorTxt == u"solid"_s ? u"Solid color"_s : colorTxt);

    // ── Info page: Card 2 — Mapping ───────────────────────────────────────────
    const auto fieldOrNone = [](const QString &s) {
        return s.isEmpty() ? QStringLiteral("—") : s;
    };
    if (this->m_infoXVal)      this->m_infoXVal->setText(fieldOrNone(this->xFieldName));
    if (this->m_infoYVal)      this->m_infoYVal->setText(fieldOrNone(this->yFieldName));
    if (this->m_infoZVal)      this->m_infoZVal->setText(fieldOrNone(this->zFieldName));
    if (this->m_infoScalarVal) this->m_infoScalarVal->setText(
            colorTxt == u"solid"_s ? u"—"_s : colorTxt);

    // ── Info page: Card 3 — Data Range / Stats ────────────────────────────────
    const bool hasData = (this->dataScalarMin < this->dataScalarMax
                          || this->subsetResult.numRows > 0);
    if (this->m_infoDataMinVal)
        this->m_infoDataMinVal->setText(
                hasData ? QString::number(this->dataScalarMin, 'g', 7) : u"—"_s);
    if (this->m_infoDataMaxVal)
        this->m_infoDataMaxVal->setText(
                hasData ? QString::number(this->dataScalarMax, 'g', 7) : u"—"_s);
    if (this->m_infoMeanVal)
        this->m_infoMeanVal->setText(
                hasData ? QString::number(this->dataScalarMean, 'g', 7) : u"—"_s);
    if (this->m_infoRangeMinVal)
        this->m_infoRangeMinVal->setText(QString::number(this->activeScalarMin, 'g', 7));
    if (this->m_infoRangeMaxVal)
        this->m_infoRangeMaxVal->setText(QString::number(this->activeScalarMax, 'g', 7));
}

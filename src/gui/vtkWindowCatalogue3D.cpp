#include "vtkWindowCatalogue3D.h"
#include "ui_vtkWindowCatalogue3D.h"

#include "app/DiagnosticsManager.h"
#include "DiagnosticsWindow.h"
#include "Catalogue3DTableModel.h"
#include "Catalogue3DParser.h"
#include "AssistantPanel.h"
#include "theme/VisivoTheme.h"

// ── VTK ──────────────────────────────────────────────────────────────────────
#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkAppendPolyData.h>
#include <vtkArrowSource.h>
#include <vtkAxesActor.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCubeSource.h>
#include <vtkCubeAxesActor.h>
#include <vtkCoordinate.h>
#include <vtkFloatArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGlyph3DMapper.h>
#include <vtkIntArray.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
#include <vtkMaskPoints.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSphereSource.h>
#include <vtkTextProperty.h>

// ── Qt ───────────────────────────────────────────────────────────────────────
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QKeySequence>
#include <QMetaObject>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>
#include "SidebarPanel.h"
#include <QtConcurrent>

#include <optional>

/// Returns true when @p errorText looks like a backend session-expiry error.
// ── std ───────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "CatalogueWcsUtil.h"

using namespace Qt::StringLiterals;

/// Used to offer the user an explicit "Re-open Dataset" recovery path instead
/// of a generic warning.
static bool isSessionExpiryError(const QString &errorText)
{
    const QString lower = errorText.toLower();
    return lower.contains(u"session"_s)
        || lower.contains(u"expired"_s)
        || lower.contains(u"not found"_s);
}

// ── Anonymous helpers ─────────────────────────────────────────────────────────
namespace {

QString catalogueFormatForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("csv")) {
        return QStringLiteral("csv");
    }
    if (suffix == QStringLiteral("vot") || suffix == QStringLiteral("votable")
        || suffix == QStringLiteral("xml")) {
        return QStringLiteral("votable");
    }
    if (suffix == QStringLiteral("fits") || suffix == QStringLiteral("fit")) {
        return QStringLiteral("fits");
    }
    if (suffix == QStringLiteral("speck")) {
        return QStringLiteral("speck");
    }
    if (suffix == QStringLiteral("tbl")) {
        return QStringLiteral("ipac");
    }
    return QStringLiteral("csv");
}

} // namespace

// ── "Add Filter" dialog ───────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

vtkWindowCatalogue3D::vtkWindowCatalogue3D(const QString &filepath, const QString &backendUrl,
                                           const QString &backendToken, const QStringList &paths,
                                           QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::vtkWindowCatalogue3D)
    , filepath(filepath)
    , backendUrl(backendUrl)
    , backendToken(backendToken)
    , m_cataloguePaths(paths)
{
    ui->setupUi(this);
    this->setupSidebar();

    // ── Connect background filter watcher ─────────────────────────────────────
    QObject::connect(&this->filterWatcher, &QFutureWatcher<Catalogue3DFilterTaskResult>::finished,
                     this, [this]() {
                         this->filterBusy = false;
                         // Re-enable filter controls
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
                                     u"Catalogue3D"_s,
                                     u"Filter failed: "_s + result.error,
                                     this->remoteDatasetId, this->remoteSessionId, u"filter"_s);
                             if (isSessionExpiryError(result.error)) {
                                 QMessageBox msg(this);
                                 msg.setWindowTitle(u"Session Expired"_s);
                                 msg.setText(
                                         u"The backend session for this dataset has expired.\n"
                                         u"The dataset needs to be reopened before continuing."_s);
                                 auto *btnReopen = msg.addButton(
                                         u"Re-open Dataset"_s, QMessageBox::AcceptRole);
                                 msg.addButton(QMessageBox::Cancel);
                                 msg.exec();
                                 if (msg.clickedButton() == btnReopen) {
                                     const QString path = this->filepath;
                                     emit reopenRequested(path, this->m_cataloguePaths);
                                     this->close();
                                 }
                             } else {
                                 QMessageBox::warning(this, u"Filter error"_s, result.error);
                             }
                             return;
                         }

                         this->entries = result.entries;
                         // Update pagination state
                         this->m_currentOffset = static_cast<int>(result.entries.size());
                         this->m_totalCount = result.totalRows;
                         const bool hasMore = this->m_currentOffset < this->m_totalCount;
                         if (this->btnLoadAll)
                             this->btnLoadAll->setVisible(hasMore);
                         if (this->btnLoadMore) {
                             this->btnLoadMore->setVisible(hasMore);
                             this->btnLoadMore->setEnabled(true);
                             this->btnLoadMore->setText(
                                     QStringLiteral("Load more (%1 remaining)")
                                             .arg(this->m_totalCount - this->m_currentOffset));
                         }

                         // Re-apply the active coordinate frame: the parser always
                         // produces FK5/J2000 scene positions; convert to Galactic if needed.
                         this->applyFrameToEntries();
                         ui->labelStatus->setText(result.statusMsg);
                         if (this->filterStatusLabel) {
                             this->filterStatusLabel->setText(result.statusMsg);
                         }

                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                 u"Catalogue3D"_s,
                                 QStringLiteral("Filter applied: %1 of %2 sources loaded.")
                                         .arg(result.entries.size())
                                         .arg(result.totalRows),
                                 this->remoteDatasetId, this->remoteSessionId, u"filter"_s);

                         // Rebuild the VTK scene with the filtered data
                         this->renderer->RemoveAllViewProps();
                         this->morphologyNames.clear();
                         this->morphologyIndexOf.clear();
                         this->labelActors.clear();
                         this->shellActors.clear();
                         this->mappedPositions.clear();
                         this->hoveredIndex = -1;
                         this->selectedIndex = -1;
                         if (!this->entries.empty()) {
                             this->buildScene();
                             this->buildLabels();
                         }
                         this->refreshCatalogueTable();
                         this->renderWindow->Render();
                     });

    // ── Connect "load more" watcher ───────────────────────────────────────────
    QObject::connect(
            &this->m_loadMoreWatcher, &QFutureWatcher<Catalogue3DFilterTaskResult>::finished,
            this, [this]() {
                this->m_loadingMore = false;

                const auto result = this->m_loadMoreWatcher.result();
                if (!result.valid) {
                    if (isSessionExpiryError(result.error)) {
                        // Auto-recover: reopen the dataset and retry without
                        // interrupting the user.
                        this->autoReopenThenLoadMore();
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

                // Append new entries
                this->entries.insert(this->entries.end(),
                                     result.entries.begin(), result.entries.end());
                this->m_currentOffset = static_cast<int>(this->entries.size());
                this->m_totalCount = result.totalRows;

                // Re-apply coordinate frame for the newly appended entries
                this->applyFrameToEntries();

                const bool hasMore = this->m_currentOffset < this->m_totalCount;
                if (this->btnLoadAll)
                    this->btnLoadAll->setVisible(hasMore);
                if (this->btnLoadMore) {
                    this->btnLoadMore->setVisible(hasMore);
                    this->btnLoadMore->setEnabled(true);
                    this->btnLoadMore->setText(
                            QStringLiteral("Load more (%1 remaining)")
                                    .arg(this->m_totalCount - this->m_currentOffset));
                }
                const QString statusMsg =
                        QStringLiteral("Loaded %1 of %2 sources.")
                                .arg(this->entries.size())
                                .arg(this->m_totalCount);
                ui->labelStatus->setText(statusMsg);
                if (this->filterStatusLabel) {
                    this->filterStatusLabel->setText(statusMsg);
                }

                // Rebuild the scene with all entries (including appended)
                this->renderer->RemoveAllViewProps();
                this->morphologyNames.clear();
                this->morphologyIndexOf.clear();
                this->labelActors.clear();
                this->shellActors.clear();
                this->mappedPositions.clear();
                this->hoveredIndex  = -1;
                this->selectedIndex = -1;
                if (!this->entries.empty()) {
                    this->buildScene();
                    this->buildLabels();
                }
                this->refreshCatalogueTable();
                this->renderWindow->Render();
            });

    ui->labelStatus->setToolTip(
            u"3D positions use RA/Dec + redshift or distance. Source geometry is parametric."_s);

    // ── Connect auto-reopen watcher ───────────────────────────────────────────
    QObject::connect(&this->m_reopenWatcher, &QFutureWatcher<BackendCatalogueInfo>::finished,
                     this, [this]() {
                         const auto result = this->m_reopenWatcher.result();
                         if (!result.valid) {
                             if (this->filterStatusLabel)
                                 this->filterStatusLabel->setText(
                                         u"Reconnect failed: "_s + result.error);
                             if (this->btnLoadMore) {
                                 this->btnLoadMore->setEnabled(true);
                                 this->btnLoadMore->setText(
                                         QStringLiteral("Load more (%1 remaining)")
                                                 .arg(this->m_totalCount - this->m_currentOffset));
                             }
                             return;
                         }
                         this->remoteDatasetId = result.datasetId;
                         this->remoteSessionId = result.sessionId;
                         if (this->m_copilot)
                             this->m_copilot->setDatasetId(this->remoteDatasetId);
                         if (this->filterStatusLabel)
                             this->filterStatusLabel->setText({});
                         this->loadMoreEntries();
                     });

    BackendClient client(backendUrl, backendToken);
    const QString catFormat = catalogueFormatForPath(filepath);
    const auto opened = this->m_cataloguePaths.size() > 1
            ? client.openCatalogue(this->m_cataloguePaths, catFormat)
            : client.openCatalogue(filepath, catFormat);
    if (!opened.valid) {
        QMessageBox::critical(this, u"Catalogue Error"_s, opened.error);
        ui->labelStatus->setText(u"Load error: "_s + opened.error);
    } else {
        this->remoteDatasetId = opened.datasetId;
        this->remoteSessionId = opened.sessionId;
        if (this->m_copilot)
            this->m_copilot->setDatasetId(this->remoteDatasetId);
        this->schema.headers.clear();
        this->schema.fields.clear();
        this->schema.fields.reserve(opened.fields.size());
        this->schema.raFieldName = opened.raField;
        this->schema.decFieldName = opened.decField;
        this->schema.xFieldName = opened.xField;
        this->schema.yFieldName = opened.yField;
        this->schema.zFieldName = opened.zField;
        for (const auto &field : opened.fields) {
            this->schema.fields.push_back({ field.name, field.type });
            this->schema.headers.append(field.name);
        }
        this->rawHeaders = this->schema.headers;
        // Detect vx/vy/vz now (before buildScene/updateColorMapping) so a
        // "Speed |v|" colour mapping never uses stale column indices.
        this->detectVelocityColumns();

        BackendTabularQueryRequest qreq;
        qreq.datasetId = this->remoteDatasetId;
        const auto subset = client.queryTabularCatalogue(qreq);
        const auto parsed = Catalogue3DParser::parseBackendSubset(this->schema, subset.rows);
        if (!subset.valid || !parsed.valid) {
            const QString error = !subset.valid ? subset.error : parsed.errorMessage;
            QMessageBox::critical(this, u"Catalogue Error"_s, error);
            ui->labelStatus->setText(u"Load error: "_s + error);
        } else {
            this->entries = parsed.entries;
            // Apply the active coordinate frame (FK5_J2000 is the parser default;
            // this is a no-op on startup since comboFrame defaults to FK5_J2000).
            this->applyFrameToEntries();
            if (parsed.skippedEntries > 0) {
                ui->labelStatus->setText(
                        QStringLiteral("Loaded %1 sources (%2 rows skipped).")
                                .arg(entries.size())
                                .arg(parsed.skippedEntries));
            } else {
                ui->labelStatus->setText(QStringLiteral("Loaded %1 sources.").arg(entries.size()));
            }
        }
    }

    setWindowTitle(QStringLiteral("3D Catalogue — %1  (%2 sources)")
                           .arg(QFileInfo(filepath).fileName())
                           .arg(entries.size()));

    // ── VTK scene ────────────────────────────────────────────────────────────
    setupRenderer();
    if (!entries.empty()) {
        buildScene();
        buildLabels();
    }
    this->ensureCatalogueDock();
    this->populateMappingControls();
    this->refreshCatalogueTable();

    // ── Filter signal/slot connections ───────────────────────────────────────
    // Widgets (filterList, btnFilter*, filterStatusLabel, btnLoadMore) were
    // created in setupSidebar().  Here we only wire interactions that require
    // filterableFields, which comes from the schema populated above.
    {
        // Build the list of filterable field names (non-string fields preferred).
        QStringList filterableFields;
        for (const auto &field : this->schema.fields) {
            if (field.type != QStringLiteral("string")) {
                filterableFields.append(field.name);
            }
        }
        if (filterableFields.isEmpty()) {
            filterableFields = this->schema.headers; // fallback: all fields
        }

        // Filter: Add
        QObject::connect(this->btnFilterAdd, &QPushButton::clicked, this,
                         [this, filterableFields]() {
                             const auto filter = showAddFilterDialog(this, filterableFields);
                             if (!filter) {
                                 return;
                             }
                             this->activeFilters.push_back(*filter);
                             this->filterList->addItem(QStringLiteral("%1 %2 %3").arg(
                                     filter->field, filter->op, filter->value));
                             DiagnosticsManager::instance()->publish(
                                     DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                     QStringLiteral("Catalogue3D"),
                                     QStringLiteral("Filter added: %1 %2 %3")
                                             .arg(filter->field, filter->op, filter->value),
                                     this->remoteDatasetId, this->remoteSessionId,
                                     QStringLiteral("filter"));
                         });
        // Filter: Edit on double-click
        QObject::connect(
                this->filterList, &QListWidget::itemDoubleClicked, this,
                [this, filterableFields](QListWidgetItem *item) {
                    if (!item || !this->filterList) {
                        return;
                    }
                    const int row = this->filterList->row(item);
                    if (row < 0 || row >= static_cast<int>(this->activeFilters.size())) {
                        return;
                    }
                    const auto updated =
                            showAddFilterDialog(this, filterableFields, &this->activeFilters[row]);
                    if (!updated) {
                        return;
                    }
                    this->activeFilters[row] = *updated;
                    item->setText(QStringLiteral("%1 %2 %3")
                                          .arg(updated->field, updated->op, updated->value));
                    DiagnosticsManager::instance()->publish(
                            DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                            QStringLiteral("Catalogue3D"),
                            QStringLiteral("Filter [%1] edited: %2 %3 %4")
                                    .arg(row)
                                    .arg(updated->field, updated->op, updated->value),
                            this->remoteDatasetId, this->remoteSessionId,
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
                    DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                    QStringLiteral("Catalogue3D"),
                    QStringLiteral("Filter removed: %1 %2 %3").arg(removed.field, removed.op,
                                                                    removed.value),
                    this->remoteDatasetId, this->remoteSessionId, QStringLiteral("filter"));
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
                        DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                        QStringLiteral("Catalogue3D"),
                        QStringLiteral("%1 filter(s) cleared.").arg(count),
                        this->remoteDatasetId, this->remoteSessionId, QStringLiteral("filter"));
            }
        });
        // Filter: Apply
        QObject::connect(this->btnFilterApply, &QPushButton::clicked, this,
                         &vtkWindowCatalogue3D::applyFilter);

        // ── View menu: catalogue dock toggle ─────────────────────────────────
        if (this->catalogueDock) {
            this->catalogueDock->toggleViewAction()->setText(u"Catalogue Data"_s);
            ui->menuView->addAction(this->catalogueDock->toggleViewAction());
        }

        // ── Diagnostics + Command Palette (replicated across windows) ────────
        ui->menuView->addSeparator();
        auto *actionDiagnostics = new QAction(u"Diagnostics"_s, this);
        actionDiagnostics->setToolTip(u"Open the application-wide diagnostics log window."_s);
        ui->menuView->addAction(actionDiagnostics);
        QObject::connect(actionDiagnostics, &QAction::triggered, this,
                         []() { DiagnosticsWindow::showSingleton(); });

        auto *actionCommandPalette = new QAction(u"Command Palette…"_s, this);
        actionCommandPalette->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
        actionCommandPalette->setShortcutContext(Qt::ApplicationShortcut);
        ui->menuView->addAction(actionCommandPalette);
        QObject::connect(actionCommandPalette, &QAction::triggered, this, [this]() {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (w->inherits("MainWindow")) {
                    QMetaObject::invokeMethod(w, "openCommandPalette",
                                              Qt::QueuedConnection);
                    return;
                }
            }
        });
    }

    // ── Connect UI controls ───────────────────────────────────────────────────
    QObject::connect(ui->chkLabels, &QCheckBox::toggled, this,
                     &vtkWindowCatalogue3D::toggleLabels);
    QObject::connect(ui->chkMorphColors, &QCheckBox::toggled, this,
                     &vtkWindowCatalogue3D::toggleMorphColors);
    QObject::connect(ui->sliderScale, &QSlider::valueChanged, this,
                     &vtkWindowCatalogue3D::scaleChanged);

    QObject::connect(ui->actionShowLabels, &QAction::toggled, ui->chkLabels,
                     &QCheckBox::setChecked);
    QObject::connect(ui->chkLabels, &QCheckBox::toggled, ui->actionShowLabels,
                     &QAction::setChecked);
    QObject::connect(ui->actionColorByMorphology, &QAction::toggled, ui->chkMorphColors,
                     &QCheckBox::setChecked);
    QObject::connect(ui->chkMorphColors, &QCheckBox::toggled, ui->actionColorByMorphology,
                     &QAction::setChecked);
    QObject::connect(ui->actionResetCamera, &QAction::triggered, this,
                     &vtkWindowCatalogue3D::resetCamera);
    QObject::connect(this->comboGeometry, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateGeometryMode(); });
    QObject::connect(this->comboSizeMode, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateSizeMode(); });
    QObject::connect(this->comboFrame, &QComboBox::currentIndexChanged, this, [this](int idx) {
        this->activeFrame = (idx == 1) ? CoordFrame::Galactic
                          : (idx == 2) ? CoordFrame::Supergalactic
                                       : CoordFrame::FK5_J2000;
        this->applyFrameToEntries();
        this->renderer->RemoveAllViewProps();
        this->morphologyNames.clear();
        this->morphologyIndexOf.clear();
        this->labelActors.clear();
        this->shellActors.clear();
        this->mappedPositions.clear();
        this->hoveredIndex  = -1;
        this->selectedIndex = -1;
        if (!this->entries.empty()) {
            this->buildScene();
            this->buildLabels();
        }
        this->renderWindow->Render();
    });
    QObject::connect(this->comboCosmology, &QComboBox::currentIndexChanged, this,
                     [this](int) {
                         this->m_cosmologyModel = this->comboCosmology->currentText();
                         this->triggerCosmologyUpdate();
                     });
    QObject::connect(this->comboDistanceMode, &QComboBox::currentIndexChanged, this,
                     [this](int idx) {
                         // Cartesian catalogues have explicit XYZ positions — no
                         // redshift to reinterpret, so the mode is a no-op there.
                         if (this->schema.isCartesian() || this->entries.empty())
                             return;
                         this->schema.distanceMode = (idx == 1)
                                 ? CatalogueDistanceMode::RedshiftSpace
                                 : CatalogueDistanceMode::Comoving;
                         this->recomputeScenePositionsAndRebuild();
                         const QString label = (idx == 1)
                                 ? QStringLiteral("Redshift-space (cz/H₀) – %1 sources.")
                                 : QStringLiteral("Real-space (comoving) – %1 sources.");
                         if (this->filterStatusLabel)
                             this->filterStatusLabel->setText(label.arg(entries.size()));
                         ui->labelStatus->setText(label.arg(entries.size()));
                     });
    QObject::connect(&this->m_cosmologyWatcher, &QFutureWatcher<Catalogue3DCosmologyTaskResult>::finished,
                     this, &vtkWindowCatalogue3D::onCosmologyFinished);
    QObject::connect(this->chkShowAxes, &QCheckBox::toggled, this,
                     &vtkWindowCatalogue3D::toggleSceneAxes);
    QObject::connect(this->chkShowBoundingBox, &QCheckBox::toggled, this,
                     &vtkWindowCatalogue3D::toggleBoundingBox);
    QObject::connect(this->chkShowShells, &QCheckBox::toggled, this,
                     &vtkWindowCatalogue3D::toggleShells);
}

vtkWindowCatalogue3D::~vtkWindowCatalogue3D()
{
    delete ui;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sidebar
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// Coordinate frame conversion
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// Cosmology
// ─────────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────────
// Filter
// ─────────────────────────────────────────────────────────────────────────────




// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// Scene construction
// ─────────────────────────────────────────────────────────────────────────────

/*static*/















// ─────────────────────────────────────────────────────────────────────────────
// Interaction
// ─────────────────────────────────────────────────────────────────────────────






// ─────────────────────────────────────────────────────────────────────────────
// Info panel
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// Slots
// ─────────────────────────────────────────────────────────────────────────────
















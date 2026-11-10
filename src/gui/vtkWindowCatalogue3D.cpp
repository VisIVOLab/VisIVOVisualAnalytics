#include "CatalogueFormat.h"
#include "WindowCloseShortcut.h"
#include "BusyStatus.h"
#include "VbtByteOrderPrompt.h"
#include <QDateTime>
#include "CosmologyCalculatorDialog.h"
#include "vtkWindowCatalogue3D.h"
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include "ui_vtkWindowCatalogue3D.h"

#include "app/DiagnosticsManager.h"
#include "DiagnosticsWindow.h"
#include "Catalogue3DTableModel.h"
#include "Catalogue3DParser.h"
#include "AssistantPanel.h"
#include "CopilotActionRouter.h"
#include "theme/VisivoTheme.h"

// ── VTK ──────────────────────────────────────────────────────────────────────
#include <QVTKOpenGLNativeWidget.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPointGaussianMapper.h>
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
    return visivo::catalogueFormatForPath(path);
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
                                           const Catalogue3DLabel &label, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::vtkWindowCatalogue3D)
    , filepath(filepath)
    , backendUrl(backendUrl)
    , backendToken(backendToken)
    , m_cataloguePaths(paths)
{
    // Cmd+W closes it, as the title bar's button does (WindowCloseShortcut.h).
    visivo::installCloseWindowShortcut(this);
    // Before anything shows a name: the title and the chrome are built from it.
    this->m_label = label;
    this->m_label.displayName = this->m_label.displayName.trimmed();
    ui->setupUi(this);
    this->buildControlPages();

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
                         // By rows consumed: rows skipped for bad coordinates
                         // must not be fetched again by the next page.
                         this->m_currentOffset = result.returnedRows;
                         this->m_sampleStep = result.step;
                         this->m_samplePhase = 1;
                         this->m_sampleLimit = result.limit;
                         this->m_totalCount = result.totalRows;
                         // Auto follows the rows: on the backend while it has
                         // more than are loaded here.
                         QTimer::singleShot(0, this,
                                            [this]() { this->applyRenderMode(this->m_renderMode); });
                         const bool hasMore = this->hasMoreRows();
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
                         this->reapplyCosmologyAfterReload();
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
                         // The command bar counts what is on screen, so it has to
                         // follow a filter: it used to keep claiming the number
                         // the window opened with.
                         this->refreshViewerChrome();
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
                this->m_currentOffset += result.returnedRows;   // rows consumed, skipped ones too
                if (this->m_sampleStep > 1)
                    ++this->m_samplePhase;   // the rows between those loaded
                this->m_totalCount = result.totalRows;
                QTimer::singleShot(0, this,
                                   [this]() { this->applyRenderMode(this->m_renderMode); });

                // Re-apply coordinate frame for the newly appended entries
                this->applyFrameToEntries();
                this->reapplyCosmologyAfterReload();

                const bool hasMore = this->hasMoreRows();
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
                this->refreshViewerChrome();   // another page of sources changes the count
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
                         if (this->m_copilot) {
                             this->m_copilot->setDatasetId(this->remoteDatasetId);
                             CopilotActionRouter::instance()->registerTarget(
                                     this->remoteDatasetId, this,
                                     [this](const QList<QJsonObject> &acts) {
                                         this->applyCopilotActions(acts);
                                     },
                                     [this]() { this->undoCopilotActions(); });
                         }
                         // The window is on a NEW session now: the status rail
                         // and the Inspector both named the expired one until
                         // this was added.
                         this->refreshViewerChrome();
                         if (this->filterStatusLabel)
                             this->filterStatusLabel->setText({});
                         this->loadMoreEntries();
                     });

    BackendClient client(backendUrl, backendToken);
    const QString catFormat = catalogueFormatForPath(filepath);
    // A window the user opens: a VBT is read as the backend was last told,
    // and warned again when its header looks wrong (issue #165).
    client.setUseRememberedVbtByteOrder(false);
    auto opened = this->m_cataloguePaths.size() > 1
            ? client.openCatalogue(this->m_cataloguePaths, catFormat)
            : client.openCatalogue(filepath, catFormat);
    // A VBT whose values look like the other byte order than its header says
    // (issue #165): ask, and open it again in the order chosen. A button's
    // answer is remembered, and the automatic reopen after a session expiry
    // (autoReopenThenLoadMore, a client left at its default) sends it; after
    // Escape nothing is sent, and the backend reads the file as it does now.
    if (opened.valid
        && visivo::askVbtByteOrder(this, backendUrl, filepath, opened.byteOrderWarning,
                                   opened.byteOrder)) {
        // The first answer's dataset is closed by the backend as the order
        // changes: no orphan is left behind.
        client.setUseRememberedVbtByteOrder(true);
        opened = this->m_cataloguePaths.size() > 1
                ? client.openCatalogue(this->m_cataloguePaths, catFormat)
                : client.openCatalogue(filepath, catFormat);
    }
    if (!opened.valid) {
        // Before the box: its reading time is not the open's ("Last 5 jobs").
        this->m_openFailedAtMs = QDateTime::currentMSecsSinceEpoch();
        // The opener's busy indicator is not up behind the box (#159).
        const visivo::BusyPause box;
        QMessageBox::critical(this, u"Catalogue Error"_s, opened.error);
        ui->labelStatus->setText(u"Load error: "_s + opened.error);
    } else {
        this->remoteDatasetId = opened.datasetId;
        this->remoteSessionId = opened.sessionId;
        // A VBT point table can be drawn on the backend (issue #83).
        this->m_cartesian = opened.coordSystem == u"cartesian"_s;
        this->m_skyCoordinates = opened.coordSystem == u"sky"_s && !opened.raField.isEmpty()
                                 && !opened.decField.isEmpty();
        this->m_pointTable = catalogueFormatForPath(filepath) == u"vbt"_s;
        // Any catalogue the backend can draw (sky or cartesian), issue #83.
        this->m_serverSideRendering = opened.serverSideRenderingAvailable;
        this->m_tableRows = opened.numRows;
        // On the FIRST open the copilot does not exist yet — it is the
        // Inspector's tab, and the Inspector is installed once this load has
        // finished (setupWorkspaceDocks does the same wiring there). This branch
        // still matters for a re-open into a live window.
        if (this->m_copilot) {
            this->m_copilot->setDatasetId(this->remoteDatasetId);
            CopilotActionRouter::instance()->registerTarget(
                    this->remoteDatasetId, this,
                    [this](const QList<QJsonObject> &acts) { this->applyCopilotActions(acts); },
                    [this]() { this->undoCopilotActions(); });
        }
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
        qreq.evenSample = this->m_pointTable;
        const auto subset = client.queryTabularCatalogue(qreq);
        this->m_sampleStep = subset.step;
        this->m_samplePhase = 1;
        this->m_sampleLimit = qreq.limit;
        const auto parsed = Catalogue3DParser::parseBackendSubset(this->schema, subset.rows, subset.rowIndex);
        if (!subset.valid || !parsed.valid) {
            const QString error = !subset.valid ? subset.error : parsed.errorMessage;
            this->m_openFailedAtMs = QDateTime::currentMSecsSinceEpoch();
            const visivo::BusyPause box;
            QMessageBox::critical(this, u"Catalogue Error"_s, error);
            ui->labelStatus->setText(u"Load error: "_s + error);
        } else {
            this->entries = parsed.entries;
            // The first page is a page like any other: what the backend holds
            // beyond it is what Load more brings, and what a row message for
            // a row not loaded yet is told about. Without the total the window
            // believed it held the whole table.
            this->m_currentOffset = subset.returnedRows > 0
                    ? subset.returnedRows
                    : static_cast<int>(subset.rows.size());
            this->m_totalCount = subset.totalRows;
            // Apply the active coordinate frame (FK5_J2000 is the parser default;
            // this is a no-op on startup since comboFrame defaults to FK5_J2000).
            this->applyFrameToEntries();
            const QString loaded = this->m_totalCount > static_cast<int>(entries.size())
                    ? QStringLiteral("Loaded %1 of %2 sources").arg(entries.size()).arg(this->m_totalCount)
                    : QStringLiteral("Loaded %1 sources").arg(entries.size());
            if (parsed.skippedEntries > 0) {
                ui->labelStatus->setText(QStringLiteral("%1 (%2 rows skipped).")
                                                 .arg(loaded)
                                                 .arg(parsed.skippedEntries));
            } else {
                ui->labelStatus->setText(loaded + u'.');
            }
        }
    }

    this->updateWindowTitle();
    this->updateLoadMoreButtons();

    // The same command bar and status rail as the image and cube viewers. Built
    // here rather than in setupUi() because the kind tag names the number of
    // sources, which is only known once the catalogue has been read.
    this->setupViewerChrome();

    // ── VTK scene ────────────────────────────────────────────────────────────
    setupRenderer();
    this->setupRemoteRendering();
    if (!entries.empty()) {
        buildScene();
        buildLabels();
    }
    // Auto: drawn on the backend when it has more rows than this window loaded.
    QTimer::singleShot(0, this, [this]() { this->applyRenderMode(this->m_renderMode); });
    this->ensureCatalogueDock();
    this->populateMappingControls();
    this->refreshCatalogueTable();
    this->applyPointTableLayout();
    this->choosePointTableDefaults();

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

        // ── Tools menu: the cosmology calculator (issue #29) ─────────────────
        // Opened on the model the sources are placed with; it is also where
        // the Custom entry of the Model selector is defined.
        {
            auto *toolsMenu = new QMenu(u"Tools"_s, this);
            toolsMenu->setToolTipsVisible(true);
            this->menuBar()->insertMenu(ui->menuView->menuAction(), toolsMenu);
            auto *actionCalculator = toolsMenu->addAction(u"Cosmology Calculator…"_s);
            actionCalculator->setToolTip(
                    u"Distances, lookback time, age and kpc per arcsecond for any "
                    u"redshift under this window's cosmology, or a custom one."_s);
            QObject::connect(actionCalculator, &QAction::triggered, this,
                             &vtkWindowCatalogue3D::openCosmologyCalculator);
        }

        // ── SAMP menu: this catalogue, a selection of it, a position ─────────
        this->setupSampMenu();

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
    // Deselecting: this, Esc, or a click on empty space (onMouseEvent). Also
    // on the window itself, not only in the menu: the macOS menu bar does not
    // take Esc as a key equivalent.
    {
        auto *clear = new QAction(u"Clear Selection"_s, this);
        clear->setShortcut(QKeySequence(Qt::Key_Escape));
        clear->setShortcutContext(Qt::WindowShortcut);
        clear->setToolTip(u"Deselect the selected source (Esc, or click on empty space)"_s);
        clear->setEnabled(false);
        ui->menuView->insertAction(ui->menuView->actions().value(1), clear);
        this->addAction(clear);
        QObject::connect(clear, &QAction::triggered, this,
                         [this]() { this->setSelectedSource(-1); });
        QObject::connect(this, &vtkWindowCatalogue3D::sourceSelectionChanged, clear,
                         [clear](int idx) { clear->setEnabled(idx >= 0); });
    }
    QObject::connect(this->comboGeometry, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->updateGeometryMode(); });
    QObject::connect(this->m_rtLightCheck, &QCheckBox::toggled, this,
                     [this](bool) { this->updateGeometryMode(); });
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
                         const QString chosen = this->comboCosmology->currentText();
                         if (chosen == visivo::cosmology::customName()
                             && this->serverDrawsPositions()) {
                             // The backend places these sources itself and has
                             // no custom model: refuse rather than draw one
                             // universe here and another there.
                             const QSignalBlocker block(this->comboCosmology);
                             this->comboCosmology->setCurrentText(this->m_appliedCosmologyModel);
                             this->statusBar()->showMessage(
                                     u"The custom cosmology is not available while the backend "
                                     u"draws the points (ray-traced or server-side view): it "
                                     u"knows Planck18, Planck15, Planck13 and WMAP9 only."_s,
                                     8000);
                             return;
                         }
                         this->m_cosmologyModel = chosen;
                         this->triggerCosmologyUpdate();
                     });
    // A custom model handed over from the calculator: a window placed with the
    // custom model re-places its sources under the new one.
    QObject::connect(&CosmologyChoiceHub::instance(), &CosmologyChoiceHub::customModelChanged,
                     this, [this]() {
                         if (this->m_cosmologyModel == visivo::cosmology::customName())
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
    if (this->m_markerObserverTag)
        this->renderer->RemoveObserver(this->m_markerObserverTag);
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
















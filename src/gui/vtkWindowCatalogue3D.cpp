#include "vtkWindowCatalogue3D.h"
#include "ui_vtkWindowCatalogue3D.h"

#include "app/DiagnosticsManager.h"
#include "DiagnosticsWindow.h"
#include "Catalogue3DTableModel.h"
#include "Catalogue3DParser.h"
#include "theme/VisivoTheme.h"

// ── VTK ──────────────────────────────────────────────────────────────────────
#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkAppendPolyData.h>
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
#include <vtkGlyph3D.h>
#include <vtkIntArray.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
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

// ── libwcs coordinate conversion ──────────────────────────────────────────────
// wcscon() converts between WCS coordinate systems (FK5/J2000, Galactic, …).
// Only the declaration is needed here; the function is linked from libwcs.
extern "C" {
void wcscon(int sys1, int sys2, double eq1, double eq2,
            double *dtheta, double *dphi, double epoch);
}
static constexpr int kWCS_J2000    = 1;
static constexpr int kWCS_GALACTIC = 3;

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

// Fixed palette for recognised morphology classes (RGB, 0-1).
// Unknown classes are assigned from the same cycle by index modulo.
const std::array<std::array<double, 3>, 10> kPalette = { {
        { 0.58, 0.18, 0.92 }, // 0 – halo        – purple
        { 1.00, 0.50, 0.00 }, // 1 – relic        – orange
        { 0.00, 0.82, 0.90 }, // 2 – mini-halo    – cyan
        { 0.70, 0.90, 0.10 }, // 3 – double relic – lime
        { 0.90, 0.25, 0.25 }, // 4 – phoenix      – red
        { 0.20, 0.75, 0.35 }, // 5 – extended     – green
        { 0.95, 0.80, 0.00 }, // 6 – diffuse      – gold
        { 0.30, 0.55, 0.95 }, // 7 – candidate    – blue
        { 0.80, 0.45, 0.80 }, // 8 – unclear      – pink
        { 0.70, 0.70, 0.70 }, // 9 – other/fallback – gray
} };

// Canonical mapping: morphology keyword → palette index.
const QMap<QString, int> kKnownMorphologies = {
        { QStringLiteral("halo"),         0 },
        { QStringLiteral("relic"),        1 },
        { QStringLiteral("mini-halo"),    2 },
        { QStringLiteral("minihalo"),     2 },
        { QStringLiteral("mini halo"),    2 },
        { QStringLiteral("double relic"), 3 },
        { QStringLiteral("doublerelic"),  3 },
        { QStringLiteral("phoenix"),      4 },
        { QStringLiteral("extended"),     5 },
        { QStringLiteral("diffuse"),      6 },
        { QStringLiteral("candidate"),    7 },
        { QStringLiteral("unclear"),      8 },
        { QStringLiteral("other"),        9 },
};

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
                                           const QString &backendToken, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::vtkWindowCatalogue3D)
    , filepath(filepath)
    , backendUrl(backendUrl)
    , backendToken(backendToken)
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
                                         "The dataset needs to be reopened before continuing."_s);
                                 auto *btnReopen = msg.addButton(
                                         u"Re-open Dataset"_s, QMessageBox::AcceptRole);
                                 msg.addButton(QMessageBox::Cancel);
                                 msg.exec();
                                 if (msg.clickedButton() == btnReopen) {
                                     const QString path = this->filepath;
                                     emit reopenRequested(path);
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
                         if (this->filterStatusLabel)
                             this->filterStatusLabel->setText({});
                         this->loadMoreEntries();
                     });

    BackendClient client(backendUrl, backendToken);
    const auto opened = client.openCatalogue(filepath, catalogueFormatForPath(filepath));
    if (!opened.valid) {
        QMessageBox::critical(this, u"Catalogue Error"_s, opened.error);
        ui->labelStatus->setText(u"Load error: "_s + opened.error);
    } else {
        this->remoteDatasetId = opened.datasetId;
        this->remoteSessionId = opened.sessionId;
        this->schema.headers.clear();
        this->schema.fields.clear();
        this->schema.fields.reserve(opened.fields.size());
        this->schema.raFieldName = opened.raField;
        this->schema.decFieldName = opened.decField;
        for (const auto &field : opened.fields) {
            this->schema.fields.push_back({ field.name, field.type });
            this->schema.headers.append(field.name);
        }
        this->rawHeaders = this->schema.headers;

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
        this->activeFrame = (idx == 1) ? CoordFrame::Galactic : CoordFrame::FK5_J2000;
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

void vtkWindowCatalogue3D::setupSidebar()
{
    // Remove the UI-file sidebar container and replace it with SidebarPanel.
    auto *mainLayout = qobject_cast<QHBoxLayout *>(ui->centralwidget->layout());
    mainLayout->removeWidget(ui->sidebarContainer);
    ui->sidebarContainer->hide();

    this->m_sidebar = new SidebarPanel(296, this);

    // Shared card styles (same palette as cube viewer).
    const QString hdrStyle    = VisivoTheme::sidebarHeaderStyleSheet();
    const QString cardStyle   = VisivoTheme::sidebarCardStyleSheet();
    const QString subHdrStyle = VisivoTheme::sidebarSubHeaderStyleSheet();

    // ── Visualization page ────────────────────────────────────────────────────
    // Built programmatically so we own the layout cleanly.  ui->pageViz (from
    // the .ui file) is left in the hidden sidebarContainer and not used.
    {
        auto *pageViz    = new QWidget;
        auto *pageLayout = new QVBoxLayout(pageViz);
        pageLayout->setContentsMargins(8, 8, 8, 8);
        pageLayout->setSpacing(8);

        auto *vizHdr = new QLabel(u"VISUALIZATION"_s, pageViz);
        vizHdr->setStyleSheet(hdrStyle);
        pageLayout->addWidget(vizHdr);

        // ── Source Representation card ────────────────────────────────────────
        auto *reprCard   = new QFrame(pageViz);
        reprCard->setStyleSheet(cardStyle);
        auto *reprLayout = new QVBoxLayout(reprCard);
        reprLayout->setContentsMargins(8, 8, 8, 8);
        reprLayout->setSpacing(6);
        auto *reprHdr = new QLabel(u"Source Representation"_s, reprCard);
        reprHdr->setStyleSheet(subHdrStyle);
        reprLayout->addWidget(reprHdr);

        this->comboGeometry = new QComboBox(reprCard);
        this->comboGeometry->addItems({ u"Ellipsoid"_s, u"Sphere"_s, u"Point"_s, u"Cross"_s });
        auto *reprForm = new QFormLayout;
        reprForm->setContentsMargins(0, 0, 0, 0);
        reprForm->addRow(u"Shape"_s, this->comboGeometry);
        reprLayout->addLayout(reprForm);
        reprLayout->addWidget(ui->chkLabels);
        reprLayout->addWidget(ui->chkMorphColors);
        pageLayout->addWidget(reprCard);

        // ── Axis Mapping card ─────────────────────────────────────────────────
        auto *axisCard   = new QFrame(pageViz);
        axisCard->setStyleSheet(cardStyle);
        auto *axisLayout = new QVBoxLayout(axisCard);
        axisLayout->setContentsMargins(8, 8, 8, 8);
        axisLayout->setSpacing(6);
        auto *axisHdr = new QLabel(u"Axis Mapping"_s, axisCard);
        axisHdr->setStyleSheet(subHdrStyle);
        axisLayout->addWidget(axisHdr);

        this->comboMapX = new QComboBox(axisCard);
        this->comboMapY = new QComboBox(axisCard);
        this->comboMapZ = new QComboBox(axisCard);
        auto *axisForm = new QFormLayout;
        axisForm->setContentsMargins(0, 0, 0, 0);
        axisForm->addRow(u"X"_s, this->comboMapX);
        axisForm->addRow(u"Y"_s, this->comboMapY);
        axisForm->addRow(u"Z"_s, this->comboMapZ);
        axisLayout->addLayout(axisForm);
        pageLayout->addWidget(axisCard);

        // ── Visual Encoding card ──────────────────────────────────────────────
        auto *encCard   = new QFrame(pageViz);
        encCard->setStyleSheet(cardStyle);
        auto *encLayout = new QVBoxLayout(encCard);
        encLayout->setContentsMargins(8, 8, 8, 8);
        encLayout->setSpacing(6);
        auto *encHdr = new QLabel(u"Visual Encoding"_s, encCard);
        encHdr->setStyleSheet(subHdrStyle);
        encLayout->addWidget(encHdr);

        // Size combo doubles as comboSizeMode (same widget, two member aliases).
        this->comboSizeMode = new QComboBox(encCard);
        this->comboMapSize  = this->comboSizeMode;
        this->comboMapColor = new QComboBox(encCard);
        auto *encForm = new QFormLayout;
        encForm->setContentsMargins(0, 0, 0, 0);
        encForm->addRow(u"Size"_s,  this->comboSizeMode);
        encForm->addRow(u"Color"_s, this->comboMapColor);
        encForm->addRow(u"Scale"_s, ui->sliderScale);
        encLayout->addLayout(encForm);
        pageLayout->addWidget(encCard);

        // ── Scene card ────────────────────────────────────────────────────────
        auto *sceneCard   = new QFrame(pageViz);
        sceneCard->setStyleSheet(cardStyle);
        auto *sceneLayout = new QVBoxLayout(sceneCard);
        sceneLayout->setContentsMargins(8, 8, 8, 8);
        sceneLayout->setSpacing(6);
        auto *sceneHdr = new QLabel(u"Scene"_s, sceneCard);
        sceneHdr->setStyleSheet(subHdrStyle);
        sceneLayout->addWidget(sceneHdr);

        this->chkShowAxes        = new QCheckBox(u"Show axes"_s,          sceneCard);
        this->chkShowBoundingBox = new QCheckBox(u"Show bounding box"_s,  sceneCard);
        this->chkShowShells      = new QCheckBox(u"Show redshift shells"_s, sceneCard);
        this->chkShowAxes->setChecked(true);
        sceneLayout->addWidget(this->chkShowAxes);
        sceneLayout->addWidget(this->chkShowBoundingBox);
        sceneLayout->addWidget(this->chkShowShells);

        this->comboFrame = new QComboBox(sceneCard);
        this->comboFrame->addItems({ u"FK5 / J2000"_s, u"Galactic (l, b)"_s });
        this->comboFrame->setToolTip(
                u"Coordinate frame for 3D scene positions.\n"
                "FK5 / J2000: standard equatorial (RA, Dec).\n"
                "Galactic (l, b): galactic longitude and latitude."_s);
        this->comboCosmology = new QComboBox(sceneCard);
        this->comboCosmology->addItems(
                { u"Planck18"_s, u"Planck15"_s, u"Planck13"_s, u"WMAP9"_s });
        this->comboCosmology->setToolTip(
                u"Cosmology model for redshift → comoving distance.\n"
                "Distances are computed on the backend via astropy."_s);

        auto *sceneForm = new QFormLayout;
        sceneForm->setContentsMargins(0, 0, 0, 0);
        sceneForm->addRow(u"Frame"_s,     this->comboFrame);
        sceneForm->addRow(u"Cosmology"_s, this->comboCosmology);
        sceneLayout->addLayout(sceneForm);
        pageLayout->addWidget(sceneCard);

        pageLayout->addStretch(1);
        this->m_sidebar->addPage(u":/icons/rail_viz.svg"_s, u"Visualization"_s, pageViz);
    }

    // ── Source Info page (from UI file) ───────────────────────────────────────
    ui->groupInfo->setStyleSheet(cardStyle);
    ui->labelInfoHdr->setStyleSheet(hdrStyle);
    this->m_sidebar->addPage(u":/icons/rail_info.svg"_s, u"Source Info"_s, ui->pageInfo);

    // ── Filters page ─────────────────────────────────────────────────────────
    // Widgets assigned to members here; signal/slot connections wired later
    // in the constructor body after the schema is known.
    {
        auto *pageFilters = new QWidget;
        auto *pageLayout  = new QVBoxLayout(pageFilters);
        pageLayout->setContentsMargins(8, 8, 8, 8);
        pageLayout->setSpacing(8);

        auto *hdr = new QLabel(u"FILTERS"_s, pageFilters);
        hdr->setStyleSheet(hdrStyle);
        pageLayout->addWidget(hdr);

        // Active Filters card.
        auto *filtersCard   = new QFrame(pageFilters);
        filtersCard->setStyleSheet(cardStyle);
        auto *filtersCardLayout = new QVBoxLayout(filtersCard);
        filtersCardLayout->setContentsMargins(8, 8, 8, 8);
        filtersCardLayout->setSpacing(6);

        auto *listLabel = new QLabel(u"Active filters"_s, filtersCard);
        listLabel->setStyleSheet(subHdrStyle);
        filtersCardLayout->addWidget(listLabel);

        this->filterList = new QListWidget(filtersCard);
        this->filterList->setSelectionMode(QAbstractItemView::SingleSelection);
        this->filterList->setToolTip(
                u"Active filters combined with AND. Double-click a row to edit it."_s);
        filtersCardLayout->addWidget(this->filterList);

        this->filterStatusLabel = new QLabel(filtersCard);
        this->filterStatusLabel->setWordWrap(true);
        this->filterStatusLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; font-size: 9px; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        filtersCardLayout->addWidget(this->filterStatusLabel);

        this->btnLoadMore = new QPushButton(u"Load more…"_s, filtersCard);
        this->btnLoadMore->setVisible(false);
        this->btnLoadMore->setToolTip(
                u"Fetch the next page of results and append them to the current scene."_s);
        filtersCardLayout->addWidget(this->btnLoadMore);
        QObject::connect(this->btnLoadMore, &QPushButton::clicked,
                         this, &vtkWindowCatalogue3D::loadMoreEntries);

        pageLayout->addWidget(filtersCard);

        // Actions card.
        auto *actionsCard   = new QFrame(pageFilters);
        actionsCard->setStyleSheet(cardStyle);
        auto *actionsCardLayout = new QVBoxLayout(actionsCard);
        actionsCardLayout->setContentsMargins(8, 8, 8, 8);
        actionsCardLayout->setSpacing(4);

        this->btnFilterAdd      = new QPushButton(u"Add…"_s,      actionsCard);
        this->btnFilterRemove   = new QPushButton(u"Remove"_s,    actionsCard);
        this->btnFilterClearAll = new QPushButton(u"Clear All"_s, actionsCard);
        this->btnFilterApply    = new QPushButton(u"Apply"_s,     actionsCard);
        actionsCardLayout->addWidget(this->btnFilterAdd);
        actionsCardLayout->addWidget(this->btnFilterRemove);
        actionsCardLayout->addWidget(this->btnFilterClearAll);
        actionsCardLayout->addWidget(this->btnFilterApply);

        pageLayout->addWidget(actionsCard);
        pageLayout->addStretch(1);

        this->m_sidebar->addPage(u":/icons/rail_filter.svg"_s, u"Filters"_s, pageFilters);
    }

    this->m_sidebar->finalize(0);
    mainLayout->insertWidget(0, this->m_sidebar);

    // View menu toggle — inserted before existing actions to keep it first.
    this->actionShowControlsSidebar = this->m_sidebar->createToggleAction(
            u"Controls Sidebar"_s, this);
    ui->menuView->insertAction(ui->menuView->actions().isEmpty()
                                       ? nullptr : ui->menuView->actions().first(),
                               this->actionShowControlsSidebar);
    ui->menuView->insertSeparator(ui->menuView->actions().value(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// Coordinate frame conversion
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCatalogue3D::applyFrameToEntries()
{
    if (this->activeFrame == CoordFrame::FK5_J2000) {
        // Restore FK5/J2000 Cartesian positions from the stored RA/Dec.
        // (Galactic mode overwrites sceneX/Y/Z in-place, so we must recompute
        // rather than just returning, otherwise switching back from Galactic
        // leaves the Galactic coordinates in the entries.)
        for (auto &e : this->entries)
            Catalogue3DParser::detail::fillCartesian(e, this->schema);
        return;
    }

    // Galactic (l, b): convert each source's equatorial RA/Dec → galactic
    // longitude l / latitude b using wcscon(WCS_J2000 → WCS_GALACTIC), then
    // project onto the same spherical-to-Cartesian formula so the scene scale
    // (Mpc from redshift/distance) is preserved.
    constexpr double pi = 3.14159265358979323846;
    for (auto &e : this->entries) {
        double l = e.raDeg;
        double b = e.decDeg;
        wcscon(kWCS_J2000, kWCS_GALACTIC, 2000.0, 2000.0, &l, &b, 2000.0);
        const double d = Catalogue3DParser::detail::entryDistanceMpc(e, this->schema);
        const double lRad = l * pi / 180.0;
        const double bRad = b * pi / 180.0;
        e.sceneX = d * std::cos(bRad) * std::cos(lRad);
        e.sceneY = d * std::cos(bRad) * std::sin(lRad);
        e.sceneZ = d * std::sin(bRad);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Cosmology
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCatalogue3D::triggerCosmologyUpdate()
{
    if (this->entries.empty() || this->m_cosmologyBusy)
        return;

    // Planck18 matches the hardcoded local integration in Catalogue3DParser;
    // just clear overrides so entryDistanceMpc() falls back to local values.
    if (this->m_cosmologyModel == QStringLiteral("Planck18")) {
        for (auto &e : this->entries)
            e.distanceMpc = 0.0;
        // Recompute scene positions and rebuild.
        constexpr double pi = 3.14159265358979323846;
        for (auto &e : this->entries) {
            const double d = Catalogue3DParser::detail::entryDistanceMpc(e, this->schema);
            const double ra  = e.raDeg  * pi / 180.0;
            const double dec = e.decDeg * pi / 180.0;
            e.sceneX = d * std::cos(dec) * std::cos(ra);
            e.sceneY = d * std::cos(dec) * std::sin(ra);
            e.sceneZ = d * std::sin(dec);
        }
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
        if (this->filterStatusLabel)
            this->filterStatusLabel->setText(
                    QStringLiteral("Cosmology: Planck18 (local) – %1 sources.").arg(entries.size()));
        ui->labelStatus->setText(
                QStringLiteral("Cosmology: Planck18 (local) – %1 sources.").arg(entries.size()));
        return;
    }

    // Non-Planck18: collect redshifts for all entries and ask the backend.
    this->m_cosmologyBusy = true;
    if (this->comboCosmology)
        this->comboCosmology->setEnabled(false);
    if (this->filterStatusLabel)
        this->filterStatusLabel->setText(
                QStringLiteral("Computing distances with %1…").arg(this->m_cosmologyModel));
    ui->labelStatus->setText(
            QStringLiteral("Computing distances with %1…").arg(this->m_cosmologyModel));

    const QString backendUrl   = this->backendUrl;
    const QString backendToken = this->backendToken;
    const QString model        = this->m_cosmologyModel;
    const Catalogue3DSchema schema = this->schema;

    // Collect per-entry redshifts (use 0.0 when absent/invalid – the backend
    // will return 0.0 distance for those and entryDistanceMpc will then fall
    // back to the direct distance field or the 300 Mpc default).
    QVector<double> redshifts;
    redshifts.reserve(static_cast<int>(this->entries.size()));
    for (const auto &e : this->entries) {
        bool ok = false;
        const double z = Catalogue3DParser::detail::fieldDouble(
                e, Catalogue3DParser::detail::detectedRedshiftField(schema), &ok);
        redshifts.append((ok && z > 0.0) ? z : 0.0);
    }

    this->m_cosmologyWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, model, redshifts, schema]()
                    -> Catalogue3DCosmologyTaskResult {
                Catalogue3DCosmologyTaskResult out;
                BackendClient client(backendUrl, backendToken);
                const auto res = client.requestCosmologyDistanceBatch(redshifts, model);
                if (!res.valid) {
                    out.error = res.error.isEmpty()
                            ? QStringLiteral("Backend cosmology request failed.")
                            : res.error;
                    return out;
                }
                out.valid = true;
                out.model = res.model;
                out.distancesMpc.reserve(static_cast<size_t>(res.distancesMpc.size()));
                for (double d : res.distancesMpc)
                    out.distancesMpc.push_back(d);
                return out;
            }));
}

void vtkWindowCatalogue3D::onCosmologyFinished()
{
    this->m_cosmologyBusy = false;
    if (this->comboCosmology)
        this->comboCosmology->setEnabled(true);

    const auto result = this->m_cosmologyWatcher.result();
    if (!result.valid) {
        const QString msg = QStringLiteral("Cosmology update failed: %1").arg(result.error);
        QMessageBox::warning(this, u"Cosmology Error"_s, msg);
        ui->labelStatus->setText(msg);
        if (this->filterStatusLabel)
            this->filterStatusLabel->setText(msg);
        return;
    }

    // Apply computed distances: override entryDistanceMpc() via e.distanceMpc.
    // Entries with returned distance 0 keep the catalogue distance field or the
    // 300 Mpc fallback (entryDistanceMpc checks distanceMpc == 0 → auto-compute).
    const int n = static_cast<int>(std::min(this->entries.size(), result.distancesMpc.size()));
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < n; ++i) {
        auto &e = this->entries[static_cast<size_t>(i)];
        e.distanceMpc = result.distancesMpc[static_cast<size_t>(i)];
        // Recompute FK5 Cartesian; applyFrameToEntries() will convert if Galactic.
        const double d = Catalogue3DParser::detail::entryDistanceMpc(e, this->schema);
        const double ra  = e.raDeg  * pi / 180.0;
        const double dec = e.decDeg * pi / 180.0;
        e.sceneX = d * std::cos(dec) * std::cos(ra);
        e.sceneY = d * std::cos(dec) * std::sin(ra);
        e.sceneZ = d * std::sin(dec);
    }
    // Reapply galactic frame if active (it uses entryDistanceMpc, now updated).
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

    const QString statusMsg = QStringLiteral("Cosmology: %1 – %2 sources.")
            .arg(result.model)
            .arg(this->entries.size());
    ui->labelStatus->setText(statusMsg);
    if (this->filterStatusLabel)
        this->filterStatusLabel->setText(statusMsg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCatalogue3D::applyFilter()
{
    if (this->remoteDatasetId.isEmpty() || this->filterBusy) {
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
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
            u"Catalogue3D"_s,
            nFilters == 0 ? u"Querying dataset (no active filters)."_s
                          : QStringLiteral("Applying %1 filter(s).").arg(nFilters),
            this->remoteDatasetId, this->remoteSessionId, u"filter"_s);

    // Capture everything needed by the background task
    const QString backendUrl = this->backendUrl;
    const QString backendToken = this->backendToken;
    const QString datasetId = this->remoteDatasetId;
    const QString sessionId = this->remoteSessionId;
    const std::vector<BackendTabularFilter> filters = this->activeFilters;
    const Catalogue3DSchema schema = this->schema;
    const int numFilters = nFilters;
    const int pageSize = this->m_pageSize;

    this->filterWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, datasetId, sessionId, filters, schema, numFilters, pageSize]()
                    -> Catalogue3DFilterTaskResult {
                Catalogue3DFilterTaskResult out;

                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) {
                    client.setSessionId(sessionId);
                }
                BackendTabularQueryRequest qreq;
                qreq.datasetId = datasetId;
                qreq.filters = filters;
                qreq.limit = pageSize;
                qreq.offset = 0;

                const auto subset = client.queryTabularCatalogue(qreq);
                if (!subset.valid) {
                    out.error = subset.error;
                    return out;
                }
                const auto parsed = Catalogue3DParser::parseBackendSubset(schema, subset.rows);
                if (!parsed.valid) {
                    out.error = parsed.errorMessage;
                    return out;
                }

                out.entries = parsed.entries;
                out.totalRows = subset.totalRows;
                out.statusMsg =
                        filters.empty()
                                ? QStringLiteral("Loaded %1 of %2 sources.")
                                          .arg(out.entries.size())
                                          .arg(subset.totalRows)
                                : QStringLiteral("%1 filter(s) active — %2 of %3 sources.")
                                          .arg(numFilters)
                                          .arg(out.entries.size())
                                          .arg(subset.totalRows);
                out.valid = true;
                return out;
            }));
}

void vtkWindowCatalogue3D::loadMoreEntries()
{
    if (this->remoteDatasetId.isEmpty() || this->m_loadingMore || this->filterBusy) {
        return;
    }
    if (this->m_totalCount >= 0
        && this->m_currentOffset >= this->m_totalCount) {
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
    const QString datasetId = this->remoteDatasetId;
    const QString sessionId = this->remoteSessionId;
    const std::vector<BackendTabularFilter> filters = this->activeFilters;
    const Catalogue3DSchema schema = this->schema;
    const int offset = this->m_currentOffset;
    const int pageSize = this->m_pageSize;

    this->m_loadMoreWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, datasetId, sessionId, filters, schema, offset, pageSize]()
                    -> Catalogue3DFilterTaskResult {
                Catalogue3DFilterTaskResult out;

                BackendClient client(backendUrl, backendToken);
                if (!sessionId.isEmpty()) {
                    client.setSessionId(sessionId);
                }
                BackendTabularQueryRequest qreq;
                qreq.datasetId = datasetId;
                qreq.filters = filters;
                qreq.limit = pageSize;
                qreq.offset = offset;

                const auto subset = client.queryTabularCatalogue(qreq);
                if (!subset.valid) {
                    out.error = subset.error;
                    return out;
                }
                const auto parsed = Catalogue3DParser::parseBackendSubset(schema, subset.rows);
                if (!parsed.valid) {
                    out.error = parsed.errorMessage;
                    return out;
                }

                out.entries = parsed.entries;
                out.totalRows = subset.totalRows;
                out.valid = true;
                return out;
            }));
}

void vtkWindowCatalogue3D::autoReopenThenLoadMore()
{
    if (this->filterStatusLabel)
        this->filterStatusLabel->setText(u"Session expired — reconnecting\u2026"_s);
    if (this->btnLoadMore) {
        this->btnLoadMore->setEnabled(false);
        this->btnLoadMore->setText(u"Reconnecting\u2026"_s);
    }

    const QString url      = this->backendUrl;
    const QString token    = this->backendToken;
    const QString filePath = this->filepath;

    this->m_reopenWatcher.setFuture(QtConcurrent::run(
            [url, token, filePath]() -> BackendCatalogueInfo {
                BackendClient client(url, token);
                return client.openCatalogue(filePath, catalogueFormatForPath(filePath));
            }));
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCatalogue3D::setupRenderer()
{
    // Attach render window to the Qt VTK widget.
    ui->vtk->setRenderWindow(renderWindow.Get());
    ui->vtk->setEnableTouchEventProcessing(false);
    qInfo() << "[catalogue3d] widget type:" << ui->vtk->metaObject()->className()
            << "touch-processing disabled to match vtkWindowCube";

    renderer->SetBackground(0.06, 0.06, 0.12);
    renderWindow->AddRenderer(renderer);

    // Trackball camera – standard 3D navigation, matching the cube viewer approach:
    // use the stock VTK style and keep catalogue logic in separate observers.
    auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
    ui->vtk->interactor()->SetInteractorStyle(style);
    qInfo() << "[catalogue3d] installed interactor style:" << style->GetClassName();

    // Orientation axes widget (bottom-left corner, non-interactive).
    vtkNew<vtkAxesActor> axesActor;
    axesWidget->SetOrientationMarker(axesActor);
    axesWidget->SetInteractor(ui->vtk->interactor());
    axesWidget->SetViewport(0.0, 0.0, 0.14, 0.14);
    axesWidget->EnabledOn();
    axesWidget->InteractiveOff();

    // ── Mouse observers ───────────────────────────────────────────────────────
    auto cb = vtkSmartPointer<vtkCallbackCommand>::New();
    cb->SetCallback([](vtkObject *, unsigned long eid, void *clientData, void *) {
        static_cast<vtkWindowCatalogue3D *>(clientData)->onMouseEvent(eid);
    });
    cb->SetClientData(this);

    auto *inter = ui->vtk->interactor();
    inter->AddObserver(vtkCommand::MouseMoveEvent,         cb);
    inter->AddObserver(vtkCommand::LeftButtonPressEvent,   cb);
    inter->AddObserver(vtkCommand::LeftButtonReleaseEvent, cb);
    inter->AddObserver(vtkCommand::LeftButtonDoubleClickEvent, cb);
    qInfo() << "[catalogue3d] observer registration complete;"
            << "hover observer is separate from camera interaction";
}

// ─────────────────────────────────────────────────────────────────────────────
// Scene construction
// ─────────────────────────────────────────────────────────────────────────────

/*static*/
std::array<double, 3> vtkWindowCatalogue3D::morphologyColor(const QString &morph,
                                                             int fallbackIndex)
{
    if (kKnownMorphologies.contains(morph))
        return kPalette[kKnownMorphologies[morph]];
    return kPalette[fallbackIndex % static_cast<int>(kPalette.size())];
}

void vtkWindowCatalogue3D::buildMorphologyLut()
{
    // Collect ordered unique morphologies preserving first-seen order.
    for (const auto &e : entries) {
        const QString morph = Catalogue3DParser::detail::entryMorphology(e, this->schema);
        if (!morphologyIndexOf.contains(morph)) {
            morphologyIndexOf[morph] = morphologyNames.size();
            morphologyNames.append(morph);
        }
    }

    const int n = morphologyNames.size();
    morphologyLut->SetNumberOfTableValues(n);
    // Range: use [-0.5, n-0.5] so each integer index falls in its own bin.
    morphologyLut->SetTableRange(-0.5, n - 0.5);

    for (int i = 0; i < n; ++i) {
        const auto [r, g, b] = morphologyColor(morphologyNames.at(i), i);
        morphologyLut->SetTableValue(i, r, g, b, 1.0);
    }
    morphologyLut->Build();

    this->valueLut->SetNumberOfTableValues(256);
    this->valueLut->SetHueRange(0.67, 0.0);
    this->valueLut->SetSaturationRange(0.85, 0.95);
    this->valueLut->SetValueRange(0.9, 1.0);
    this->valueLut->Build();
}

void vtkWindowCatalogue3D::buildScene()
{
    buildMorphologyLut();
    this->mappedPositions.resize(entries.size());

    // ── Populate point cloud ──────────────────────────────────────────────────
    sourcePoints->SetNumberOfPoints(static_cast<vtkIdType>(entries.size()));

    morphScalars->SetName("Morphology");
    morphScalars->SetNumberOfValues(static_cast<vtkIdType>(entries.size()));

    // Compute bounding box to derive a sensible default glyph radius.
    double xMin = std::numeric_limits<double>::max();
    double yMin = xMin, zMin = xMin;
    double xMax = std::numeric_limits<double>::lowest();
    double yMax = xMax, zMax = xMax;

    maxDistanceMpc = 1.0;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const auto &e = entries[i];
        morphScalars->SetValue(i, morphologyIndexOf.value(
                                          Catalogue3DParser::detail::entryMorphology(e, this->schema), 0));

        xMin = std::min(xMin, e.sceneX); xMax = std::max(xMax, e.sceneX);
        yMin = std::min(yMin, e.sceneY); yMax = std::max(yMax, e.sceneY);
        zMin = std::min(zMin, e.sceneZ); zMax = std::max(zMax, e.sceneZ);
        maxDistanceMpc = std::max(maxDistanceMpc,
                                  Catalogue3DParser::detail::entryDistanceMpc(e, this->schema));
    }

    const double diag = std::sqrt((xMax - xMin) * (xMax - xMin)
                                  + (yMax - yMin) * (yMax - yMin)
                                  + (zMax - zMin) * (zMax - zMin));
    // Default glyph radius ≈ 1.2 % of diagonal so fields remain readable.
    defaultGlyphRadius = std::max(0.5, diag * 0.012);
    sizeNormalizationValue = 1.0;

    // Vertices cell array so vtkGlyph3D recognises each point.
    vtkNew<vtkCellArray> verts;
    for (vtkIdType i = 0; i < static_cast<vtkIdType>(entries.size()); ++i)
        verts->InsertNextCell(1, &i);

    sourcesPolyData->SetPoints(sourcePoints);
    sourcesPolyData->SetVerts(verts);
    sourcesPolyData->GetPointData()->SetScalars(morphScalars);
    this->glyphScaleVectors->SetName("ScaleVector");
    this->glyphScaleVectors->SetNumberOfComponents(3);
    this->glyphScaleVectors->SetNumberOfTuples(static_cast<vtkIdType>(entries.size()));
    sourcesPolyData->GetPointData()->SetVectors(this->glyphScaleVectors);

    // ── Glyph pipeline ────────────────────────────────────────────────────────
    sphereSource->SetRadius(0.5); // unit sphere; scaled by SetScaleFactor
    sphereSource->SetPhiResolution(10);
    sphereSource->SetThetaResolution(10);
    pointCubeSource->SetXLength(1.0);
    pointCubeSource->SetYLength(1.0);
    pointCubeSource->SetZLength(1.0);
    vtkNew<vtkPoints> crossPoints;
    vtkNew<vtkCellArray> crossCells;
    const std::array<std::array<double, 3>, 6> crossVertices{ { { -0.5, 0.0, 0.0 },
                                                                 { 0.5, 0.0, 0.0 },
                                                                 { 0.0, -0.5, 0.0 },
                                                                 { 0.0, 0.5, 0.0 },
                                                                 { 0.0, 0.0, -0.5 },
                                                                 { 0.0, 0.0, 0.5 } } };
    for (const auto &v : crossVertices) {
        crossPoints->InsertNextPoint(v[0], v[1], v[2]);
    }
    const std::array<std::array<vtkIdType, 2>, 3> crossLines{ { { 0, 1 }, { 2, 3 }, { 4, 5 } } };
    for (const auto &line : crossLines) {
        crossCells->InsertNextCell(2);
        crossCells->InsertCellPoint(line[0]);
        crossCells->InsertCellPoint(line[1]);
    }
    vtkNew<vtkPolyData> crossData;
    crossData->SetPoints(crossPoints);
    crossData->SetLines(crossCells);
    this->crossSource->RemoveAllInputs();
    this->crossSource->AddInputData(crossData);

    glyphs->SetInputData(sourcesPolyData);
    glyphs->SetSourceConnection(sphereSource->GetOutputPort());
    glyphs->SetScaleModeToScaleByVectorComponents();
    glyphs->SetVectorModeToUseVector();
    glyphs->OrientOff();
    glyphs->SetColorModeToColorByScalar();  // pass morphology index to mapper
    glyphs->SetScaleFactor(1.0);

    const int nMorph = morphologyNames.size();
    glyphMapper->SetInputConnection(glyphs->GetOutputPort());
    glyphMapper->SetLookupTable(morphologyLut);
    glyphMapper->SetScalarRange(-0.5, nMorph - 0.5);
    glyphMapper->SetColorModeToMapScalars();
    glyphMapper->ScalarVisibilityOn();

    glyphActor->SetMapper(glyphMapper);
    renderer->AddActor(glyphActor);
    this->pointsMapper->SetInputData(sourcesPolyData);
    this->pointsMapper->SetLookupTable(this->morphologyLut);
    this->pointsMapper->SetScalarRange(-0.5, nMorph - 0.5);
    this->pointsMapper->ScalarVisibilityOn();
    this->pointsActor->SetMapper(this->pointsMapper);
    this->pointsActor->GetProperty()->SetPointSize(4.0);
    this->pointsActor->GetProperty()->SetColor(0.78, 0.82, 0.95);
    this->pointsActor->VisibilityOff();
    renderer->AddActor(this->pointsActor);

    // ── Hover highlight (yellow wireframe) ────────────────────────────────────
    hoverSphere->SetRadius(defaultGlyphRadius * 1.35);
    hoverSphere->SetPhiResolution(12);
    hoverSphere->SetThetaResolution(12);

    hoverMapper->SetInputConnection(hoverSphere->GetOutputPort());
    hoverActor->SetMapper(hoverMapper);
    hoverActor->GetProperty()->SetRepresentationToWireframe();
    hoverActor->GetProperty()->SetColor(1.0, 0.95, 0.0); // yellow
    hoverActor->GetProperty()->SetLineWidth(1.8);
    hoverActor->VisibilityOff();
    renderer->AddActor(hoverActor);

    // ── Selection highlight (bright white wireframe) ───────────────────────────
    selectSphere->SetRadius(defaultGlyphRadius * 1.18);
    selectSphere->SetPhiResolution(12);
    selectSphere->SetThetaResolution(12);

    selectMapper->SetInputConnection(selectSphere->GetOutputPort());
    selectActor->SetMapper(selectMapper);
    selectActor->GetProperty()->SetRepresentationToWireframe();
    selectActor->GetProperty()->SetColor(1.0, 1.0, 1.0); // white
    selectActor->GetProperty()->SetLineWidth(2.2);
    selectActor->VisibilityOff();
    renderer->AddActor(selectActor);

    // ── Bounding-box coordinate axes ──────────────────────────────────────────
    const double bounds[6] = { xMin, xMax, yMin, yMax, zMin, zMax };
    cubeAxesActor->SetBounds(bounds);
    cubeAxesActor->SetCamera(renderer->GetActiveCamera());
    cubeAxesActor->SetXTitle("X (Mpc)");
    cubeAxesActor->SetYTitle("Y (Mpc)");
    cubeAxesActor->SetZTitle("Z (Mpc)");
    cubeAxesActor->SetXUnits("");
    cubeAxesActor->SetYUnits("");
    cubeAxesActor->SetZUnits("");
    // Axis colours: red / green / blue
    for (int axis = 0; axis < 3; ++axis) {
        cubeAxesActor->GetTitleTextProperty(axis)->SetColor(
                axis == 0 ? 1.0 : 0.5, axis == 1 ? 1.0 : 0.5, axis == 2 ? 1.0 : 0.5);
        cubeAxesActor->GetLabelTextProperty(axis)->SetColor(0.8, 0.8, 0.8);
    }
    cubeAxesActor->SetFlyModeToOuterEdges();
    cubeAxesActor->DrawXGridlinesOff();
    cubeAxesActor->DrawYGridlinesOff();
    cubeAxesActor->DrawZGridlinesOff();
    renderer->AddActor(cubeAxesActor);
    cubeAxesActor->SetVisibility(this->chkShowBoundingBox && this->chkShowBoundingBox->isChecked() ? 1 : 0);

    this->buildShells();
    this->updatePointPositions();
    this->updateColorMapping();
    this->updateGlyphPipeline();

    renderer->ResetCamera();
    renderWindow->Render();
}

void vtkWindowCatalogue3D::buildLabels()
{
    for (const auto &e : entries) {
        auto actor = vtkSmartPointer<vtkBillboardTextActor3D>::New();
        actor->SetInput(Catalogue3DParser::detail::entryDisplayName(e, this->schema).toUtf8().constData());
        actor->SetPosition(e.sceneX, e.sceneY, e.sceneZ);
        actor->GetTextProperty()->SetFontSize(11);
        actor->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        actor->GetTextProperty()->ShadowOn();
        actor->GetTextProperty()->SetShadowOffset(1, -1);
        actor->SetVisibility(0); // hidden until user enables
        labelActors.push_back(actor);
        renderer->AddActor(actor);
    }
}

void vtkWindowCatalogue3D::buildShells()
{
    for (const auto &actor : this->shellActors) {
        this->renderer->RemoveActor(actor);
    }
    this->shellActors.clear();
    if (entries.empty()) {
        return;
    }

    const std::array<double, 3> shellFractions{ { 0.33, 0.66, 1.0 } };
    for (double fraction : shellFractions) {
        vtkNew<vtkSphereSource> shellSource;
        shellSource->SetRadius(std::max(1.0, this->maxDistanceMpc * fraction));
        shellSource->SetPhiResolution(48);
        shellSource->SetThetaResolution(48);
        vtkNew<vtkPolyDataMapper> shellMapper;
        shellMapper->SetInputConnection(shellSource->GetOutputPort());
        auto shellActor = vtkSmartPointer<vtkActor>::New();
        shellActor->SetMapper(shellMapper);
        shellActor->GetProperty()->SetRepresentationToWireframe();
        shellActor->GetProperty()->SetColor(0.42, 0.5, 0.66);
        shellActor->GetProperty()->SetOpacity(0.2);
        shellActor->VisibilityOff();
        this->renderer->AddActor(shellActor);
        this->shellActors.push_back(shellActor);
    }
}

QStringList vtkWindowCatalogue3D::numericRawFieldNames() const
{
    QStringList items;
    for (const int index : this->numericRawFieldIndices) {
        if (index >= 0 && index < this->rawHeaders.size()) {
            items.append(this->rawHeaders.at(index));
        }
    }
    return items;
}

bool vtkWindowCatalogue3D::rawFieldValue(const Catalogue3DEntry &entry, int fieldIndex,
                                         double &value) const
{
    if (fieldIndex < 0 || fieldIndex >= entry.rawFieldValues.size()) {
        return false;
    }
    return Catalogue3DParser::detail::maybeNumeric(entry.rawFieldValues.at(fieldIndex), value);
}

double vtkWindowCatalogue3D::mappedAxisValue(const Catalogue3DEntry &entry, const QString &mappingKey) const
{
    if (mappingKey == QStringLiteral("computed:x")) {
        return entry.sceneX;
    }
    if (mappingKey == QStringLiteral("computed:y")) {
        return entry.sceneY;
    }
    if (mappingKey == QStringLiteral("computed:z")) {
        return entry.sceneZ;
    }
    if (mappingKey.startsWith(QStringLiteral("raw:"))) {
        bool okIndex = false;
        const int index = mappingKey.mid(4).toInt(&okIndex);
        double value = 0.0;
        if (okIndex && this->rawFieldValue(entry, index, value)) {
            return value;
        }
    }
    return 0.0;
}

double vtkWindowCatalogue3D::mappedSizeValue(const Catalogue3DEntry &entry, bool &ok) const
{
    ok = true;
    if (this->mapSizeKey == QStringLiteral("builtin:fixed")) {
        return 1.0;
    }
    if (this->mapSizeKey == QStringLiteral("builtin:major")) {
        const double value = Catalogue3DParser::detail::entryMajorAxisArcmin(entry, this->schema);
        if (value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    if (this->mapSizeKey == QStringLiteral("builtin:lls")) {
        const double value = Catalogue3DParser::detail::entryLlsMajorKpc(entry, this->schema);
        if (value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    if (this->mapSizeKey == QStringLiteral("builtin:flux")) {
        const double value = Catalogue3DParser::detail::entryFluxMJy(entry, this->schema);
        if (value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    if (this->mapSizeKey.startsWith(QStringLiteral("raw:"))) {
        bool okIndex = false;
        const int index = this->mapSizeKey.mid(4).toInt(&okIndex);
        double value = 0.0;
        if (okIndex && this->rawFieldValue(entry, index, value) && value > 0.0) {
            return value;
        }
        ok = false;
        return 1.0;
    }
    ok = false;
    return 1.0;
}

double vtkWindowCatalogue3D::mappedColorValue(const Catalogue3DEntry &entry, bool &ok) const
{
    ok = true;
    if (this->mapColorKey.startsWith(QStringLiteral("raw:"))) {
        bool okIndex = false;
        const int index = this->mapColorKey.mid(4).toInt(&okIndex);
        double value = 0.0;
        if (okIndex && this->rawFieldValue(entry, index, value)) {
            return value;
        }
        ok = false;
        return 0.0;
    }
    ok = false;
    return 0.0;
}

QString vtkWindowCatalogue3D::fallbackDetailsText(const Catalogue3DEntry &entry) const
{
    QString text;
    for (int i = 0; i < this->rawHeaders.size() && i < entry.rawFieldValues.size(); ++i) {
        const QString value = entry.rawFieldValues.at(i).trimmed();
        if (value.isEmpty()) {
            continue;
        }
        text += QStringLiteral("%1: %2\n").arg(this->rawHeaders.at(i), value);
    }
    return text.trimmed();
}

void vtkWindowCatalogue3D::updatePointPositions()
{
    if (this->entries.empty()) {
        return;
    }

    const auto validatedAxisKey = [this](const QString &requestedKey, const QString &fallbackKey) {
        if (!requestedKey.startsWith(QStringLiteral("raw:"))) {
            return requestedKey;
        }
        int validCount = 0;
        double minValue = std::numeric_limits<double>::max();
        double maxValue = std::numeric_limits<double>::lowest();
        for (const auto &entry : this->entries) {
            const double value = this->mappedAxisValue(entry, requestedKey);
            if (!std::isfinite(value)) {
                continue;
            }
            ++validCount;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
        if (validCount < 3 || !(minValue < maxValue)) {
            qWarning() << "[catalogue3d] invalid axis mapping" << requestedKey
                       << "-> fallback to" << fallbackKey;
            return fallbackKey;
        }
        return requestedKey;
    };

    const QString resolvedXKey = validatedAxisKey(this->mapXKey, QStringLiteral("computed:x"));
    const QString resolvedYKey = validatedAxisKey(this->mapYKey, QStringLiteral("computed:y"));
    const QString resolvedZKey = validatedAxisKey(this->mapZKey, QStringLiteral("computed:z"));

    double xMin = std::numeric_limits<double>::max();
    double yMin = xMin;
    double zMin = xMin;
    double xMax = std::numeric_limits<double>::lowest();
    double yMax = xMax;
    double zMax = xMax;

    for (vtkIdType i = 0; i < static_cast<vtkIdType>(this->entries.size()); ++i) {
        const auto &entry = this->entries[static_cast<std::size_t>(i)];
        const double x = this->mappedAxisValue(entry, resolvedXKey);
        const double y = this->mappedAxisValue(entry, resolvedYKey);
        const double z = this->mappedAxisValue(entry, resolvedZKey);
        this->mappedPositions[static_cast<std::size_t>(i)] = { x, y, z };
        this->sourcePoints->SetPoint(i, x, y, z);
        xMin = std::min(xMin, x); xMax = std::max(xMax, x);
        yMin = std::min(yMin, y); yMax = std::max(yMax, y);
        zMin = std::min(zMin, z); zMax = std::max(zMax, z);
        if (i < static_cast<vtkIdType>(this->labelActors.size())) {
            this->labelActors[static_cast<std::size_t>(i)]->SetPosition(x, y, z);
        }
    }
    this->sourcePoints->Modified();
    this->sourcesPolyData->Modified();
    const double bounds[6] = { xMin, xMax, yMin, yMax, zMin, zMax };
    this->cubeAxesActor->SetBounds(bounds);
    this->renderer->ResetCameraClippingRange();
    this->updateHighlightSphere(this->hoverSphere.Get(), this->hoverActor.Get(), this->hoveredIndex, 1.35);
    this->updateHighlightSphere(this->selectSphere.Get(), this->selectActor.Get(), this->selectedIndex, 1.18);
    qInfo() << "[catalogue3d] axis mapping updated"
            << "X=" << this->mapXKey << "resolved=" << resolvedXKey
            << "Y=" << this->mapYKey << "resolved=" << resolvedYKey
            << "Z=" << this->mapZKey << "resolved=" << resolvedZKey;
}

void vtkWindowCatalogue3D::updateColorMapping()
{
    if (this->entries.empty()) {
        return;
    }

    if (this->mapColorKey == QStringLiteral("builtin:morphology")) {
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(this->entries.size()); ++i) {
            const auto &entry = this->entries[static_cast<std::size_t>(i)];
            this->morphScalars->SetValue(
                    i, this->morphologyIndexOf.value(
                               Catalogue3DParser::detail::entryMorphology(entry, this->schema), 0));
        }
        this->morphScalars->Modified();
        this->sourcesPolyData->GetPointData()->SetScalars(this->morphScalars);
        this->glyphMapper->SetLookupTable(this->morphologyLut);
        this->glyphMapper->SetScalarRange(-0.5, this->morphologyNames.size() - 0.5);
        this->pointsMapper->SetLookupTable(this->morphologyLut);
        this->pointsMapper->SetScalarRange(-0.5, this->morphologyNames.size() - 0.5);
        qInfo() << "[catalogue3d] color mapping: morphology";
        return;
    }

    double minValue = std::numeric_limits<double>::max();
    double maxValue = std::numeric_limits<double>::lowest();
    bool anyValid = false;
    for (vtkIdType i = 0; i < static_cast<vtkIdType>(this->entries.size()); ++i) {
        bool ok = false;
        const double value = this->mappedColorValue(this->entries[static_cast<std::size_t>(i)], ok);
        if (!ok || !std::isfinite(value)) {
            this->morphScalars->SetValue(i, 0.0);
            continue;
        }
        this->morphScalars->SetValue(i, value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        anyValid = true;
    }

    if (!anyValid || !(minValue < maxValue)) {
        qWarning() << "[catalogue3d] invalid color mapping" << this->mapColorKey
                   << "-> fallback to morphology";
        this->mapColorKey = QStringLiteral("builtin:morphology");
        if (this->comboMapColor) {
            QSignalBlocker blocker(this->comboMapColor);
            this->comboMapColor->setCurrentIndex(0);
        }
        this->updateColorMapping();
        return;
    }

    this->morphScalars->Modified();
    this->sourcesPolyData->GetPointData()->SetScalars(this->morphScalars);
    this->valueLut->SetTableRange(minValue, maxValue);
    this->valueLut->Build();
    this->glyphMapper->SetLookupTable(this->valueLut);
    this->glyphMapper->SetScalarRange(minValue, maxValue);
    this->pointsMapper->SetLookupTable(this->valueLut);
    this->pointsMapper->SetScalarRange(minValue, maxValue);
    qInfo() << "[catalogue3d] color mapping:" << this->mapColorKey
            << "range:" << minValue << maxValue;
}

void vtkWindowCatalogue3D::updateGlyphPipeline()
{
    if (entries.empty()) {
        return;
    }

    this->pointsActor->VisibilityOff();
    this->glyphActor->VisibilityOn();
    switch (this->geometryMode) {
    case GeometryMode::Ellipsoid:
    case GeometryMode::Sphere:
        this->glyphs->SetSourceConnection(this->sphereSource->GetOutputPort());
        break;
    case GeometryMode::Cross:
        this->glyphs->SetSourceConnection(this->crossSource->GetOutputPort());
        break;
    case GeometryMode::Point:
        this->glyphActor->VisibilityOff();
        this->pointsActor->VisibilityOn();
        break;
    }

    this->updateGlyphScales();
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::updateGlyphScales()
{
    if (entries.empty()) {
        return;
    }

    sizeNormalizationValue = 1.0;
    bool anyValid = false;
    for (const auto &entry : entries) {
        bool ok = false;
        const double value = this->mappedSizeValue(entry, ok);
        if (!ok || !std::isfinite(value) || value <= 0.0) {
            continue;
        }
        sizeNormalizationValue = std::max(sizeNormalizationValue, value);
        anyValid = true;
    }
    if (!anyValid) {
        qWarning() << "[catalogue3d] invalid size mapping" << this->mapSizeKey
                   << "-> fallback to fixed size";
        this->mapSizeKey = QStringLiteral("builtin:fixed");
        if (this->comboMapSize) {
            QSignalBlocker blocker(this->comboMapSize);
            this->comboMapSize->setCurrentIndex(0);
        }
        this->sizeMode = SizeMode::Fixed;
        sizeNormalizationValue = 1.0;
    }

    const double sliderScale = ui->sliderScale->value() / 100.0;
    double minVisual = std::numeric_limits<double>::max();
    double maxVisual = 0.0;
    for (vtkIdType i = 0; i < static_cast<vtkIdType>(entries.size()); ++i) {
        const Catalogue3DEntry &entry = entries[static_cast<std::size_t>(i)];
        bool ok = false;
        const double sourceValue = this->mappedSizeValue(entry, ok);
        const double normalized = ok && sourceValue > 0.0
                ? std::clamp(sourceValue / sizeNormalizationValue, 0.15, 1.6)
                : 0.35;
        double major = defaultGlyphRadius * sliderScale * normalized;
        double minor = major;
        double depth = major;

        if (this->geometryMode == GeometryMode::Ellipsoid) {
            const double majorAxisArcmin =
                    Catalogue3DParser::detail::entryMajorAxisArcmin(entry, this->schema);
            const double minorAxisArcmin =
                    Catalogue3DParser::detail::entryMinorAxisArcmin(entry, this->schema);
            const double llsMajorKpc =
                    Catalogue3DParser::detail::entryLlsMajorKpc(entry, this->schema);
            const double llsMinorKpc =
                    Catalogue3DParser::detail::entryLlsMinorKpc(entry, this->schema);
            if (this->mapSizeKey == QStringLiteral("builtin:major") && majorAxisArcmin > 0.0) {
                major = defaultGlyphRadius * sliderScale * std::max(0.15, majorAxisArcmin / sizeNormalizationValue);
                const double minorNorm = minorAxisArcmin > 0.0
                        ? std::max(0.12, minorAxisArcmin / sizeNormalizationValue)
                        : std::max(0.12, majorAxisArcmin / sizeNormalizationValue * 0.65);
                minor = defaultGlyphRadius * sliderScale * minorNorm;
                depth = minor;
            } else if (this->mapSizeKey == QStringLiteral("builtin:lls") && llsMajorKpc > 0.0) {
                major = defaultGlyphRadius * sliderScale * std::max(0.15, llsMajorKpc / sizeNormalizationValue);
                const double minorNorm = llsMinorKpc > 0.0
                        ? std::max(0.12, llsMinorKpc / sizeNormalizationValue)
                        : std::max(0.12, llsMajorKpc / sizeNormalizationValue * 0.7);
                minor = defaultGlyphRadius * sliderScale * minorNorm;
                depth = minor;
            }
        }

        if (this->geometryMode == GeometryMode::Sphere || this->geometryMode == GeometryMode::Point
            || this->geometryMode == GeometryMode::Cross) {
            major = minor = depth = defaultGlyphRadius * sliderScale * normalized;
        }

        this->glyphScaleVectors->SetTuple3(i, minor, major, depth);
        minVisual = std::min(minVisual, std::min({ minor, major, depth }));
        maxVisual = std::max(maxVisual, std::max({ minor, major, depth }));
    }
    this->glyphScaleVectors->Modified();
    this->glyphs->Modified();
    this->pointsActor->GetProperty()->SetPointSize(std::clamp(4.0 * (ui->sliderScale->value() / 100.0), 3.0, 12.0));

    updateHighlightSphere(hoverSphere.Get(), hoverActor.Get(), hoveredIndex, 1.45);
    updateHighlightSphere(selectSphere.Get(), selectActor.Get(), selectedIndex, 1.28);
    qInfo() << "[catalogue3d] size mapping:" << this->mapSizeKey
            << "visual range:" << minVisual << maxVisual;
}

// ─────────────────────────────────────────────────────────────────────────────
// Interaction
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCatalogue3D::onMouseEvent(unsigned long eid)
{
    int x = 0, y = 0;
    ui->vtk->interactor()->GetEventPosition(x, y);

    if (eid == vtkCommand::LeftButtonDoubleClickEvent) {
        const int idx = entries.empty() ? -1 : pickNearestSource(x, y);
        qInfo() << "[catalogue3d] double-click observer fired, picked source:" << idx;
        if (idx >= 0) {
            const int newSelection = (idx == selectedIndex) ? -1 : idx;
            qInfo() << "[catalogue3d] double-click selection index:" << newSelection
                    << "(no zoom action executed)";
            setSelectedSource(newSelection);
        } else {
            qInfo() << "[catalogue3d] double-click miss; clearing selection"
                    << "(no zoom action executed)";
            setSelectedSource(-1);
        }
        this->leftClickCandidate = false;
        return;
    }

    if (eid == vtkCommand::LeftButtonPressEvent) {
        leftButtonDown = true;
        leftClickCandidate = true;
        pressX = x;
        pressY = y;
        pressPickIndex = entries.empty() ? -1 : pickNearestSource(x, y);
        qInfo() << "[catalogue3d] left press observed; camera remains owned by VTK style";
        qInfo() << "[catalogue3d] left press position:" << pressX << pressY
                << "press-pick-index:" << pressPickIndex;
        return;
    }

    if (eid == vtkCommand::LeftButtonReleaseEvent) {
        leftButtonDown = false;
        // Treat as a click only when the pointer hasn't moved significantly.
        const int dx = x - pressX;
        const int dy = y - pressY;
        qInfo() << "[catalogue3d] left release position:" << x << y
                << "click-candidate dx/dy:" << dx << dy;
        const bool isClick = this->leftClickCandidate
                && (dx * dx + dy * dy <= clickDragThresholdPx * clickDragThresholdPx);
        this->leftClickCandidate = false;
        if (isClick && !entries.empty()) {
            qInfo() << "[catalogue3d] click accepted with threshold:" << clickDragThresholdPx;
            int idx = this->pressPickIndex;
            if (idx < 0) {
                idx = pickNearestSource(x, y);
            }
            qInfo() << "[catalogue3d] click selection hit/miss index:" << idx;
            if (idx >= 0) {
                setSelectedSource(idx == selectedIndex ? -1 : idx);
            } else {
                qInfo() << "[catalogue3d] selection cleared by empty click";
                setSelectedSource(-1);
            }
        } else if (isClick) {
            qInfo() << "[catalogue3d] click accepted on empty scene; selection cleared";
            setSelectedSource(-1);
        } else {
            qInfo() << "[catalogue3d] drag rejected for selection";
        }
        return;
    }

    // MouseMoveEvent: no hover activation/highlighting. Camera drag remains owned by VTK.
    if (leftButtonDown) {
        qInfo() << "[catalogue3d] drag mouse move observed -> camera interaction path";
    }
}

int vtkWindowCatalogue3D::pickNearestSource(int displayX, int displayY) const
{
    vtkNew<vtkCoordinate> coord;
    coord->SetCoordinateSystemToWorld();

    double bestDist2 = hoverThresholdPx * hoverThresholdPx;
    int bestIdx = -1;

    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const auto &position = this->mappedPositions[static_cast<std::size_t>(i)];
        coord->SetValue(position[0], position[1], position[2]);
        // GetComputedDisplayValue returns int pixel coords [x, y].
        const int *dp = coord->GetComputedDisplayValue(renderer.Get());
        const double dx = dp[0] - displayX;
        const double dy = dp[1] - displayY;
        const double dist2 = dx * dx + dy * dy;
        if (dist2 < bestDist2) {
            bestDist2 = dist2;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void vtkWindowCatalogue3D::updateHighlightSphere(vtkSphereSource *sphere, vtkActor *actor,
                                                  int idx, double radiusFactor)
{
    if (idx < 0) {
        actor->VisibilityOff();
        return;
    }
    const auto &position = this->mappedPositions[static_cast<std::size_t>(idx)];
    double tuple[3] = { defaultGlyphRadius, defaultGlyphRadius, defaultGlyphRadius };
    this->glyphScaleVectors->GetTuple(idx, tuple);
    const double r = std::max({ tuple[0], tuple[1], tuple[2] }) * radiusFactor;
    sphere->SetCenter(position[0], position[1], position[2]);
    sphere->SetRadius(r);
    sphere->Update();
    actor->VisibilityOn();
}

void vtkWindowCatalogue3D::setHoveredSource(int idx)
{
    if (idx == hoveredIndex)
        return;
    hoveredIndex = idx;
    updateHighlightSphere(hoverSphere.Get(), hoverActor.Get(), idx, 1.35);
    renderWindow->Render();
}

void vtkWindowCatalogue3D::setSelectedSource(int idx)
{
    selectedIndex = idx;
    if (hoveredIndex >= 0) {
        hoveredIndex = -1;
        hoverActor->VisibilityOff();
    }
    updateHighlightSphere(selectSphere.Get(), selectActor.Get(), idx, 1.18);
    updateInfoPanel();
    qInfo() << "[catalogue3d] selected source index:" << idx;
    emit this->sourceSelectionChanged(idx);
    renderWindow->Render();
}

// ─────────────────────────────────────────────────────────────────────────────
// Info panel
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCatalogue3D::updateInfoPanel()
{
    if (selectedIndex < 0) {
        ui->labelInfo->setText(u"No source selected"_s);
        qInfo() << "[catalogue3d] details panel updated from selection: none";
        return;
    }

    const Catalogue3DEntry &e = entries[selectedIndex];
    const QString displayName = Catalogue3DParser::detail::entryDisplayName(e, this->schema);
    const QString morphology = Catalogue3DParser::detail::entryMorphology(e, this->schema);
    const double distanceMpc = Catalogue3DParser::detail::entryDistanceMpc(e, this->schema);
    const double redshift =
            Catalogue3DParser::detail::fieldDouble(e, Catalogue3DParser::detail::detectedRedshiftField(this->schema));
    const double majorAxisArcmin =
            Catalogue3DParser::detail::entryMajorAxisArcmin(e, this->schema);
    const double minorAxisArcmin =
            Catalogue3DParser::detail::entryMinorAxisArcmin(e, this->schema);
    const double llsMajorKpc =
            Catalogue3DParser::detail::entryLlsMajorKpc(e, this->schema);
    const double llsMinorKpc =
            Catalogue3DParser::detail::entryLlsMinorKpc(e, this->schema);
    const double fluxMJy =
            Catalogue3DParser::detail::entryFluxMJy(e, this->schema);
    QString html;
    html += QStringLiteral("<b>%1</b>").arg(displayName.toHtmlEscaped());
    html += QStringLiteral("<br>RA: %1°").arg(e.raDeg, 0, 'f', 4);
    html += QStringLiteral("<br>Dec: %1°").arg(e.decDeg, 0, 'f', 4);
    if (redshift > 0.0)
        html += QStringLiteral("<br>z: %1").arg(redshift, 0, 'f', 4);
    if (distanceMpc > 0.0)
        html += QStringLiteral("<br>d: %1 Mpc").arg(distanceMpc, 0, 'f', 1);
    html += QStringLiteral("<br>Type: <i>%1</i>").arg(morphology.toHtmlEscaped());
    if (majorAxisArcmin > 0.0) {
        if (minorAxisArcmin > 0.0)
            html += QStringLiteral("<br>Size: %1′ × %2′")
                            .arg(majorAxisArcmin, 0, 'f', 1)
                            .arg(minorAxisArcmin, 0, 'f', 1);
        else
            html += QStringLiteral("<br>Size: %1′").arg(majorAxisArcmin, 0, 'f', 1);
    }
    if (llsMajorKpc > 0.0) {
        if (llsMinorKpc > 0.0)
            html += QStringLiteral("<br>LLS: %1 × %2 kpc")
                            .arg(llsMinorKpc, 0, 'f', 1)
                            .arg(llsMajorKpc, 0, 'f', 1);
        else
            html += QStringLiteral("<br>LLS: %1 kpc").arg(llsMajorKpc, 0, 'f', 1);
    }
    if (fluxMJy > 0.0)
        html += QStringLiteral("<br>Flux: %1 mJy").arg(fluxMJy, 0, 'f', 2);
    html += QStringLiteral(
            "<br><br><span style='color:#b0b8c8'>3D depth comes from RA/Dec + redshift or distance. Source shape is parametric.</span>");

    ui->labelInfo->setText(html);
    qInfo() << "[catalogue3d] details panel updated from selection:" << selectedIndex;
}

// ─────────────────────────────────────────────────────────────────────────────
// Slots
// ─────────────────────────────────────────────────────────────────────────────

void vtkWindowCatalogue3D::toggleLabels(bool checked)
{
    for (auto &actor : labelActors)
        actor->SetVisibility(checked ? 1 : 0);
    if (!entries.empty())
        renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleMorphColors(bool checked)
{
    glyphMapper->SetScalarVisibility(checked ? 1 : 0);
    pointsMapper->SetScalarVisibility(checked ? 1 : 0);
    if (!checked)
        glyphActor->GetProperty()->SetColor(0.75, 0.75, 0.85);
    if (!checked)
        pointsActor->GetProperty()->SetColor(0.75, 0.75, 0.85);
    if (checked) {
        this->updateColorMapping();
    }
    if (!entries.empty())
        renderWindow->Render();
}

void vtkWindowCatalogue3D::scaleChanged(int value)
{
    if (entries.empty())
        return;
    Q_UNUSED(value);
    this->updateGlyphScales();
    renderWindow->Render();
}

void vtkWindowCatalogue3D::resetCamera()
{
    renderer->ResetCamera();
    renderWindow->Render();
}

void vtkWindowCatalogue3D::updateGeometryMode()
{
    switch (this->comboGeometry ? this->comboGeometry->currentIndex() : 0) {
    case 1:
        this->geometryMode = GeometryMode::Sphere;
        break;
    case 2:
        this->geometryMode = GeometryMode::Point;
        break;
    case 3:
        this->geometryMode = GeometryMode::Cross;
        break;
    default:
        this->geometryMode = GeometryMode::Ellipsoid;
        break;
    }
    this->updateGlyphPipeline();
}

void vtkWindowCatalogue3D::updateSizeMode()
{
    this->mapSizeKey = this->comboSizeMode ? this->comboSizeMode->currentData().toString()
                                           : QStringLiteral("builtin:fixed");
    if (this->mapSizeKey == QStringLiteral("builtin:major")) {
        this->sizeMode = SizeMode::MajorAxis;
    } else if (this->mapSizeKey == QStringLiteral("builtin:lls")) {
        this->sizeMode = SizeMode::Lls;
    } else if (this->mapSizeKey == QStringLiteral("builtin:flux")) {
        this->sizeMode = SizeMode::Flux;
    } else {
        this->sizeMode = SizeMode::Fixed;
    }
    this->updateGlyphScales();
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleSceneAxes(bool checked)
{
    this->axesWidget->SetEnabled(checked ? 1 : 0);
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleBoundingBox(bool checked)
{
    this->cubeAxesActor->SetVisibility(checked ? 1 : 0);
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::toggleShells(bool checked)
{
    for (const auto &actor : this->shellActors) {
        actor->SetVisibility(checked ? 1 : 0);
    }
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::ensureCatalogueDock()
{
    if (this->catalogueDock) {
        return;
    }
    this->catalogueDock = new QDockWidget(u"Catalogue"_s, this);
    this->catalogueDock->setObjectName(u"Catalogue3DDock"_s);
    auto *dockWidget = new QWidget(this->catalogueDock);
    auto *dockLayout = new QVBoxLayout(dockWidget);
    dockLayout->setContentsMargins(4, 4, 4, 4);
    dockLayout->setSpacing(0);

    // Table only — mappings live in the Visualization sidebar page;
    // selected-source details live in the Source Info sidebar page.
    this->catalogueTableView = new QTableView(dockWidget);
    this->catalogueTableModel = new Catalogue3DTableModel(this->catalogueDock);
    this->catalogueTableView->setModel(this->catalogueTableModel);
    this->catalogueTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->catalogueTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    this->catalogueTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->catalogueTableView->setAlternatingRowColors(true);
    this->catalogueTableView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->catalogueTableView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->catalogueTableView->verticalHeader()->setVisible(false);
    this->catalogueTableView->horizontalHeader()->setStretchLastSection(false);
    this->catalogueTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    dockLayout->addWidget(this->catalogueTableView, 1);

    this->catalogueDock->setWidget(dockWidget);
    this->addDockWidget(Qt::BottomDockWidgetArea, this->catalogueDock);
    QObject::connect(this, &vtkWindowCatalogue3D::sourceSelectionChanged, this,
                     &vtkWindowCatalogue3D::syncTableSelection);
    QObject::connect(this->catalogueTableView->selectionModel(), &QItemSelectionModel::currentRowChanged,
                     this, [this](const QModelIndex &current) {
        if (this->syncingTableSelection) {
            return;
        }
        this->setSelectedSource(current.isValid() ? current.row() : -1);
    });
    QObject::connect(this->catalogueTableView, &QTableView::doubleClicked, this,
                     [this](const QModelIndex &index) {
        if (index.isValid()) {
            this->setSelectedSource(index.row());
            this->centerOnSource(index.row());
        }
    });
    QObject::connect(this->comboMapX, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapX || this->updatingMappingControls) {
            return;
        }
        this->mapXKey = this->comboMapX->currentData().toString();
        this->updatePointPositions();
        this->renderWindow->Render();
    });
    QObject::connect(this->comboMapY, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapY || this->updatingMappingControls) {
            return;
        }
        this->mapYKey = this->comboMapY->currentData().toString();
        this->updatePointPositions();
        this->renderWindow->Render();
    });
    QObject::connect(this->comboMapZ, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapZ || this->updatingMappingControls) {
            return;
        }
        this->mapZKey = this->comboMapZ->currentData().toString();
        this->updatePointPositions();
        this->renderWindow->Render();
    });
    QObject::connect(this->comboMapColor, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!this->comboMapColor || this->updatingMappingControls) {
            return;
        }
        this->mapColorKey = this->comboMapColor->currentData().toString();
        this->updateColorMapping();
        this->renderWindow->Render();
    });
}

void vtkWindowCatalogue3D::refreshCatalogueTable()
{
    if (!this->catalogueTableModel) {
        return;
    }
    this->catalogueTableModel->setCatalogue(&this->entries, &this->schema);
    if (this->catalogueTableView) {
        this->catalogueTableView->resizeColumnsToContents();
    }
}

void vtkWindowCatalogue3D::populateMappingControls()
{
    if (!this->comboMapX || !this->comboMapY || !this->comboMapZ || !this->comboMapSize
        || !this->comboMapColor) {
        return;
    }

    this->numericRawFieldIndices.clear();
    for (int column = 0; column < static_cast<int>(this->schema.fields.size()); ++column) {
        const QString type = this->schema.fields[static_cast<std::size_t>(column)].type.toLower();
        if (type == QStringLiteral("float") || type == QStringLiteral("int")) {
            this->numericRawFieldIndices.push_back(column);
        }
    }

    this->updatingMappingControls = true;
    const auto fillAxisCombo = [this](QComboBox *combo, const QString &currentKey, const QString &defaultKey) {
        combo->clear();
        combo->addItem(u"Computed Cartesian X"_s, QStringLiteral("computed:x"));
        combo->addItem(u"Computed Cartesian Y"_s, QStringLiteral("computed:y"));
        combo->addItem(u"Computed Cartesian Z"_s, QStringLiteral("computed:z"));
        for (const int index : this->numericRawFieldIndices) {
            combo->addItem(QStringLiteral("raw:%1").arg(this->rawHeaders.value(index)),
                           QStringLiteral("raw:%1").arg(index));
        }
        int comboIndex = combo->findData(currentKey);
        if (comboIndex < 0) {
            comboIndex = combo->findData(defaultKey);
        }
        combo->setCurrentIndex(std::max(0, comboIndex));
    };
    fillAxisCombo(this->comboMapX, this->mapXKey, QStringLiteral("computed:x"));
    fillAxisCombo(this->comboMapY, this->mapYKey, QStringLiteral("computed:y"));
    fillAxisCombo(this->comboMapZ, this->mapZKey, QStringLiteral("computed:z"));

    this->comboMapSize->clear();
    this->comboMapSize->addItem(u"Fixed"_s, QStringLiteral("builtin:fixed"));
    this->comboMapSize->addItem(u"Major axis"_s, QStringLiteral("builtin:major"));
    this->comboMapSize->addItem(u"LLS"_s, QStringLiteral("builtin:lls"));
    this->comboMapSize->addItem(u"Flux"_s, QStringLiteral("builtin:flux"));
    for (const int index : this->numericRawFieldIndices) {
        this->comboMapSize->addItem(QStringLiteral("raw:%1").arg(this->rawHeaders.value(index)),
                                    QStringLiteral("raw:%1").arg(index));
    }
    int sizeIndex = this->comboMapSize->findData(this->mapSizeKey);
    this->comboMapSize->setCurrentIndex(sizeIndex >= 0 ? sizeIndex : 0);

    this->comboMapColor->clear();
    this->comboMapColor->addItem(u"Morphology color"_s, QStringLiteral("builtin:morphology"));
    for (const int index : this->numericRawFieldIndices) {
        this->comboMapColor->addItem(QStringLiteral("raw:%1").arg(this->rawHeaders.value(index)),
                                     QStringLiteral("raw:%1").arg(index));
    }
    int colorIndex = this->comboMapColor->findData(this->mapColorKey);
    this->comboMapColor->setCurrentIndex(colorIndex >= 0 ? colorIndex : 0);
    this->updatingMappingControls = false;
}

void vtkWindowCatalogue3D::updateDockSelectionDetails()
{
    if (!this->catalogueDetailsView) {
        return;
    }
    if (this->selectedIndex < 0 || this->selectedIndex >= static_cast<int>(this->entries.size())) {
        this->catalogueDetailsView->clear();
        qInfo() << "[catalogue3d] catalogue details view cleared";
        return;
    }
    const auto &entry = this->entries[static_cast<std::size_t>(this->selectedIndex)];
    QString details;
    details += QStringLiteral("Name: %1\n")
                       .arg(Catalogue3DParser::detail::entryDisplayName(entry, this->schema));
    details += QStringLiteral("RA: %1\n").arg(entry.raDeg, 0, 'f', 6);
    details += QStringLiteral("Dec: %1\n").arg(entry.decDeg, 0, 'f', 6);
    details += QStringLiteral("Distance: %1\n")
                       .arg(Catalogue3DParser::detail::entryDistanceMpc(entry, this->schema), 0, 'f', 3);
    details += QStringLiteral("Morphology: %1\n")
                       .arg(Catalogue3DParser::detail::entryMorphology(entry, this->schema));
    const QString rawDetails = this->fallbackDetailsText(entry);
    if (!rawDetails.isEmpty()) {
        details += QStringLiteral("\nRaw fields\n%1").arg(rawDetails);
    }
    this->catalogueDetailsView->setPlainText(details);
    qInfo() << "[catalogue3d] catalogue details view updated for selection:" << this->selectedIndex;
}

void vtkWindowCatalogue3D::syncTableSelection(int index)
{
    if (!this->catalogueTableView || !this->catalogueTableView->selectionModel() || !this->catalogueTableModel) {
        return;
    }
    this->syncingTableSelection = true;
    if (index < 0 || index >= this->catalogueTableModel->rowCount()) {
        this->catalogueTableView->clearSelection();
    } else {
        const QModelIndex modelIndex = this->catalogueTableModel->index(index, 0);
        this->catalogueTableView->selectionModel()->setCurrentIndex(
                modelIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        this->catalogueTableView->scrollTo(modelIndex, QAbstractItemView::PositionAtCenter);
    }
    this->syncingTableSelection = false;
}

void vtkWindowCatalogue3D::centerOnSource(int idx, double zoomFactor)
{
    if (idx < 0 || idx >= static_cast<int>(this->entries.size())) {
        return;
    }
    const auto &targetPosition = this->mappedPositions[static_cast<std::size_t>(idx)];
    auto *camera = this->renderer->GetActiveCamera();
    if (!camera) {
        return;
    }
    double focal[3];
    double cameraPosition[3];
    camera->GetFocalPoint(focal);
    camera->GetPosition(cameraPosition);
    const double dx = cameraPosition[0] - focal[0];
    const double dy = cameraPosition[1] - focal[1];
    const double dz = cameraPosition[2] - focal[2];
    camera->SetFocalPoint(targetPosition[0], targetPosition[1], targetPosition[2]);
    camera->SetPosition(targetPosition[0] + dx / zoomFactor,
                        targetPosition[1] + dy / zoomFactor,
                        targetPosition[2] + dz / zoomFactor);
    this->renderer->ResetCameraClippingRange();
    this->renderWindow->Render();
}

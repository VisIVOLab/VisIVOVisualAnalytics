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
#include <vtkGlyph3DMapper.h>
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
#include <QTimer>
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

// ── Data ──
// Extracted verbatim from vtkWindowCatalogue3D.cpp.

namespace {
// catalogueFormatForPath duplicated (also used by the constructor in main).
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
}  // namespace

void vtkWindowCatalogue3D::applyFrameToEntries()
{
    // Cartesian (.speck / XYZ) catalogues carry positions directly — there is no
    // RA/Dec to transform, so leave the parsed X/Y/Z scene positions untouched.
    if (this->schema.isCartesian()) {
        return;
    }
    if (this->activeFrame == CoordFrame::FK5_J2000) {
        // Restore FK5/J2000 Cartesian positions from the stored RA/Dec.
        // (Galactic mode overwrites sceneX/Y/Z in-place, so we must recompute
        // rather than just returning, otherwise switching back from Galactic
        // leaves the Galactic coordinates in the entries.)
        for (auto &e : this->entries)
            Catalogue3DParser::detail::fillCartesian(e, this->schema);
        return;
    }

    // Galactic (l, b) / Supergalactic (SGL, SGB): convert each source's
    // equatorial RA/Dec → galactic via wcscon(WCS_J2000 → WCS_GALACTIC); for the
    // supergalactic frame chain the de Vaucouleurs galactic→SG rotation. Then
    // project onto the same spherical-to-Cartesian formula so the scene scale
    // (Mpc from redshift/distance) is preserved. R-13.
    constexpr double pi = 3.14159265358979323846;
    const bool toSuper = (this->activeFrame == CoordFrame::Supergalactic);
    for (auto &e : this->entries) {
        double lon = e.raDeg;
        double lat = e.decDeg;
        wcscon(kWCS_J2000, kWCS_GALACTIC, 2000.0, 2000.0, &lon, &lat, 2000.0);
        if (toSuper)
            galacticToSupergalactic(lon, lat, lon, lat);
        const double d = Catalogue3DParser::detail::entryDistanceMpc(e, this->schema);
        const double lonRad = lon * pi / 180.0;
        const double latRad = lat * pi / 180.0;
        e.sceneX = d * std::cos(latRad) * std::cos(lonRad);
        e.sceneY = d * std::cos(latRad) * std::sin(lonRad);
        e.sceneZ = d * std::sin(latRad);
    }
}

void vtkWindowCatalogue3D::recomputeScenePositionsAndRebuild()
{
    // Recompute each source's Cartesian scene position from its (mode-aware) radial
    // distance, then rebuild the scene + labels from scratch. Shared by the local
    // Planck18 cosmology path and the R-14 distance-mode toggle.
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
    // buildScene() rebuilds the redshift shells hidden; reapply the user's toggle
    // so a checked "Show shells" survives a cosmology / distance-mode recompute.
    if (this->chkShowShells && this->chkShowShells->isChecked())
        this->toggleShells(true);
    // The table's Distance / X / Y / Z columns derive from the (now recomputed)
    // positions — refresh it so it doesn't show stale distances after the recompute.
    this->refreshCatalogueTable();
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::triggerCosmologyUpdate()
{
    // Cartesian (.speck / XYZ) catalogues have explicit positions — there is no
    // redshift→distance to recompute, and doing so would clobber the X/Y/Z scene.
    if (this->schema.isCartesian())
        return;
    if (this->entries.empty() || this->m_cosmologyBusy)
        return;

    // Planck18 matches the hardcoded local integration in Catalogue3DParser;
    // just clear overrides so entryDistanceMpc() falls back to local values.
    if (this->m_cosmologyModel == QStringLiteral("Planck18")) {
        for (auto &e : this->entries)
            e.distanceMpc = std::numeric_limits<double>::quiet_NaN();
        this->recomputeScenePositionsAndRebuild();
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

    // Collect per-entry redshifts using the SAME convention as the parser: a
    // catalogue z of 0 means "not measured" (see entryDistanceMpc), so only z > 0
    // goes out as a measurement. Everything else — absent, unparseable, zero,
    // negative, non-finite — is sent as −1, which the backend refuses and answers
    // null for, and that is what makes an unusable row identifiable on the way
    // back. Substituting 0, as this did, made the backend see a valid z = 0 and
    // return 0 Mpc, which would place an unmeasured source at the scene origin.
    QVector<double> redshifts;
    redshifts.reserve(static_cast<int>(this->entries.size()));
    for (const auto &e : this->entries) {
        bool ok = false;
        const double z = Catalogue3DParser::detail::fieldDouble(
                e, schema, Catalogue3DParser::detail::detectedRedshiftField(schema), &ok);
        redshifts.append((ok && std::isfinite(z) && z > 0.0) ? z : -1.0);
    }

    this->m_cosmologyWatcher.setFuture(QtConcurrent::run(
            [backendUrl, backendToken, model, redshifts]()
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
    if (this->m_sceneBuilding) {
        // A large buildScene() is pumping the event loop; this queued watcher
        // signal must NOT mutate entries / rebuild re-entrantly. Flag it and
        // apply once at the end of the build (result() stays valid).
        this->m_cosmologyFinishDeferred = true;
        return;
    }
    this->m_cosmologyFinishDeferred = false;
    this->m_cosmologyBusy = false;
    if (this->comboCosmology)
        this->comboCosmology->setEnabled(true);
    if (this->schema.isCartesian())   // positions are explicit X/Y/Z; nothing to recompute
        return;

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
    // The "no override" sentinel is NaN, not 0.0: this field holds a distance the
    // backend COMPUTED, and 0.0 is a legitimate output of that computation, so it
    // cannot double as "unset". (A raw catalogue z of 0 is a different question —
    // there zero does mean "not measured", and the parser treats it that way.)
    const int n = static_cast<int>(std::min(this->entries.size(), result.distancesMpc.size()));
    int refused = 0;
    for (int i = 0; i < n; ++i) {
        const double d = result.distancesMpc[static_cast<size_t>(i)];
        // NaN = the backend refused that redshift, i.e. this row has no usable one.
        // CLEAR the override rather than skipping: leaving the previous value would
        // keep a stale distance from an earlier model, when what this row should do
        // is fall back to its catalogue distance, or to the 300 Mpc default.
        if (!std::isfinite(d)) {
            ++refused;
            this->entries[static_cast<size_t>(i)].distanceMpc =
                    std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        this->entries[static_cast<size_t>(i)].distanceMpc = d;
    }
    // Recompute positions from the applied overrides and rebuild — the shared
    // helper also reapplies the shells toggle and refreshes the table, so a
    // backend cosmology change keeps them consistent (R-14 follow-up).
    this->recomputeScenePositionsAndRebuild();

    // One status line, written once. An earlier version set the refused-count
    // message here and then had it overwritten by this one two lines later.
    QString statusMsg = QStringLiteral("Cosmology: %1 – %2 sources.")
            .arg(result.model)
            .arg(this->entries.size());
    if (refused > 0) {
        statusMsg += u" %1 with no usable redshift kept their catalogue distance."_s
                             .arg(refused);
    }
    ui->labelStatus->setText(statusMsg);
    if (this->filterStatusLabel)
        this->filterStatusLabel->setText(statusMsg);
}

void vtkWindowCatalogue3D::applyFilter()
{
    if (this->remoteDatasetId.isEmpty() || this->filterBusy || this->m_sceneBuilding) {
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
    if (this->btnLoadAll) {
        this->btnLoadAll->setVisible(false);
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

void vtkWindowCatalogue3D::loadAllEntries()
{
    if (this->remoteDatasetId.isEmpty() || this->m_loadingMore || this->filterBusy || this->m_sceneBuilding)
        return;
    const int total = this->m_totalCount;
    if (total <= 0 || this->m_currentOffset >= total)
        return;
    if (total > 500000) {
        const auto answer = QMessageBox::question(
                this, u"Load all sources"_s,
                QStringLiteral("Load all %1 sources at once?\n\nThis can use significant memory "
                               "and may temporarily freeze the UI while the scene builds.")
                        .arg(total),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    // One page = everything. applyFilter() captures the page size synchronously,
    // so we can restore it immediately for any later paginated query.
    const int savedPageSize = this->m_pageSize;
    this->m_pageSize = total;
    this->applyFilter();
    this->m_pageSize = savedPageSize;
}

void vtkWindowCatalogue3D::loadMoreEntries()
{
    if (this->remoteDatasetId.isEmpty() || this->m_loadingMore || this->filterBusy || this->m_sceneBuilding) {
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
    const QStringList paths = this->m_cataloguePaths;   // preserve a multi-file group

    this->m_reopenWatcher.setFuture(QtConcurrent::run(
            [url, token, filePath, paths]() -> BackendCatalogueInfo {
                BackendClient client(url, token);
                const QString fmt = catalogueFormatForPath(filePath);
                return paths.size() > 1 ? client.openCatalogue(paths, fmt)
                                        : client.openCatalogue(filePath, fmt);
            }));
}

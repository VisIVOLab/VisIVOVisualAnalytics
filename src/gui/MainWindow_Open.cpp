#include "MainWindow.h"

#include "ConeSearchWindow.h"

#include "Hips2FitsDialog.h"
#include "ui_MainWindow.h"

#include "AboutDialog.h"
#include "AuthWrapper.h"
#include "app/BackendClient.h"
#include "app/BackendClientFactory.h"
#include "app/SampClient.h"
#include "app/SampMessageHandler.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "BaselineDialog.h"
#include "CommandPalette.h"
#include "DataHubWidget.h"
#include "DiagnosticsWindow.h"
#include "LinewidthDialog.h"
#include "ArepoExtractDialog.h"
#include "RemoteFileBrowserDialog.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "theme/VisivoTheme.h"
#include "SourceFindDialog.h"
#include "StackDialog.h"
#include "WebViewProcess.h"
#include "vtkWindowCatalogue3D.h"
#include "vtkWindowCube.h"
#include "vtkWindowImage.h"
#include "vtkWindowVectorField.h"

#include <QLabel>
#include <QStatusBar>
#include "HiPSWindow.h"
#include "vtkWindowVbt.h"
#include "vtkWindowVbtVolume.h"
#include "VLKBInventoryTree.h"
#include "SkavaSearchPanel.h"
#include "../skava/SkavaClient.h"
#include "../skava/BackendRouting.h"
#include "../Settings.h"
#include "vlkb/VlkbQueryService.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QWebChannel>
#include <QPixmap>
#include <QStyle>
#include <QPainter>
#include <QUrl>
#include <QtConcurrent>

#include <memory>

#include <cmath>

#include "app/DiagnosticsManager.h"

#include <vtkImageData.h>

using namespace Qt::StringLiterals;

// ── MainWindow: Open (extracted from MainWindow.cpp) ──

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

struct VbtOpenTaskResult
{
    bool valid{ false };
    QString error;
    BackendVbtOpenResult openResult;
    BackendVbtSubsetResult subsetResult;
    QString backendUrl;
    QString backendToken;
    QString sessionId;
};

static VbtOpenTaskResult runVbtOpenTask(const QString &headerPath,
                                        const QString &backendUrl,
                                        const QString &backendToken,
                                        int maxPointRows)
{
    VbtOpenTaskResult out;
    out.backendUrl = backendUrl;
    out.backendToken = backendToken;

    BackendClient client(backendUrl, backendToken);

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
            QStringLiteral("Opening VBT: %1").arg(headerPath), QString(), QString(),
            QStringLiteral("open"));

    out.openResult = client.openVbt(headerPath);
    if (!out.openResult.valid) {
        out.error = out.openResult.error.isEmpty()
                ? QStringLiteral("The backend could not open this VBT dataset.")
                : out.openResult.error;
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                QStringLiteral("Open failed: %1").arg(out.error), QString(), QString(),
                QStringLiteral("open"));
        return out;
    }

    out.sessionId = client.sessionId();

    if (out.openResult.kind == QStringLiteral("volume")) {
        const QString firstField = out.openResult.fields.empty()
                ? QString()
                : out.openResult.fields[0].name;
        out.subsetResult = client.requestVbtSubset(out.openResult.datasetId, 0, firstField);
        if (!out.subsetResult.valid) {
            out.error = out.subsetResult.error.isEmpty()
                    ? QStringLiteral("Backend returned no volume data.")
                    : out.subsetResult.error;
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                    QStringLiteral("Volume subset failed: %1").arg(out.error),
                    out.openResult.datasetId, out.sessionId, QStringLiteral("open"));
            return out;
        }
    } else {
        // Capped, not "everything": a point table is returned as one float64
        // column per field, so asking for all rows of a large table costs more
        // than the service has. Without a cap a 21-million-row file killed it,
        // and the client reported "connection refused".
        out.subsetResult = client.requestVbtSubset(out.openResult.datasetId, maxPointRows);
        if (!out.subsetResult.valid) {
            out.error = out.subsetResult.error.isEmpty()
                    ? QStringLiteral("Backend returned no table data.")
                    : out.subsetResult.error;
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                    QStringLiteral("Table subset failed: %1").arg(out.error),
                    out.openResult.datasetId, out.sessionId, QStringLiteral("open"));
            return out;
        }
    }

    // A truncated table must SAY it is truncated: showing two million of
    // twenty-one million particles as if they were the dataset is a quiet way
    // to draw a wrong conclusion.
    const bool truncated = out.openResult.kind != QStringLiteral("volume")
            && out.subsetResult.numRows > 0
            && out.subsetResult.numRows < out.openResult.numRows;
    if (truncated) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                QStringLiteral("Showing %1 of %2 rows — the rest is not loaded. "
                               "Raise Settings ▸ Visualization ▸ Max glyphs to see more.")
                        .arg(out.subsetResult.numRows)
                        .arg(out.openResult.numRows),
                out.openResult.datasetId, out.sessionId, QStringLiteral("open"));
    }
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
            QStringLiteral("Opened %1 (%2 rows, %3 fields).")
                    .arg(out.openResult.kind)
                    .arg(out.openResult.numRows)
                    .arg(out.openResult.fields.size()),
            out.openResult.datasetId, out.sessionId, QStringLiteral("open"));

    out.valid = true;
    return out;
}

}  // namespace

void MainWindow::reopenDataset(const RecentDataset &dataset)
{
    // ── SKAVA-origin entries route through discovery on reopen ──
    // Re-querying SKAVA (rather than reusing the stale local path) lets
    // the operator's file move across nodes, or the visivo-backend at a
    // node be replaced, without breaking the Recent Datasets entry.
    if (dataset.source == u"skava"_s && !dataset.obsId.isEmpty()) {
        SkavaClient skava(this->settings->getSkavaUrl(),
                           this->settings->getSkavaToken(), this);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const auto datalink = skava.getDatalink(dataset.obsId);
        QApplication::restoreOverrideCursor();
        if (!datalink.valid) {
            QMessageBox::warning(
                    this, u"Reopen from SKAVA"_s,
                    u"Could not reopen '%1':\n\nSKAVA query failed: %2"_s
                            .arg(dataset.displayName,
                                  datalink.error.isEmpty()
                                          ? u"unknown error"_s : datalink.error));
            return;
        }
        doOpenSkavaDataset(dataset.obsId, datalink);
        return;
    }

    if (dataset.type == u"cube"_s || dataset.type == u"image"_s) {
        doOpenDataPath(dataset.id);
    } else if (dataset.type == u"catalogue"_s) {
        doOpenCataloguePath(dataset.id);
    } else if (dataset.type == u"vbt"_s || dataset.type == u"vbt-volume"_s) {
        doOpenVbtPath(dataset.id);
    } else if (dataset.type == u"hips"_s) {
        openHiPS();
    } else {
        QMessageBox::information(
                this, u"Cannot reopen"_s,
                QStringLiteral("Reopening '%1' (type: %2) is not yet supported.")
                        .arg(dataset.displayName, dataset.type));
    }
}


void MainWindow::openData()
{
    // All datasets are opened via the backend. The backend may be a localhost
    // instance or a remote server — that distinction is a deployment detail,
    // not a client-side mode. There is no "local FITS open" path.
    //
    // Reachable only from the Data Hub's old action grid, which nothing builds
    // any more (the quick-actions list offers one "Open…" instead). It is kept
    // wired rather than half-deleted, so it has to behave like the others: its
    // browser has no extension filter, and since any file can be picked there,
    // the selection goes through the classifier rather than straight to the
    // FITS/HDF5 route.
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a dataset: the backend is not reachable.\n\n"
                u"Start a local backend or configure a remote backend URL in Settings, "
                u"then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString path = dialog.selectedFilePath();
    if (path.isEmpty()) {
        return;
    }

    dispatchOpenForPath(path);
}


void MainWindow::doOpenSkavaDataset(const QString &obsId,
                                      const SkavaDatalinkResponse &datalink)
{
    // Backend-aware routing per docs/distributed-backend-design.md §4.2.
    const BackendNode chosen = pickBackendForSkavaDataset(this->settings, datalink);
    // ── Routing decision ────────────────────────────────────────────
    // pickBackendForSkavaDataset returns:
    //   * a remote BackendNode → compute-next-to-data via that node
    //   * the local BackendNode → still use compute-next-to-data, just
    //     against the local backend (no special-case needed — the
    //     desktop's "local" backend supports /v1/datasets/open_skava
    //     same as any remote one)
    //   * nothing → no SKAVA visivo-backend descriptor matched; we
    //     fall through to the download-via-local path below, which
    //     handles HTTP-served archives.
    // Strip ``file://`` from the access URL so downstream viewer code can
    // both call backend endpoints that expect a real path (fileHeader,
    // moment exports, etc.) and render a friendly window title via
    // QFileInfo(path).fileName(). Returns an empty string when the URL
    // isn't a file:// one — callers fall back to obs_id in that case.
    auto pathFromFileUrl = [](const QString &url) -> QString {
        if (!url.startsWith(QStringLiteral("file://"))) {
            return QString();
        }
        QString tail = url.mid(7);  // strip "file://"
        // Allow file://localhost/path → /path
        if (tail.startsWith(QStringLiteral("localhost/"))) {
            tail = tail.mid(9);  // strip "localhost"
        }
        return QUrl::fromPercentEncoding(tail.toUtf8());
    };
    const QString fsPath = pathFromFileUrl(datalink.primaryAccessUrl);

    if (!chosen.id.isEmpty()) {
        this->setActiveBackendBadge(chosen);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        BackendClient client(chosen.url, chosen.token);
        const auto opened = client.openSkavaDataset(obsId, datalink.primaryAccessUrl);
        QApplication::restoreOverrideCursor();
        if (!opened.valid) {
            qWarning().noquote() << QStringLiteral(
                    "[skava] remote backend %1 refused open: %2, falling back to local download")
                    .arg(chosen.id, opened.error);
            // Revert the badge: the remote refused, so the imminent
            // download/open below is served by the local backend.
            this->setActiveBackendBadge(this->settings->getLocalBackend());
            // Fall through to the local-download path so the user still
            // gets the dataset rather than a hard failure.
        } else {
            // Prefer the real filesystem path so window title shows the
            // FITS basename and "View FITS header" works without round-
            // tripping the backend through an obs_id string.
            const QString displayPath = !fsPath.isEmpty()
                    ? fsPath
                    : (obsId.isEmpty() ? datalink.primaryAccessUrl : obsId);
            openViewerFromOpenedDataset(displayPath, opened, client);
            // Re-record the just-opened dataset as a SKAVA entry: the
            // generic ``openViewerFromOpenedDataset`` recorded it under
            // its path with source=remote, but we want source=skava so
            // the DataHub badge fires and the next reopen routes through
            // the discovery flow instead of a dead local path.
            if (this->recentMgr) {
                this->recentMgr->remove(displayPath);
                const QString name = !fsPath.isEmpty()
                        ? QFileInfo(fsPath).fileName() : displayPath;
                this->recentMgr->record(
                        QStringLiteral("skava:") + obsId,
                        name, opened.kind, u"skava"_s, obsId);
            }
            return;
        }
    }
    // ── Fallback: download via local backend cache → open ──────────────
    doOpenDataFromUrl(datalink.primaryAccessUrl, obsId);
}


void MainWindow::doSkavaSodaSubset(const QString &obsId,
                                     const SkavaDatalinkResponse &datalink)
{
    // The cutout must run where the data lives: pick the same co-located
    // backend that would open the dataset. A SODA subset only makes sense
    // when that backend exists AND serves a file:// replica it can open in
    // place — otherwise there is nothing to compute next to.
    const BackendNode chosen = pickBackendForSkavaDataset(this->settings, datalink);
    const bool isFileUrl =
            datalink.primaryAccessUrl.startsWith(QStringLiteral("file://"));
    if (chosen.id.isEmpty() || !isFileUrl) {
        QMessageBox::information(
                this, u"SODA subset unavailable"_s,
                u"This dataset has no co-located VisIVO backend serving a "
                u"file:// replica, so a server-side SODA cutout cannot run.\n\n"
                u"Use “Open selected in VisIVO” to download and open the full "
                u"dataset instead."_s);
        return;
    }

    // ── Region dialog (POS + BAND), prefilled from the dataset metadata ──
    const SkavaDatasetMetadata &m = datalink.dataset;
    QDialog dlg(this);
    dlg.setWindowTitle(u"SODA subset - %1"_s.arg(obsId));
    auto *form = new QFormLayout(&dlg);
    auto mkEdit = [&](double value, const QString &placeholder) {
        auto *e = new QLineEdit(&dlg);
        e->setPlaceholderText(placeholder);
        e->setValidator(new QDoubleValidator(&dlg));
        if (std::isfinite(value)) e->setText(QString::number(value, 'g', 10));
        return e;
    };
    auto *raEdit   = mkEdit(m.sRa,  u"RA (deg)"_s);
    auto *decEdit  = mkEdit(m.sDec, u"Dec (deg)"_s);
    auto *radEdit  = mkEdit(std::isfinite(m.sFov) ? m.sFov * 0.5 : 0.1,
                            u"radius (deg)"_s);
    auto *bMinEdit = mkEdit(m.emMin, u"min (m)"_s);
    auto *bMaxEdit = mkEdit(m.emMax, u"max (m)"_s);
    form->addRow(u"POS, RA (deg)"_s, raEdit);
    form->addRow(u"Dec (deg)"_s, decEdit);
    form->addRow(u"Radius (deg)"_s, radEdit);
    form->addRow(u"BAND, min (m)"_s, bMinEdit);
    form->addRow(u"BAND, max (m)"_s, bMaxEdit);
    auto *hint = new QLabel(
            u"Leave all POS fields or both BAND fields blank to omit that "
            u"axis. At least one of POS / BAND is required. The cutout runs on "
            u"node %1."_s.arg(chosen.name.isEmpty() ? chosen.id : chosen.name),
            &dlg);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Run cutout"_s);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    // ── Build the SODA POS / BAND strings ────────────────────────────────
    auto readNum = [](QLineEdit *e, double &out) {
        const QString t = e->text().trimmed();
        if (t.isEmpty()) return false;
        bool ok = false;
        out = t.toDouble(&ok);
        return ok;
    };
    double ra = 0, dec = 0, rad = 0, bMin = 0, bMax = 0;
    QString pos, band;
    if (readNum(raEdit, ra) && readNum(decEdit, dec) && readNum(radEdit, rad)) {
        pos = u"CIRCLE %1 %2 %3"_s.arg(QString::number(ra, 'g', 10),
                                        QString::number(dec, 'g', 10),
                                        QString::number(rad, 'g', 10));
    }
    if (readNum(bMinEdit, bMin) && readNum(bMaxEdit, bMax)) {
        band = u"%1 %2"_s.arg(QString::number(std::min(bMin, bMax), 'g', 10),
                               QString::number(std::max(bMin, bMax), 'g', 10));
    }
    if (pos.isEmpty() && band.isEmpty()) {
        QMessageBox::information(
                this, u"SODA subset"_s,
                u"Provide a POS (RA/Dec/radius) and/or a BAND (min/max) to "
                u"subset."_s);
        return;
    }

    // ── Run the cutout on the chosen backend, then open it in place ──────
    this->setActiveBackendBadge(chosen);
    const QString accessUrl = datalink.primaryAccessUrl;
    const QString url = chosen.url;
    const QString token = chosen.token;

    auto *watcher = new QFutureWatcher<BackendSodaCutoutResult>(this);
    QObject::connect(
            watcher, &QFutureWatcher<BackendSodaCutoutResult>::finished, this,
            [this, watcher, url, token, obsId]() {
                QApplication::restoreOverrideCursor();
                watcher->deleteLater();
                const auto res = watcher->result();
                if (!res.valid) {
                    QMessageBox::critical(
                            this, u"SODA subset failed"_s,
                            res.error.isEmpty() ? u"The backend returned no detail."_s
                                                : res.error);
                    return;
                }
                // The cutout lives on the backend filesystem — open it there,
                // never downloading it to the desktop (compute next to data).
                BackendClient client(url, token);
                const auto opened = client.openDataset(res.path);
                if (!opened.valid) {
                    QMessageBox::critical(
                            this, u"Could not open SODA cutout"_s,
                            opened.error.isEmpty()
                                    ? u"Backend returned no error detail."_s
                                    : opened.error);
                    return;
                }
                openViewerFromOpenedDataset(res.path, opened, client);
                if (this->recentMgr) {
                    this->recentMgr->record(res.path,
                                            u"%1 [SODA cutout]"_s.arg(obsId),
                                            opened.kind);
                }
            });

    QApplication::setOverrideCursor(Qt::WaitCursor);
    watcher->setFuture(QtConcurrent::run(
            [url, token, obsId, accessUrl, pos, band]() {
                BackendClient bc(url, token);
                return bc.sodaCutout(obsId, accessUrl, pos, band, QString());
            }));
}


void MainWindow::openBackendProductFits(const QString &backendUrl, const QString &backendToken,
                                          const QString &path, const QString &displayName)
{
    BackendClient client(backendUrl, backendToken);
    const auto opened = client.openDataset(path);
    if (!opened.valid) {
        QMessageBox::critical(
                this, u"Could not open product"_s,
                opened.error.isEmpty() ? u"Backend returned no error detail."_s : opened.error);
        return;
    }
    openViewerFromOpenedDataset(path, opened, client);
    if (this->recentMgr) this->recentMgr->record(path, displayName, opened.kind);
}

void MainWindow::openSourceFinder(const QString &backendUrl, const QString &backendToken,
                                  const QString &sessionId, const QString &datasetId)
{
    if (datasetId.isEmpty()) {
        QMessageBox::information(this, u"Source Finding"_s,
                u"Open a spectral cube first."_s);
        return;
    }
    // Shared ownership: the dialog's async SoFiA run captures a copy of this
    // shared_ptr, so the client survives an early dialog close (no use-after-free)
    // and is freed when the last holder — dialog or in-flight worker — releases it.
    auto client = std::make_shared<BackendClient>(backendUrl, backendToken);
    client->setSessionId(sessionId);   // resolve the dataset in the right session
    auto *dlg = new SourceFindDialog(client, datasetId, this);
    // Open the SoFiA-2 mask cube in the 3D viewer (mask → 3D inspection of the
    // detections) on the same node backend that ran the source finder.
    QObject::connect(dlg, &SourceFindDialog::openMaskCubeRequested, this,
                     [this, backendUrl, backendToken](const QString &maskPath) {
                         this->openBackendProductFits(backendUrl, backendToken, maskPath,
                                                      u"SoFiA mask cube"_s);
                     });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();   // non-modal — consistent with the other tool dialogs
    dlg->raise();
    dlg->activateWindow();
}


void MainWindow::doOpenDataFromUrl(const QString &url, const QString &obsId)
{
    if (url.trimmed().isEmpty()) {
        QMessageBox::warning(this, u"SKAVA"_s,
                u"No access URL was returned by SKAVA for this dataset."_s);
        return;
    }
    this->setActiveBackendBadge(this->settings->getLocalBackend());
    BackendClient client(this->settings->getBackendUrl(),
                          this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(this, u"Backend unavailable"_s,
                u"Cannot download the SKAVA dataset: the VisIVO backend is "
                u"not reachable.\n\nError: %1"_s.arg(
                        health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto opened = client.openDatasetFromUrl(url, obsId);
    QApplication::restoreOverrideCursor();
    if (!opened.valid) {
        QMessageBox::critical(this, u"Open from SKAVA"_s,
                u"Could not open the dataset:\n\n%1"_s.arg(
                        opened.error.isEmpty() ? u"unknown error"_s : opened.error));
        return;
    }
    // The viewer wants a displayable path — for SKAVA-originated opens we
    // use the obs_id so the window title, Recent Datasets entry, SAMP
    // metadata etc. carry meaningful labels. The actual data is pulled
    // by the viewer via opened.datasetId (already session-registered).
    const QString displayPath = obsId.isEmpty() ? url : obsId;
    openViewerFromOpenedDataset(displayPath, opened, client);
}


void MainWindow::openDatasetPath(const QString &path)
{
    // Entry point for the desktop (Finder / file manager / command line). The
    // backend opens the file by PATH, so it has to be a readable local file;
    // saying so here is better than the backend's generic "could not open".
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        QMessageBox::warning(this, u"Cannot open file"_s,
                             u"“%1” is not a readable file."_s.arg(path));
        return;
    }
    this->show();
    this->raise();
    this->activateWindow();
    // Through the CLASSIFIER, exactly like File ▸ Open…. This entry point used
    // to call doOpenDataPath() directly, which is the FITS/HDF5 route: a
    // double-clicked IPAC table, .speck, CSV, VOTable or VisIVO Binary Table was
    // answered with "Unsupported file type. Expected FITS (.fits, .fit) or HDF5"
    // although the application opens every one of them from its own Open dialog.
    // Whatever the desktop hands over goes through the same single decision.
    this->dispatchOpenForPath(info.absoluteFilePath());
}

void MainWindow::doOpenDataPath(const QString &path)
{
    this->setActiveBackendBadge(this->settings->getLocalBackend());
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a dataset: the backend is not reachable.\n\n"
                u"Error: %1"_s.arg(health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }

    const auto opened = client.openDataset(path);
    if (!opened.valid) {
        QMessageBox::critical(
                this, u"Could not open dataset"_s,
                u"The backend could not open this dataset.\n\n%1\n\n"
                u"Supported formats: FITS (image, cube). For HDF5, Gadget, VOTable, "
                u"or simulation data, convert with VisIVOServer first."_s.arg(
                        opened.error.isEmpty() ? u"Backend returned no error detail."_s
                                               : opened.error));
        return;
    }
    openViewerFromOpenedDataset(path, opened, client);
}


void MainWindow::wireImageViewerProducts(vtkWindowImage *window,
                                         const QString &backendUrl,
                                         const QString &backendToken,
                                         const QString &sessionId,
                                         const QString &datasetId)
{
    // The single-map tools that used to live in the Data Hub's Science menu.
    // Each viewer's own context is captured here, so they act on THAT image
    // rather than on whichever dataset was opened last.
    //
    // Every path that constructs a vtkWindowImage must come through here.
    // Wiring it inline in the generic open path left the VLKB inventory's own
    // window (openVlkbImageLayer) with enabled actions whose signals nobody
    // answered, so the tools did nothing at all.
    if (!window)
        return;
    // Each result goes back to the window that asked for it, which records it in
    // its own Session Data. Without that the figure existed only in a message
    // box and the metrics only in a dialog, in a window whose tree stayed empty.
    QPointer<vtkWindowImage> owner(window);
    QObject::connect(window, &vtkWindowImage::publicationFigureRequested, this,
                     [this, owner, backendUrl, backendToken, sessionId, datasetId]() {
                         this->runPublicationFigure(
                                 backendUrl, backendToken, sessionId, datasetId,
                                 [owner](const QString &outputPath) {
                                     if (owner)
                                         owner->recordPublicationFigure(outputPath);
                                 });
                     });
    // "Open this file on its own", which is the one thing a viewer cannot do for
    // itself. What the file is decides which viewer receives it: a layer that
    // turned out to be somewhere else is an image, a catalogue query's result is
    // a table, and dispatchOpenForPath knows the difference.
    QObject::connect(window, &vtkWindowImage::openInOwnWindowRequested, this,
                     [this](const QString &path) { this->dispatchOpenForPath(path); });
    QObject::connect(window, &vtkWindowImage::imageQualityRequested, this,
                     [this, owner, backendUrl, backendToken, sessionId, datasetId]() {
                         this->runImageQuality(
                                 backendUrl, backendToken, sessionId, datasetId,
                                 [owner](const QVariantMap &metrics) {
                                     if (owner)
                                         owner->recordImageQuality(metrics);
                                 });
                     });
}

void MainWindow::adoptSessionContext(const QString &backendUrl, const QString &backendToken,
                                     const QString &sessionId, const QString &datasetId,
                                     int depth, const QString &spectralAxisType,
                                     const QString &spectralAxisUnit)
{
    // The Data Hub keeps a SESSION, not a "current dataset": its Combine
    // entries pick their inputs from the session list. The per-dataset tools
    // that used to read these members live in the viewers now and receive the
    // context of the window that asks.
    m_sciBackendUrl        = backendUrl;
    m_sciSessionId         = sessionId;
    m_sciToken             = backendToken;
    m_sciDatasetId         = datasetId;
    m_sciDepth             = depth > 0 ? depth : 1;
    m_sciSpectralAxisType  = spectralAxisType;
    m_sciSpectralAxisUnit  = spectralAxisUnit;
    this->refreshCombineMenuState();
}

void MainWindow::openViewerFromOpenedDataset(const QString &path,
                                              const BackendOpenDatasetResult &opened,
                                              const BackendClient &client)
{
    QWidget *win = nullptr;
    if (opened.kind == u"image"_s || opened.kind == u"dynspec"_s) {
        const bool isDynspec = opened.kind == u"dynspec"_s;
        qDebug().noquote()
                << QStringLiteral("[fits] active_axes=%1 -> opening as %2 | %3")
                           .arg(opened.activeAxes)
                           .arg(isDynspec ? QStringLiteral("dynamic spectrum")
                                          : QStringLiteral("2D image"))
                           .arg(opened.degenerateAxesSummary.isEmpty()
                                        ? QStringLiteral("no collapsed axes")
                                        : opened.degenerateAxesSummary);
        auto *imgWin = new vtkWindowImage(path, client.baseUrl(), opened.datasetId,
                                 opened.ctype, opened.cunit, opened.crval, opened.crpix,
                                 opened.cdelt, opened.bunit, opened.degenerateAxesSummary,
                                 opened.wcsStatus, opened.wcsWarningMessage,
                                 client.sessionId(), client.token(), this);
        if (!isDynspec) {
            imgWin->setBeamInfo(opened.beamMajorDeg, opened.beamMinorDeg,
                                opened.beamPaDeg);
        }
        imgWin->setDynamicSpectrumMode(isDynspec);
        this->wireImageViewerProducts(imgWin, client.baseUrl(), client.token(),
                                      client.sessionId(), opened.datasetId);
        win = imgWin;
    } else if (opened.kind == u"cube"_s) {
        qDebug().noquote()
                << QStringLiteral("[fits] active_axes=%1 -> opening as cube | %2")
                           .arg(opened.activeAxes)
                           .arg(opened.degenerateAxesSummary.isEmpty()
                                        ? QStringLiteral("no collapsed axes")
                                        : opened.degenerateAxesSummary);
        auto *cubeWin = new vtkWindowCube(path, client.baseUrl(), opened.datasetId, opened.width,
                                          opened.height, opened.depth, opened.spacing,
                                          opened.origin, opened.ctype, opened.cunit, opened.crval,
                                          opened.crpix, opened.cdelt, opened.degenerateAxesSummary,
                                          opened.spectralAxisType, opened.spectralAxisUnit,
                                          opened.wcsStatus, opened.wcsWarningMessage,
                                          client.sessionId(), client.token(),
                                          // Pattern B: propagate the backend's
                                          // size-aware loading policy so the
                                          // viewer can skip the auto full-res
                                          // fetch for cubes above the threshold.
                                          opened.recommendedLoadMode,
                                          opened.fileSizeBytes,
                                          opened.fullResThresholdBytes,
                                          this);
        cubeWin->setDarkPlots(this->settings->getPlotTheme() == 0);
        cubeWin->setBeamInfo(opened.beamMajorDeg, opened.beamMinorDeg,
                             opened.beamPaDeg);
        cubeWin->setBrightnessUnit(opened.bunit);
        cubeWin->setRestFrequency(opened.restFreqHz);
        cubeWin->setServerSideRenderingAvailable(opened.serverSideRenderingAvailable);
        // Live refresh: when this cube viewer pushes a new FITS into the
        // Workspace Exports, the Data Hub panel updates within frames of
        // the operation instead of on the next backend health tick.
        if (this->dataHub) {
            QObject::connect(cubeWin, &vtkWindowCube::workspaceExportsChanged,
                             this->dataHub, &DataHubWidget::refreshWorkspaceExports);
        }
        // Copilot "show sources" → open the SoFiA-2 dialog for THIS cube's
        // dataset/backend and overlay the mask in 3D (reuses the interactive
        // source-find path). Capture this cube's own url/token/datasetId so it
        // targets the right dataset even with several cubes open.
        {
            const QString sfUrl = client.baseUrl(), sfToken = client.token(),
                          sfSession = client.sessionId(), sfDataset = opened.datasetId;
            QObject::connect(cubeWin, &vtkWindowCube::sourceFinderRequested, this,
                             [this, sfUrl, sfToken, sfSession, sfDataset]() {
                                 this->openSourceFinder(sfUrl, sfToken, sfSession, sfDataset);
                             });
        }
        // A product stored as a backend FITS (baseline-subtracted cube, mask,
        // sub-cube) reopened from its Session Data row.
        {
            const QString pUrl = client.baseUrl(), pToken = client.token();
            QObject::connect(cubeWin, &vtkWindowCube::openBackendFitsRequested, this,
                             [this, pUrl, pToken](const QString &path, const QString &label) {
                                 this->openBackendProductFits(pUrl, pToken, path, label);
                             });
        }
        // Cube → Image contour relay: when the cube viewer emits
        // contourDataReady, find the first open image viewer and forward.
        QObject::connect(cubeWin, &vtkWindowCube::contourDataReady,
                         this, [](const QString &label, vtkImageData *data) {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *img = qobject_cast<vtkWindowImage *>(w)) {
                    img->overlayExternalContourData(label, data);
                    return;
                }
            }
        });
        win = cubeWin;
    } else {
        QMessageBox::information(
                this, u"Unsupported dataset"_s,
                u"The backend reported an unsupported dataset kind: '%1'.\n\n"
                u"Only FITS images and spectral cubes are supported."_s.arg(opened.kind));
        return;
    }

    this->m_sampCurrentFitsPath = path;
    this->m_sampCurrentFitsName = QFileInfo(path).fileName();
    this->m_sampCurrentCataloguePath.clear();
    this->m_sampCurrentCatalogueName.clear();
    this->m_sampCurrentCatalogueFormat.clear();
    this->updateSampActions();

    // Record in recent datasets (uses the final kind returned by the backend).
    if (this->recentMgr) {
        const QString name = QFileInfo(path).fileName();
        this->recentMgr->record(path, name, opened.kind);
    }

    this->adoptSessionContext(client.baseUrl(), client.token(), client.sessionId(),
                              opened.datasetId, opened.depth,
                              opened.spectralAxisType, opened.spectralAxisUnit);

    win->showMaximized();
    win->raise();
    win->activateWindow();
}


void MainWindow::openVlkbImageLayer(const QString &backendPath, const QString &displayName,
                                     QPointer<VLKBInventoryTree> tree)
{
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open VLKB dataset: the backend is not reachable.\n\nError: %1"_s.arg(
                        health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }

    const auto opened = client.openDataset(backendPath);
    if (!opened.valid) {
        QMessageBox::critical(
                this, u"Could not open VLKB dataset"_s,
                (opened.error.isEmpty() ? u"Backend returned no error detail."_s : opened.error));
        return;
    }

    if (opened.kind != u"image"_s) {
        // Not a 2D image — fall back to the generic open so cubes land in the right viewer.
        doOpenDataPath(backendPath);
        return;
    }

    auto *win = new vtkWindowImage(backendPath, client.baseUrl(), opened.datasetId,
                                    opened.ctype, opened.cunit, opened.crval, opened.crpix,
                                    opened.cdelt, opened.bunit, opened.degenerateAxesSummary,
                                    opened.wcsStatus, opened.wcsWarningMessage,
                                    client.sessionId(), client.token(), this);
    this->wireImageViewerProducts(win, client.baseUrl(), client.token(),
                                  client.sessionId(), opened.datasetId);
    // …and adopt the session, so the Data Hub's Combine entries see the datasets
    // this path opened. Only the generic path did that, so a session made up of
    // VLKB images alone left Combine disabled with datasets sitting in it.
    this->adoptSessionContext(client.baseUrl(), client.token(), client.sessionId(),
                              opened.datasetId, opened.depth,
                              opened.spectralAxisType, opened.spectralAxisUnit);

    // Override the master layer name with the human-readable inventory label.
    win->setLayerDisplayName(0, displayName);

    // Associate this window with the inventory tree so subsequent 2D items from
    // the same session are stacked as additional layers.
    if (tree) tree->setLinkedImageWindow(win);

    this->m_sampCurrentFitsPath = backendPath;
    this->m_sampCurrentFitsName = displayName;
    this->m_sampCurrentCataloguePath.clear();
    this->m_sampCurrentCatalogueName.clear();
    this->m_sampCurrentCatalogueFormat.clear();
    this->updateSampActions();

    // Record in recent datasets using the display name, not the uuid path.
    if (this->recentMgr)
        this->recentMgr->record(backendPath, displayName, u"image"_s);

    win->showMaximized();
    win->raise();
    win->activateWindow();
}


/// Offer to load every same-extension sibling in the folder as ONE combined
/// catalogue — the spatial subtables of a cosmological snapshot arrive that
/// way. Returns the group, or an empty list to open just the selected file.
static QStringList catalogueSiblingGroup(const BackendClient &client, const QString &filepath,
                                         QWidget *parent)
{
    const QFileInfo info(filepath);
    const QString suffix = info.suffix().toLower();
    QStringList group;
    const auto listing = client.listFiles(info.path());
    if (listing.valid) {
        for (const auto &entry : listing.entries) {
            if (entry.type == u"file"_s && !entry.path.isEmpty()
                && QFileInfo(entry.name).suffix().toLower() == suffix)
                group << entry.path;
        }
    }
    if (group.size() < 2)
        return {};
    std::sort(group.begin(), group.end());
    const auto answer = QMessageBox::question(
            parent, u"Multiple catalogue files"_s,
            u"Found %1 .%2 files in this folder.\n\nLoad ALL of them as one "
            u"combined catalogue? (No = open just the selected file.)"_s
                    .arg(group.size())
                    .arg(suffix),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer == QMessageBox::Yes ? group : QStringList();
}


void MainWindow::openCatalogue3D()
{
    BackendClient client(settings->getBackendUrl(), settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a catalogue: the backend is not reachable.\n\n"
                u"Start a local backend or configure a remote backend URL in Settings, "
                u"then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    dialog.setExtensionFilter({u".csv"_s, u".vot"_s, u".votable"_s, u".xml"_s,
                               u".fits"_s, u".fit"_s, u".speck"_s, u".tbl"_s},
                              u"Catalogue tables"_s);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString filepath = dialog.selectedFilePath();
    if (filepath.isEmpty())
        return;

    doOpenCataloguePath(filepath, catalogueSiblingGroup(client, filepath, this));
}


void MainWindow::doOpenCataloguePath(const QString &filepath, const QStringList &paths)
{
    this->m_sampCurrentFitsPath.clear();
    this->m_sampCurrentFitsName.clear();
    this->m_sampCurrentCataloguePath = filepath;
    this->m_sampCurrentCatalogueName = QFileInfo(filepath).fileName();
    this->m_sampCurrentCatalogueFormat = catalogueFormatForPath(filepath);
    this->updateSampActions();
    auto *win = new vtkWindowCatalogue3D(filepath, settings->getBackendUrl(),
                                         settings->getBackendToken(), paths, this);
    win->setAttribute(Qt::WA_DeleteOnClose);
    // On session expiry the window emits reopenRequested with its current path
    // group (single or multi-file), so the reopened window rebuilds the same
    // combined catalogue.
    QObject::connect(win, &vtkWindowCatalogue3D::reopenRequested, this,
                     [this](const QString &p, const QStringList &ps) {
                         this->doOpenCataloguePath(p, ps);
                     });
    win->showMaximized();
    win->raise();
    win->activateWindow();

    if (this->recentMgr) {
        this->recentMgr->record(filepath, QFileInfo(filepath).fileName(), u"catalogue"_s);
    }
}


// ── One Open… ────────────────────────────────────────────────────────────────
//
// There used to be three: *Open Remote Dataset…*, *Open 3D Catalogue…*,
// *Open VBT…*. Two of them accepted `.fits`, so choosing one meant already
// knowing whether the FITS in hand held an image, a cube or a table — the
// backend's three open routes, put to the user as a question. Now the file is
// chosen first and the backend says what it is; the user is asked only when the
// content really is ambiguous.

void MainWindow::openAnything()
{
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a file: the backend is not reachable.\n\n"
                u"Start a local backend or configure a remote backend URL in Settings, "
                u"then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    // No extension filter: the whole point is that the user does not have to
    // decide what kind of file this is before picking it.
    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString path = dialog.selectedFilePath();
    if (path.isEmpty())
        return;

    dispatchOpenForPath(path);
}


void MainWindow::dispatchOpenForPath(const QString &path)
{
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    // Asked here rather than only in openAnything(): the desktop reaches this
    // without going through the browser, and a backend that is down would
    // otherwise surface as "the backend could not identify this file", followed
    // by a dialog asking the user to choose a viewer that cannot open anything.
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a file: the backend is not reachable.\n\n"
                u"Error: %1"_s.arg(health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }
    const BackendClassifyResult verdict = client.classifyFile(path);

    // A VBT picked by its `.bin` opens through the `.head` beside it: the VBT
    // route parses the header, and handing it the binary would fail on a file
    // the user chose correctly.
    const QString target = verdict.valid && !verdict.openPath.isEmpty() ? verdict.openPath : path;

    const QString opener = verdict.valid ? verdict.opener : QString();
    if (opener == u"dataset"_s) {
        doOpenDataPath(target);
        return;
    }
    if (opener == u"catalogue"_s) {
        doOpenCataloguePath(target, catalogueSiblingGroup(client, target, this));
        return;
    }
    if (opener == u"vbt"_s) {
        doOpenVbtPath(target);
        return;
    }
    if (opener == u"unsupported"_s) {
        // We know what it is and nothing here opens it. Offering the three
        // viewers would only move the failure one dialog later.
        QMessageBox box(QMessageBox::Information, u"Cannot open this file"_s,
                        u"%1\n\n%2"_s.arg(QFileInfo(path).fileName(), verdict.detail),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }

    // Ambiguous, unrecognised, or a backend too old to have the classifier:
    // ask, rather than guess and open the wrong viewer.
    const bool isTable = verdict.valid && verdict.kind == u"fits-table"_s;
    const QString detail = verdict.valid
            ? verdict.detail
            : u"The backend could not identify this file (%1)."_s.arg(
                      verdict.error.isEmpty() ? u"no detail"_s : verdict.error);

    QMessageBox box(this);
    box.setWindowTitle(u"How should this open?"_s);
    box.setIcon(QMessageBox::Question);
    box.setTextFormat(Qt::PlainText);
    box.setText(u"%1\n\n%2"_s.arg(QFileInfo(path).fileName(), detail));
    auto *asCatalogue = box.addButton(u"3-D Catalogue"_s, QMessageBox::AcceptRole);
    auto *asDataset = box.addButton(u"Image or Cube"_s, QMessageBox::AcceptRole);
    QPushButton *asVbt = isTable ? nullptr
                                 : box.addButton(u"VisIVO Binary Table"_s,
                                                 QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(isTable ? asCatalogue : asDataset);
    box.exec();

    if (box.clickedButton() == asCatalogue)
        doOpenCataloguePath(target, catalogueSiblingGroup(client, target, this));
    else if (box.clickedButton() == asDataset)
        doOpenDataPath(target);
    else if (asVbt && box.clickedButton() == asVbt)
        doOpenVbtPath(target);
}


void MainWindow::openVbt()
{
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();

    BackendClient client(backendUrl, backendToken);
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a VBT dataset: the backend is not reachable.\n\n"
                u"Start a local backend or configure a remote backend URL in Settings, "
                u"then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    // Either half of the pair: the classifier resolves a picked `.bin` to the
    // `.head` beside it, and a user who sees both in the folder should not have
    // to know which one the application wants.
    dialog.setExtensionFilter({u".bin.head"_s, u".head"_s, u".bin"_s}, u"VBT files only"_s);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString picked = dialog.selectedFilePath();
    if (picked.isEmpty()) {
        return;
    }

    // Through the classifier, not straight to doOpenVbtPath(): `/v1/vbt/open`
    // wants the HEADER, and a user who picks the `.bin` — now that the browser
    // lets them — would otherwise hand the binary to a parser expecting four
    // lines of text. The classifier answers with the `.head` beside it.
    dispatchOpenForPath(picked);
}


void MainWindow::openHips2Fits()
{
    auto *dialog = new Hips2FitsDialog(this->settings->getBackendUrl(),
                                       this->settings->getBackendToken(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &Hips2FitsDialog::cutoutReady, this,
                     [this](const QString &path) {
                         // It is a FITS in the workspace like any other: the
                         // unified open decides whether it is an image or a cube.
                         this->dispatchOpenForPath(path);
                     });
    dialog->open();
}


void MainWindow::openConeSearch()
{
    auto *window = new ConeSearchWindow(this->settings->getBackendUrl(),
                                        this->settings->getBackendToken(), this);
    QObject::connect(window, &ConeSearchWindow::catalogueReady, this,
                     [this](const QString &path) {
                         // The search saved every row as CSV, so what the legacy
                         // could only show in a table is a catalogue here.
                         this->doOpenCataloguePath(path);
                     });
    window->show();
    window->raise();
    window->activateWindow();
}


void MainWindow::openArepoSnapshot()
{
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();

    BackendClient client(backendUrl, backendToken);
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open an AREPO snapshot: the backend is not reachable.\n\nError: %1"_s
                        .arg(health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }

    // Browsed on the BACKEND's filesystem: a simulation snapshot is tens of
    // gigabytes and lives where the compute is. The extraction reads it there
    // and writes the cube there too.
    RemoteFileBrowserDialog browser(client.baseUrl(), client.token(), this);
    browser.setExtensionFilter({ u".hdf5"_s, u".h5"_s }, u"HDF5 snapshots"_s);
    if (browser.exec() != QDialog::Accepted) {
        return;
    }
    const QString path = browser.selectedFilePath();
    if (path.isEmpty()) {
        return;
    }

    auto *dialog = new ArepoExtractDialog(path, backendUrl, backendToken, this);
    QObject::connect(dialog, &ArepoExtractDialog::cubeReady, this,
                     [this](const QString &backendPath) { this->doOpenDataPath(backendPath); });
    dialog->show();
}

void MainWindow::openVelocityField()
{
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();

    BackendClient client(backendUrl, backendToken);
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a velocity field: the backend is not reachable.\n\nError: %1"_s
                        .arg(health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    dialog.setExtensionFilter({ u".fits"_s, u".fit"_s }, u"FITS velocity fields"_s);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString path = dialog.selectedFilePath();
    if (path.isEmpty()) {
        return;
    }

    // The 64³×3 grid is small; do the two synchronous calls under a wait cursor.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    this->statusBar()->showMessage(u"Opening velocity field…"_s);
    const BackendVelocityOpenResult opened = client.openVelocityField(path);
    if (!opened.valid) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, u"Velocity Field"_s,
                             u"Open failed: %1"_s.arg(opened.error.isEmpty() ? u"unknown error"_s
                                                                             : opened.error));
        return;
    }
    client.setSessionId(opened.sessionId);
    const BackendVelocityGridResult grid = client.fetchVelocityGrid(opened.datasetId);
    QApplication::restoreOverrideCursor();
    if (!grid.valid) {
        QMessageBox::warning(this, u"Velocity Field"_s,
                             u"Grid fetch failed: %1"_s.arg(grid.error.isEmpty()
                                                                    ? u"unknown error"_s
                                                                    : grid.error));
        return;
    }

    auto *win = new vtkWindowVectorField(path, grid, opened.datasetId, client.baseUrl(),
                                         client.token(), client.sessionId(), nullptr);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->resize(1100, 760);
    win->show();
    this->statusBar()->showMessage(
            u"Velocity field opened: %1×%2×%3"_s.arg(grid.nx).arg(grid.ny).arg(grid.nz), 5000);
}


void MainWindow::doOpenVbtPath(const QString &headerPath)
{
    this->m_sampCurrentFitsPath.clear();
    this->m_sampCurrentFitsName.clear();
    this->m_sampCurrentCataloguePath.clear();
    this->m_sampCurrentCatalogueName.clear();
    this->m_sampCurrentCatalogueFormat.clear();
    this->updateSampActions();
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();
    // Read here, on the UI thread: the task below runs on a worker and must not
    // touch Settings. A point table larger than this is truncated, and the task
    // says so rather than letting a partial cloud pass for the whole dataset.
    const int maxPointRows = std::max(1, this->settings->getMaxGlyphs());

    // Disable the action while loading to prevent double-submit
    if (ui->actionOpenVbt) {
        ui->actionOpenVbt->setEnabled(false);
    }
    this->statusBar()->showMessage(u"Opening VBT dataset…"_s);

    auto *watcher = new QFutureWatcher<VbtOpenTaskResult>(this);
    QObject::connect(
            watcher, &QFutureWatcher<VbtOpenTaskResult>::finished, this,
            [this, watcher, headerPath]() {
                watcher->deleteLater();

                if (ui->actionOpenVbt) {
                    ui->actionOpenVbt->setEnabled(true);
                }
                this->statusBar()->clearMessage();

                const auto result = watcher->result();
                if (!result.valid) {
                    QMessageBox::critical(this, u"Could not open VBT"_s, result.error);
                    return;
                }

                QWidget *win = nullptr;
                const QString vbtType = result.openResult.kind == u"volume"_s
                        ? u"vbt-volume"_s : u"vbt"_s;
                if (result.openResult.kind == u"volume"_s) {
                    win = new vtkWindowVbtVolume(
                            result.openResult, result.subsetResult,
                            result.backendUrl, result.backendToken,
                            result.sessionId, headerPath, this);
                } else {
                    auto *vbtWin = new vtkWindowVbt(
                            result.openResult, result.subsetResult,
                            result.backendUrl, result.backendToken,
                            result.sessionId, headerPath, this);
                    // On session expiry the window emits reopenRequested; reuse normal open flow.
                    QObject::connect(vbtWin, &vtkWindowVbt::reopenRequested,
                                     this, &MainWindow::doOpenVbtPath);
                    win = vbtWin;
                }
                win->showMaximized();
                win->raise();
                win->activateWindow();

                // Record after successful open (inside the watcher callback
                // because kind is only known after the async open completes).
                if (this->recentMgr) {
                    // Strip .bin.head suffix for the display name.
                    QString name = QFileInfo(headerPath).fileName();
                    if (name.endsWith(u".bin.head"_s)) {
                        name.chop(9);
                    }
                    this->recentMgr->record(headerPath, name, vbtType);
                }
            });

    watcher->setFuture(
            QtConcurrent::run(&runVbtOpenTask, headerPath, backendUrl, backendToken,
                              maxPointRows));
}

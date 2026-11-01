#include "MainWindow.h"
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

#include <QLabel>
#include <QStatusBar>
#include "HiPSWindow.h"
#include "vtkWindowVbt.h"
#include "vtkWindowVbtVolume.h"
#include "VLKBInventoryTree.h"
#include "SkavaSearchPanel.h"
#include "../skava/SkavaClient.h"
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

#include <cmath>

#include "app/DiagnosticsManager.h"

#include <vtkImageData.h>

using namespace Qt::StringLiterals;

// ── MainWindow: Samp (extracted from MainWindow.cpp) ──

namespace {

bool backendUrlIsLocal(const QString &url)
{
    // A run on a remote backend (--backend) has no local one, even when a
    // tunnel puts the remote backend at a localhost address.
    if (Settings::sessionBackend())
        return false;
    const QUrl parsed(url);
    const QString host = parsed.host().trimmed().toLower();
    return host.isEmpty() || host == QStringLiteral("127.0.0.1")
           || host == QStringLiteral("localhost") || host == QStringLiteral("::1");
}

}  // namespace

void MainWindow::updateSampActions()
{
    const bool connected = this->m_sampClient && this->m_sampClient->isConnected();
    const bool hasCurrentFits = !this->m_sampCurrentFitsPath.isEmpty();
    const bool hasCurrentCatalogue = !this->m_sampCurrentCataloguePath.isEmpty();
    if (m_actSampConnect) {
        m_actSampConnect->setEnabled(!connected);
    }
    if (m_actSampDisconnect) {
        m_actSampDisconnect->setEnabled(connected);
    }
    if (m_actSampSendCurrentFits) {
        m_actSampSendCurrentFits->setEnabled(connected && hasCurrentFits);
        m_actSampSendCurrentFits->setToolTip(
                hasCurrentFits
                        ? u"Broadcast the current FITS dataset to SAMP-compatible tools."_s
                        : u"Open a FITS image or cube first."_s);
    }
    if (m_actSampSendCurrentCatalogue) {
        m_actSampSendCurrentCatalogue->setEnabled(connected && hasCurrentCatalogue);
        m_actSampSendCurrentCatalogue->setToolTip(
                hasCurrentCatalogue
                        ? u"Broadcast the current catalogue to SAMP-compatible tools."_s
                        : u"Open a catalogue first."_s);
    }
}


void MainWindow::handleSampFitsDataset(const QString &backendPath, const QString &mtype)
{
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
            QStringLiteral("Received %1 for %2").arg(mtype, backendPath),
            QString(), QString(), QStringLiteral("receive"));

    this->syncSampSettings();

    QString stagedPath = backendPath;
    const QUrl url(backendPath);
    if (url.isValid()
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
        const auto imported = this->m_sampBackendClient->importSampUrl(
                backendPath, QFileInfo(url.path()).fileName());
        if (!imported.valid) {
            const QString message =
                    QStringLiteral("Failed to import the SAMP FITS URL into the backend: %1")
                            .arg(imported.error);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                    message, QString(), QString(), QStringLiteral("receive"));
            QMessageBox::warning(this, u"SAMP receive failed"_s, message);
            return;
        }
        stagedPath = imported.backendPath;
    } else if (!backendUrlIsLocal(this->settings->getBackendUrl())
               && QFileInfo(backendPath).isAbsolute()) {
        const auto uploaded = this->m_sampBackendClient->uploadFileToSampStorage(backendPath);
        if (!uploaded.valid) {
            const QString message =
                    QStringLiteral("Failed to stage the desktop-local FITS file on the backend: %1")
                            .arg(uploaded.error);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                    message, QString(), QString(), QStringLiteral("receive"));
            QMessageBox::warning(this, u"SAMP receive failed"_s, message);
            return;
        }
        stagedPath = uploaded.backendPath;
    }

    this->doOpenDataPath(stagedPath);
}


void MainWindow::handleSampTableLoad(const QString &url, const QString &localPath)
{
    this->syncSampSettings();

    QString stagedPath = localPath.isEmpty() ? url : localPath;
    const QUrl parsed(url);
    if (parsed.isValid()
        && (parsed.scheme() == QStringLiteral("http") || parsed.scheme() == QStringLiteral("https"))) {
        const auto imported = this->m_sampBackendClient->importSampUrl(
                url, QFileInfo(parsed.path()).fileName());
        if (!imported.valid) {
            const QString message =
                    QStringLiteral("Failed to import the SAMP table into the backend: %1")
                            .arg(imported.error);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                    message, QString(), QString(), QStringLiteral("receive"));
            QMessageBox::warning(this, u"SAMP table import failed"_s, message);
            return;
        }
        stagedPath = imported.backendPath;
    } else if (!backendUrlIsLocal(this->settings->getBackendUrl())
               && QFileInfo(stagedPath).isAbsolute()) {
        const auto uploaded = this->m_sampBackendClient->uploadFileToSampStorage(stagedPath);
        if (!uploaded.valid) {
            const QString message =
                    QStringLiteral("Failed to stage the SAMP table on the backend: %1")
                            .arg(uploaded.error);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                    message, QString(), QString(), QStringLiteral("receive"));
            QMessageBox::warning(this, u"SAMP table import failed"_s, message);
            return;
        }
        stagedPath = uploaded.backendPath;
    }

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
            QStringLiteral("Received SAMP table %1").arg(url),
            QString(), QString(), QStringLiteral("receive"));
    this->doOpenCataloguePath(stagedPath);
}


void MainWindow::handleSampSkyCoord(double raDeg, double decDeg)
{
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
            QStringLiteral("Received coord.pointAt.sky RA=%1 Dec=%2")
                    .arg(raDeg, 0, 'f', 6)
                    .arg(decDeg, 0, 'f', 6),
            QString(), QString(), QStringLiteral("receive"));

    if (!this->m_lastHiPSWindow) {
        this->openHiPS();
    }
    if (this->m_lastHiPSWindow) {
        this->m_lastHiPSWindow->setCenter(raDeg, decDeg);
        this->m_lastHiPSWindow->raise();
        this->m_lastHiPSWindow->activateWindow();
    }
}


void MainWindow::sendCurrentFitsViaSamp()
{
    if (this->m_sampCurrentFitsPath.isEmpty()) {
        return;
    }
    this->syncSampSettings();
    const auto result =
            this->m_sampBackendClient->sendFitsToSamp(this->m_sampCurrentFitsPath,
                                                      this->m_sampCurrentFitsName);
    if (!result.valid) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                QStringLiteral("Failed to send FITS dataset via SAMP: %1").arg(result.error),
                QString(), QString(), QStringLiteral("send"));
        QMessageBox::critical(
                this, u"SAMP send failed"_s,
                u"Could not send the current FITS dataset via SAMP.\n\n%1"_s.arg(
                        result.error.isEmpty() ? u"Backend returned no error detail."_s
                                               : result.error));
        return;
    }

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
            QStringLiteral("Sent FITS dataset via SAMP (%1).")
                    .arg(result.sentDirect ? QStringLiteral("direct") : result.jobId),
            QString(), QString(), QStringLiteral("send"));
    this->statusBar()->showMessage(u"SAMP send queued"_s, 3000);
}


void MainWindow::sendCurrentCatalogueViaSamp()
{
    if (this->m_sampCurrentCataloguePath.isEmpty()) {
        return;
    }
    this->syncSampSettings();
    const auto result =
            this->m_sampBackendClient->sendCatalogueToSamp(this->m_sampCurrentCataloguePath,
                                                           this->m_sampCurrentCatalogueFormat,
                                                           this->m_sampCurrentCatalogueName);
    if (!result.valid) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                QStringLiteral("Failed to send catalogue via SAMP: %1").arg(result.error),
                QString(), QString(), QStringLiteral("send"));
        QMessageBox::critical(
                this, u"SAMP send failed"_s,
                u"Could not send the current catalogue via SAMP.\n\n%1"_s.arg(
                        result.error.isEmpty() ? u"Backend returned no error detail."_s
                                               : result.error));
        return;
    }

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
            QStringLiteral("Sent catalogue via SAMP (%1).")
                    .arg(result.sentDirect ? QStringLiteral("direct") : result.jobId),
            QString(), QString(), QStringLiteral("send"));
    this->statusBar()->showMessage(u"SAMP catalogue send queued"_s, 3000);
}

#include "MainWindow.h"
#include "app/DatasetName.h"
#include "CatalogueFormat.h"
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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>

#include <cmath>
#include <memory>

#include "app/DiagnosticsManager.h"

#include <vtkImageData.h>

using namespace Qt::StringLiterals;

// ── MainWindow: Samp (extracted from MainWindow.cpp) ──

namespace {

/// Larger received files are refused: the upload to the backend holds the
/// whole file in memory, base64-encoded, on both sides.
constexpr qint64 kMaxSampDownloadBytes = 512LL * 1024 * 1024;

/// Above this many rows, sending a catalogue asks first.
constexpr qint64 kLargeSampTableRows = 1000000;

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
    const bool connecting = this->m_sampClient && this->m_sampClient->isConnecting();
    const bool waiting = this->m_sampClient && this->m_sampClient->isWaitingForHub();
    // Staging, or asking the hub first whether anybody takes the file.
    const bool sendBusy = this->m_sampSendInFlight || !this->m_sampSubscriberChecks.isEmpty();
    if (m_actSampStatus) {
        QString status;
        QString tip;
        if (connected) {
            const QString who = u"VisIVO [%1]"_s.arg(this->m_sampClient->publicId());
            const bool receiving = this->m_sampClient->isReceiving();
            status = receiving ? u"Connected as %1, receiving"_s.arg(who)
                               : u"Connected as %1, cannot receive"_s.arg(who);
            tip = receiving ? this->m_sampClient->hubUrl() : this->m_sampClient->receiveProblem();
        } else if (waiting) {
            // The quiet retries count as connecting, but the user sees a wait.
            status = u"Not connected: waiting for a hub"_s;
            tip = u"No SAMP hub answers. VisIVO connects as soon as one is running "
                  u"(start TOPCAT, Aladin or samp_hub); Disconnect From Hub stops waiting."_s;
        } else {
            status = connecting ? u"Connecting…"_s : u"Not connected"_s;
        }
        m_actSampStatus->setText(status);
        m_actSampStatus->setToolTip(tip);
    }
    if (m_actSampConnect) {
        // While waiting for a lost hub, Connect still tries at once (and says
        // why it fails).
        m_actSampConnect->setEnabled(!connected && (!connecting || waiting));
    }
    if (m_actSampDisconnect) {
        // Also while connecting or waiting for a lost hub: it stops the retries.
        m_actSampDisconnect->setEnabled(connected || connecting || waiting);
    }
    if (m_actSampSendCurrentFits) {
        m_actSampSendCurrentFits->setEnabled(connected && hasCurrentFits && !sendBusy);
        m_actSampSendCurrentFits->setToolTip(
                hasCurrentFits
                        ? u"Broadcast the current FITS dataset to SAMP-compatible tools."_s
                        : u"Open a FITS image or cube first."_s);
    }
    if (m_actSampSendCurrentCatalogue) {
        m_actSampSendCurrentCatalogue->setEnabled(connected && hasCurrentCatalogue && !sendBusy);
        m_actSampSendCurrentCatalogue->setToolTip(
                hasCurrentCatalogue
                        ? u"Broadcast the current catalogue to SAMP-compatible tools."_s
                        : u"Open a catalogue first."_s);
    }
}


void MainWindow::stageSampUrl(const QString &url, const QString &mtype, const QString &fileName,
                              const QString &what, const QString &replyToken,
                              std::function<void(const QString &backendPath, bool staged)> done)
{
    this->syncSampSettings();
    // Failures are shown without blocking: the message came from another
    // application, and the user may be working in this one. A sender that
    // made a call is told too.
    QPointer<SampClient> samp = this->m_sampClient;
    auto fail = [this, what, replyToken, samp](const QString &detail) {
        if (samp)
            samp->replyToCall(replyToken, detail);
        const QString message = u"Could not open the %1 sent over SAMP: %2"_s.arg(what, detail);
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                message, QString(), QString(), QStringLiteral("receive"));
        this->statusBar()->showMessage(message, 8000);
        auto *box = new QMessageBox(QMessageBox::Warning, u"SAMP receive failed"_s, message,
                                    QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    };

    const QUrl parsed(url);
    if (parsed.isLocalFile()) {
        const QString localPath = parsed.toLocalFile();
        // On the same machine a local backend reads the file where it is,
        // when its name says its format (the backend reads a catalogue's
        // format from the extension); otherwise it is staged under fileName.
        if (backendUrlIsLocal(this->settings->getBackendUrl())
            && Samp::hasFormatExtension(QFileInfo(localPath).fileName(), mtype)) {
            done(localPath, false);
            return;
        }
        const auto uploaded = this->m_sampBackendClient->uploadFileToSampStorage(localPath, fileName);
        if (!uploaded.valid) {
            fail(uploaded.error);
            return;
        }
        done(uploaded.backendPath, true);
        return;
    }
    if (parsed.scheme() != u"http"_s && parsed.scheme() != u"https"_s) {
        fail(u"unsupported URL %1"_s.arg(url));
        return;
    }

    // An http URL from a hub is usually hub-local (http://127.0.0.1:...), which
    // a remote backend cannot reach: the client fetches it and uploads it,
    // for local and remote backends alike.
    if (!this->m_sampNam)
        this->m_sampNam = new QNetworkAccessManager(this);
    QNetworkRequest request(parsed);
    request.setTransferTimeout(120000);
    QNetworkReply *reply = this->m_sampNam->get(request);
    this->statusBar()->showMessage(u"SAMP: downloading %1…"_s.arg(fileName));
    auto tooLarge = std::make_shared<bool>(false);
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [reply, tooLarge](qint64 received, qint64) {
                         if (received > kMaxSampDownloadBytes && !*tooLarge) {
                             *tooLarge = true;
                             reply->abort();
                         }
                     });
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, fileName, fail, tooLarge, done = std::move(done)]() {
                         reply->deleteLater();
                         if (*tooLarge) {
                             fail(u"the file is larger than %1 MB"_s.arg(
                                     kMaxSampDownloadBytes / (1024 * 1024)));
                             return;
                         }
                         if (reply->error() != QNetworkReply::NoError) {
                             fail(reply->errorString());
                             return;
                         }
                         const QByteArray data = reply->readAll();
                         if (data.isEmpty()) {
                             fail(u"the sender served an empty file"_s);
                             return;
                         }
                         this->statusBar()->showMessage(
                                 u"SAMP: staging %1 on the backend…"_s.arg(fileName));
                         const auto uploaded =
                                 this->m_sampBackendClient->uploadDataToSampStorage(data, fileName);
                         if (!uploaded.valid) {
                             fail(uploaded.error);
                             return;
                         }
                         this->statusBar()->clearMessage();
                         done(uploaded.backendPath, true);
                     });
}


void MainWindow::handleSampImageLoad(const QString &url, const QString &name,
                                     const QString &replyToken)
{
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
            QStringLiteral("Received image.load.fits for %1").arg(url),
            QString(), QString(), QStringLiteral("receive"));
    const QString mtype = QString::fromLatin1(Samp::kImageLoadFits);
    this->stageSampUrl(url, mtype, Samp::stagedFileName(url, name, mtype), u"image"_s, replyToken, [this, url, name, mtype, replyToken](const QString &path, bool staged) {
                           // A staged copy is shown under the sender's name
                           // (the viewers ask datasetDisplayName), and kept out
                           // of Recent Datasets; set before the viewer exists.
                           const QString shownName = Samp::displayName(url, name);
                           if (staged)
                               visivo::rememberReceivedName(path, shownName);
                           // The backend has read the FITS when this returns
                           // true; the viewer then fetches pixels on its own,
                           // and a failure there shows in the viewer.
                           const bool opened = this->doOpenDataPath(path);
                           // Sent on again, it is staged under this name: a
                           // file name with the right extension, not the label.
                           if (opened && staged && !shownName.isEmpty())
                               this->m_sampCurrentFitsName =
                                       Samp::stagedFileName(QString(), shownName, mtype);
                           if (this->m_sampClient)
                               this->m_sampClient->replyToCall(
                                       replyToken, opened ? QString()
                                                     : u"the backend could not open the image"_s);
                       });
}


void MainWindow::handleSampTableLoad(const QString &url, const QString &tableId,
                                     const QString &name, const QString &mtype,
                                     const QString &replyToken)
{
    this->stageSampUrl(
            url, mtype, Samp::stagedFileName(url, name, mtype), u"table"_s, replyToken,
            [this, url, tableId, name, mtype, replyToken](const QString &path, bool staged) {
                DiagnosticsManager::instance()->publish(
                        DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                        QStringLiteral("Received SAMP table %1%2")
                                .arg(url, tableId.isEmpty() ? QString()
                                                            : u" (table-id %1)"_s.arg(tableId)),
                        QString(), QString(), QStringLiteral("receive"));
                // The sender's name for the table, not the hashed name it was
                // staged under: SAMP's "name", else the URL's file name. The
                // window shows it wherever it would show the file's name.
                const QString shownName = Samp::displayName(url, name);
                vtkWindowCatalogue3D *win =
                        this->doOpenCataloguePath(path, QStringList(), shownName, staged);
                const bool opened = win && win->hasLoadedRows();
                if (win && !shownName.isEmpty()) {
                    // Sent on again, it is staged under this name: a file name
                    // (no slashes, the right extension), not the label.
                    this->m_sampCurrentCatalogueName =
                            Samp::stagedFileName(QString(), shownName, mtype);
                }
                // Keep how the sender names the table, so its row messages
                // (and VisIVO's, back to it) find the catalogue.
                if (opened)
                    this->m_sampTables.remember({ tableId, url }, path);
                if (this->m_sampClient)
                    this->m_sampClient->replyToCall(
                            replyToken, opened ? QString()
                                          : u"the backend could not open the table"_s);
            });
}


QString MainWindow::sampSenderName(const QString &senderId) const
{
    if (this->m_sampClient) {
        for (const SampClientInfo &info : this->m_sampClient->registeredClients()) {
            if (info.id == senderId && !info.name.trimmed().isEmpty())
                return info.name.trimmed();
        }
    }
    return senderId.isEmpty() ? u"A SAMP tool"_s : senderId;
}


std::optional<SampTableRegistry::Entry> MainWindow::sampTableFor(const QString &tableId,
                                                                const QString &url) const
{
    if (const auto entry = this->m_sampTables.find({ tableId, url }))
        return entry;
    if (const auto pending = this->m_sampPendingTables.find({ tableId, url }))
        return SampTableRegistry::Entry{ { tableId, pending->url }, pending->localKey,
                                         pending->subset };
    return std::nullopt;
}


QString MainWindow::sampLocalKeyFor(const QString &tableId, const QString &url) const
{
    const auto entry = this->sampTableFor(tableId, url);
    return entry ? entry->localKey : QString();
}


void MainWindow::handleSampHighlightRow(const QString &tableId, const QString &url, qint64 row,
                                        const QString &senderId)
{
    // A tool broadcasts the highlights of every table it holds; most are not
    // VisIVO's, and say nothing worth a line in Diagnostics.
    const auto table = this->sampTableFor(tableId, url);
    if (!table) {
        qDebug() << "[SAMP] highlight for a table VisIVO does not hold:" << tableId << url;
        return;
    }
    const QString key = table->localKey;
    // Row k of a subset VisIVO sent is the k-th row it was cut from.
    const qint64 catalogueRow = Samp::toCatalogueRow(table->subset, row);
    if (catalogueRow < 0) {
        qDebug() << "[SAMP] highlight of a row the subset does not have:" << row;
        return;
    }
    const QString from = this->sampSenderName(senderId);
    bool shown = false;
    // Looked up now, on the GUI thread: a viewer closed since the table was
    // received is simply not found.
    for (vtkWindowCatalogue3D *win : this->findChildren<vtkWindowCatalogue3D *>()) {
        if (win->sampTableKey() == key) {
            win->highlightSampRow(catalogueRow, from);
            shown = true;
        }
    }
    if (!shown)
        qDebug() << "[SAMP] highlight for a catalogue no longer open:" << key;
}


void MainWindow::handleSampSelectRows(const QString &tableId, const QString &url,
                                      const QList<qint64> &rows, const QString &senderId)
{
    const auto table = this->sampTableFor(tableId, url);
    if (!table) {
        qDebug() << "[SAMP] selection for a table VisIVO does not hold:" << tableId << url;
        return;
    }
    const QString key = table->localKey;
    const QList<qint64> catalogueRows = Samp::toCatalogueRows(table->subset, rows);
    const QString from = this->sampSenderName(senderId);
    bool shown = false;
    for (vtkWindowCatalogue3D *win : this->findChildren<vtkWindowCatalogue3D *>()) {
        if (win->sampTableKey() == key) {
            win->selectSampRows(catalogueRows, from);
            shown = true;
        }
    }
    if (shown) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                QStringLiteral("%1 selected %2 rows of %3.").arg(from).arg(rows.size()).arg(key),
                QString(), QString(), QStringLiteral("receive"));
    }
}


void MainWindow::sendSampRowHighlight(const QString &tableKey, qint64 row)
{
    if (!this->m_sampClient || !this->m_sampClient->isConnected())
        return;
    // Every identity the catalogue has in the other tools: the table it was
    // received as, and each copy VisIVO sent. They are the same rows, so the
    // row is highlighted in all of them, and the table the user is working
    // with in TOPCAT (usually the original) follows the click. A subset copy
    // numbers its rows its own way, and may not have this one.
    for (const SampTableRegistry::Entry &entry : this->m_sampTables.entriesFor(tableKey)) {
        const qint64 tableRow = Samp::toTableRow(entry.subset, row);
        if (tableRow >= 0)
            this->m_sampClient->sendHighlightRow(entry.ref, tableRow);
    }
}


bool MainWindow::sampToolsHoldTable(const QString &tableKey) const
{
    return !tableKey.isEmpty() && !this->m_sampTables.entriesFor(tableKey).isEmpty();
}


int MainWindow::selectRowsInSampTools(const QString &tableKey, const QList<qint64> &rows)
{
    if (!this->m_sampClient || !this->m_sampClient->isConnected() || tableKey.isEmpty())
        return 0;
    int named = 0;
    for (const SampTableRegistry::Entry &entry : this->m_sampTables.entriesFor(tableKey)) {
        // A subset copy gets the rows it holds, as it numbers them; one that
        // holds none of them gets an empty selection, which clears its own,
        // as the user would expect of "select these rows".
        this->m_sampClient->sendSelectRowList(entry.ref, Samp::toTableRows(entry.subset, rows));
        ++named;
    }
    if (named > 0) {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                QStringLiteral("Selected %1 rows of %2 in the SAMP tools (%3 tables).")
                        .arg(rows.size())
                        .arg(visivo::datasetDisplayName(tableKey))
                        .arg(named),
                QString(), QString(), QStringLiteral("send"));
    }
    return named;
}


bool MainWindow::sendSkyPositionViaSamp(double raDeg, double decDeg, const QString &recipientId)
{
    if (!this->m_sampClient || !this->m_sampClient->isConnected())
        return false;
    const auto params = Samp::pointAtSkyParams(raDeg, decDeg);
    if (!params)
        return false;
    this->m_sampClient->notify(recipientId, QString::fromLatin1(Samp::kCoordPointAtSky), *params);
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
            QStringLiteral("Sent coord.pointAt.sky RA=%1 Dec=%2 to %3.")
                    .arg(params->value(u"ra"_s).toString(), params->value(u"dec"_s).toString(),
                         recipientId.isEmpty() ? u"every tool"_s
                                               : this->sampSenderName(recipientId)),
            QString(), QString(), QStringLiteral("send"));
    return true;
}


bool MainWindow::sampConnected() const
{
    return this->m_sampClient && this->m_sampClient->isConnected();
}


QList<Samp::Recipient> MainWindow::sampRecipients(const QString &mtype) const
{
    return this->m_sampClient ? this->m_sampClient->recipientsFor(mtype) : QList<Samp::Recipient>();
}


void MainWindow::connectSampHub()
{
    if (this->m_actSampConnect && this->m_actSampConnect->isEnabled())
        this->m_actSampConnect->trigger();
}


bool MainWindow::sampSendBusy() const
{
    return this->m_sampSendInFlight || !this->m_sampSubscriberChecks.isEmpty();
}


void MainWindow::showSampStatus(QMainWindow *from, const QString &text, int timeoutMs)
{
    // The Data Hub's status bar, and the viewer's: a maximised viewer hides
    // the Data Hub, and the user is looking at the window they sent from.
    this->statusBar()->showMessage(text, timeoutMs);
    if (from && from != this && from->statusBar())
        from->statusBar()->showMessage(text, timeoutMs);
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
        this->m_lastHiPSWindow->showSkyPosition(raDeg, decDeg);
        this->m_lastHiPSWindow->raise();
        this->m_lastHiPSWindow->activateWindow();
    }
}


void MainWindow::sendViaSamp(const QString &what, const QString &name, const QString &recipientId,
                             QMainWindow *fromWindow,
                             std::function<BackendSampSendResult(const BackendClient &)> stage,
                             const QString &pendingTableId)
{
    QPointer<QMainWindow> from = fromWindow;
    if (this->m_sampSendInFlight) {
        this->m_sampPendingTables.remove(pendingTableId);
        this->showSampStatus(from, u"SAMP: a file is still being prepared for sending."_s, 4000);
        return;
    }
    this->syncSampSettings();
    this->m_sampSendInFlight = true;
    this->updateSampActions();
    this->showSampStatus(from, u"SAMP: preparing %1 for sending…"_s.arg(name));

    // Staging converts and copies the whole file on the backend: seconds to
    // minutes for a large table, so not on the GUI thread.
    const QString baseUrl = this->m_sampBackendClient->baseUrl();
    const QString token = this->m_sampBackendClient->token();
    auto *watcher = new QFutureWatcher<BackendSampSendResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSampSendResult>::finished, this,
                     [this, watcher, what, localName = name, baseUrl, recipientId, from,
                      pendingTableId]() {
        watcher->deleteLater();
        this->m_sampSendInFlight = false;
        this->updateSampActions();
        const BackendSampSendResult result = watcher->result();
        // Nothing went out: the table's identity must not stay behind.
        const auto forgetTable = [this, pendingTableId]() {
            this->m_sampPendingTables.remove(pendingTableId);
        };
        // The name the tools are given, which is what they show: the backend
        // may have renamed it (an IPAC table goes out as a VOTable, .vot).
        const QString sentName = result.params.value(u"name"_s).toString().trimmed();
        const QString name = sentName.isEmpty() ? localName : sentName;
        if (!result.valid) {
            forgetTable();
            const QString detail =
                    result.error.isEmpty() ? u"Backend returned no error detail."_s : result.error;
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                    QStringLiteral("Failed to send the %1 %2 via SAMP: %3").arg(what, name, detail),
                    QString(), QString(), QStringLiteral("send"));
            this->statusBar()->clearMessage();
            if (from && from->statusBar())
                from->statusBar()->clearMessage();
            QMessageBox::critical(from ? from.data() : static_cast<QWidget *>(this),
                                  u"SAMP send failed"_s,
                                  u"Could not send the %1 via SAMP.\n\n%2"_s.arg(what, detail));
            return;
        }
        const QString size = result.sizeBytes > 0
                ? u", %1 MB"_s.arg(double(result.sizeBytes) / (1024.0 * 1024.0), 0, 'f', 1)
                : QString();
        const QString rows = result.rows >= 0 ? u"%L1 rows"_s.arg(result.rows) : QString();
        const QString facts = rows.isEmpty() && size.isEmpty()
                ? QString()
                : u" (%1%2)"_s.arg(rows, rows.isEmpty() ? size.mid(2) : size);
        if (!result.mtype.isEmpty()
            && (!this->m_sampClient || !this->m_sampClient->isConnected())) {
            // The backend staged it and handed the message back; with no hub
            // there is nobody to give it to.
            forgetTable();
            const QString message =
                    u"SAMP: the hub went away while %1 was being prepared; nothing was sent. "
                    u"Reconnect and send it again."_s.arg(name);
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                    message, QString(), QString(), QStringLiteral("send"));
            this->showSampStatus(from, message, 10000);
            return;
        }
        if (result.mtype.isEmpty()) {
            // An older backend queued it itself: it goes out as a
            // notification, which no tool answers.
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                    QStringLiteral("Queued the %1 %2%3 for SAMP; the receiving tools do not "
                                   "confirm a queued message.").arg(what, name, facts),
                    QString(), QString(), QStringLiteral("send"));
            this->showSampStatus(from, u"SAMP: %1 queued for sending"_s.arg(name), 5000);
            return;
        }
        // The file is on the backend that staged it, even if the settings
        // name another one by now: its URL goes into the message.
        QVariantMap params = result.params;
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (it.value().typeId() == QMetaType::QString)
                it.value() = it.value().toString().replace(u"__BACKEND_BASE_URL__"_s, baseUrl);
        }
        // The URL the tools will name the table by, known before the hub
        // says it went out: a row message can come first.
        this->m_sampPendingTables.setUrl(params.value(u"table-id"_s).toString().trimmed(),
                                         params.value(u"url"_s).toString().trimmed());
        const QString tag = this->m_sampClient->callAll(result.mtype, params, recipientId);
        this->m_sampSends.insert(tag, SampSend{ name, false, {}, {}, {}, from, pendingTableId });
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                QStringLiteral("Sending the %1 %2%3 via SAMP to %4; waiting for the answers.")
                        .arg(what, name, facts,
                             recipientId.isEmpty() ? u"every tool that takes it"_s
                                                   : this->sampSenderName(recipientId)),
                QString(), QString(), QStringLiteral("send"));
        this->showSampStatus(from, u"SAMP: %1 sent, waiting for an answer…"_s.arg(name));
    });
    watcher->setFuture(QtConcurrent::run([baseUrl, token, stage]() {
        BackendClient client(baseUrl, token);
        return stage(client);
    }));
}


void MainWindow::showSampSendProgress(const QString &tag, const QString &answer)
{
    const auto it = this->m_sampSends.find(tag);
    if (it == this->m_sampSends.end())
        return;
    if (!answer.isEmpty())
        it->lastAnswer = answer;
    const QString lastAnswer = it->lastAnswer;
    const QPointer<QMainWindow> from = it->from;
    QStringList waiting;
    if (it->recipientsKnown) {
        for (const QString &id : it->asked) {
            if (!it->answered.contains(id))
                waiting << this->sampSenderName(id);
        }
    }
    if (it->recipientsKnown && waiting.isEmpty()) {
        // Every tool asked has answered (maybe before the hub said whom it
        // asked): the last answer stays on screen, not a "waiting".
        const QString done = lastAnswer.isEmpty() ? u"SAMP: %1 sent."_s.arg(it->name) : lastAnswer;
        this->m_sampSends.erase(it);
        this->showSampStatus(from, done, 10000);
        return;
    }
    const QString pending = waiting.isEmpty() ? u"waiting for an answer…"_s
                                              : u"waiting for %1…"_s.arg(waiting.join(u", "_s));
    this->showSampStatus(from, lastAnswer.isEmpty() ? u"SAMP: %1 sent, %2"_s.arg(it->name, pending)
                                                    : u"%1 (%2)"_s.arg(lastAnswer, pending));
}


void MainWindow::whenSampToolTakes(const QString &mtype, const QString &name,
                                   const QString &recipientId, QMainWindow *from,
                                   std::function<void()> proceed)
{
    if (this->sampSendBusy()) {
        this->showSampStatus(from, u"SAMP: a file is still being prepared for sending."_s, 4000);
        return;
    }
    if (!this->m_sampClient || !this->m_sampClient->isConnected())
        return;
    if (!recipientId.isEmpty()) {
        // The tool was picked from the hub's list, which says what it takes;
        // it may have left since the menu was shown.
        for (const SampClientInfo &info : this->m_sampClient->registeredClients()) {
            if (info.id != recipientId)
                continue;
            if (Samp::subscribes(info, mtype)) {
                proceed();
            } else {
                this->showSampStatus(from, u"SAMP: %1 does not take %2 (%3)."_s.arg(
                                                   this->sampSenderName(recipientId), name, mtype),
                                     10000);
            }
            return;
        }
        this->showSampStatus(from, u"SAMP: that tool has left the hub; nothing was sent."_s,
                             10000);
        return;
    }
    const QString tag = this->m_sampClient->askSubscribers(mtype);
    this->m_sampSubscriberChecks.insert(
            tag, SampSubscriberCheck{ name, mtype, this->settings->getBackendUrl(),
                                      std::move(proceed), from });
    this->updateSampActions();
    this->showSampStatus(from, u"SAMP: looking for a tool that takes %1…"_s.arg(name));
}


void MainWindow::sendCurrentFitsViaSamp()
{
    if (this->m_sampCurrentFitsPath.isEmpty()) {
        return;
    }
    const QString path = this->m_sampCurrentFitsPath;
    const QString name = this->m_sampCurrentFitsName.isEmpty() ? QFileInfo(path).fileName()
                                                               : this->m_sampCurrentFitsName;
    this->sendFitsViaSamp(path, name, QString(), this);
}


void MainWindow::sendFitsViaSamp(const QString &path, const QString &name,
                                 const QString &recipientId, QMainWindow *from)
{
    if (path.isEmpty() || visivo::isFieldReference(path))
        return;   // a field of a VBT volume is no FITS file another tool could load
    const QString mtype = QString::fromLatin1(Samp::kImageLoadFits);
    // Staged under a file name with the right extension, whatever the label.
    const QString fileName = Samp::stagedFileName(QString(), name, mtype);
    QPointer<QMainWindow> window = from;
    this->whenSampToolTakes(mtype, fileName, recipientId, from,
                            [this, path, fileName, recipientId, window]() {
        this->sendViaSamp(u"image"_s, fileName, recipientId, window,
                          [path, fileName](const BackendClient &client) {
                              return client.sendFitsToSamp(path, fileName, /*returnMessage=*/true);
                          });
    });
}


void MainWindow::sendCurrentCatalogueViaSamp()
{
    if (this->m_sampCurrentCataloguePath.isEmpty()) {
        return;
    }
    const QString path = this->m_sampCurrentCataloguePath;
    const QString format = this->m_sampCurrentCatalogueFormat;
    const QString name = this->m_sampCurrentCatalogueName.isEmpty() ? QFileInfo(path).fileName()
                                                                    : this->m_sampCurrentCatalogueName;
    // Nobody to take it: said at once, not after the question and the
    // staging below.
    this->whenSampToolTakes(Samp::catalogueLoadMType(format), name, QString(), this,
                            [this, path, format, name]() {
                                this->sendCatalogueViaSamp(path, format, name, QString(), this);
                            });
}


void MainWindow::sendCatalogueFileViaSamp(const QString &path, const QString &name,
                                          const QString &recipientId, QMainWindow *from)
{
    if (path.isEmpty())
        return;
    const QString format = visivo::catalogueFormatForPath(path);
    const QString mtype = Samp::catalogueLoadMType(format);
    const QString fileName = Samp::stagedFileName(QString(), name, mtype);
    QPointer<QMainWindow> window = from;
    this->whenSampToolTakes(mtype, fileName, recipientId, from,
                            [this, path, format, fileName, recipientId, window]() {
                                this->sendCatalogueViaSamp(path, format, fileName, recipientId,
                                                           window);
                            });
}


void MainWindow::sendCatalogueRowsViaSamp(const QString &path, const QString &name,
                                          const QList<qint64> &rows, const QString &recipientId,
                                          QMainWindow *from)
{
    if (path.isEmpty() || rows.isEmpty())
        return;
    const QString format = visivo::catalogueFormatForPath(path);
    // A subset always goes out as a VOTable, under a name that says what it is.
    const QString mtype = QString::fromLatin1(Samp::kTableLoadVotable);
    const QString stem = QFileInfo(Samp::stagedFileName(QString(), name, mtype)).completeBaseName();
    const QString fileName = u"%1_%2rows.vot"_s.arg(stem).arg(rows.size());
    QPointer<QMainWindow> window = from;
    this->whenSampToolTakes(mtype, fileName, recipientId, from,
                            [this, path, format, fileName, rows, recipientId, window]() {
        const QString tableId =
                u"visivo-%1"_s.arg(QUuid::createUuid().toString(QUuid::Id128).left(16));
        // Row k of the table sent is catalogue row rows[k]: the row messages
        // of either tool are mapped through it.
        auto subset = std::make_shared<const SampSubset>(
                std::vector<qint64>(rows.cbegin(), rows.cend()));
        this->m_sampPendingTables.add(tableId, path, subset,
                                      QDateTime::currentMSecsSinceEpoch());
        this->sendViaSamp(u"selection"_s, fileName, recipientId, window,
                          [path, format, fileName, tableId, rows](const BackendClient &client) {
            BackendSampSendResult result = client.sendCatalogueToSamp(
                    path, format, fileName, tableId, /*returnMessage=*/true, rows);
            if (result.valid && !result.subset) {
                // An older backend ignored the rows and staged the whole
                // table: that is not what the user asked to send.
                result = BackendSampSendResult{};
                result.error = u"This backend cannot send a selection of rows (it would send "
                               u"the whole table). Update the backend, or send the whole "
                               u"catalogue."_s;
            }
            return result;
        }, tableId);
    });
}


void MainWindow::sendCatalogueViaSamp(const QString &path, const QString &format,
                                      const QString &name, const QString &recipientId,
                                      QMainWindow *from)
{
    // Millions of rows are a lot for a desktop table tool: TOPCAT keeps the
    // whole table in memory, and staging it takes a while. Say so first.
    qint64 rows = -1;
    for (vtkWindowCatalogue3D *win : this->findChildren<vtkWindowCatalogue3D *>()) {
        if (win->cataloguePath() == path)
            rows = std::max(rows, win->tableRowCount());
    }
    if (rows > kLargeSampTableRows) {
        const auto answer = QMessageBox::question(
                from ? static_cast<QWidget *>(from) : this, u"Send a large catalogue?"_s,
                u"%1 has %L2 rows. Preparing it takes a while, and the receiving tool "
                u"(TOPCAT, for example) holds the whole table in memory, which may need "
                u"several GB.\n\nSend it anyway?"_s.arg(name).arg(rows),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    // A table-id of VisIVO's own, so the receiving tool and VisIVO can name
    // the table in row messages (table.highlight.row) both ways.
    const QString tableId =
            u"visivo-%1"_s.arg(QUuid::createUuid().toString(QUuid::Id128).left(16));
    this->m_sampPendingTables.add(tableId, path, {}, QDateTime::currentMSecsSinceEpoch());
    this->sendViaSamp(u"catalogue"_s, name, recipientId, from,
                      [path, format, name, tableId](const BackendClient &client) {
        return client.sendCatalogueToSamp(path, format, name, tableId, /*returnMessage=*/true);
    }, tableId);
}

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
#include "ToolProgressSheet.h"
#include "VlkbPosString.h"
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

// ── MainWindow: Sky (extracted from MainWindow.cpp) ──

void MainWindow::revealVLKBTab()
{
    if (!this->m_tabs || !this->m_vlkbPanel) return;

    // Already in the tab strip → just focus it.
    const int existing = this->m_tabs->indexOf(this->m_vlkbPanel);
    if (existing >= 0) {
        this->m_tabs->setCurrentIndex(existing);
        return;
    }

    // User had closed it earlier; re-add right after the Data Hub.
    this->m_vlkbPanel->setParent(this->m_tabs);
    this->m_vlkbPanel->show();
    const int idx = this->m_tabs->insertTab(1, this->m_vlkbPanel, u"VLKB"_s);
    this->m_tabs->setCurrentIndex(idx);
}


void MainWindow::openHiPS()
{
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();
    auto *hipsWidget = new HiPSWindow(backendUrl, backendToken, m_tabs);
    this->m_lastHiPSWindow = hipsWidget;
    QObject::connect(hipsWidget, &QObject::destroyed, this, [this]() {
        this->m_lastHiPSWindow.clear();
    });
    const int idx = m_tabs->addTab(hipsWidget, u"HiPS Viewer"_s);
    m_tabs->setCurrentIndex(idx);

    if (this->recentMgr) {
        this->recentMgr->record(u"hips"_s, u"HiPS Viewer"_s, u"hips"_s);
    }
}


namespace {
/// How long to wait for the VLKB inventory service before giving up.
constexpr int kVlkbQueryTimeoutMs = 120000;
}   // namespace

void MainWindow::loadPanoramicView()
{
    ui->view->load(QUrl::fromUserInput(this->settings->getPanoramicView()));
}


void MainWindow::changeViewSelectionMode()
{
    if (ui->radioSelNone->isChecked()) {
        ui->view->page()->runJavaScript(u"activatePointSelection(false)"_s);
        ui->view->page()->runJavaScript(u"activateRectangularSelection(false)"_s);
        return;
    }

    if (ui->radioSelPoint->isChecked()) {
        ui->view->page()->runJavaScript(u"activatePointSelection(true)"_s);
        return;
    }

    // ui->radioSelRect->isChecked
    ui->view->page()->runJavaScript(u"activateRectangularSelection(true)"_s);
}


void MainWindow::skyRegionSelected(const QString &point, const QString &area)
{
    ui->radioSelNone->setChecked(true);

    // Get coords
    const QStringList coordsStr = point.split(',');
    const double coords[2] = { coordsStr[0].simplified().toDouble(),
                               coordsStr[1].simplified().toDouble() };
    ui->lineLon->setText(QString::number(coords[0]));
    ui->lineLat->setText(QString::number(coords[1]));

    // Get Area
    if (area.isEmpty()) {
        // Point
        if (ui->lineRadius->text().isEmpty()) {
            ui->lineRadius->setText(QString::number(0.1));
        }
        ui->lineDLon->clear();
        ui->lineDLat->clear();
    } else {
        // Rect
        const QStringList areaStr = area.split(',');
        const double rect[2] = { areaStr[0].simplified().toDouble(),
                                 areaStr[1].simplified().toDouble() };
        ui->lineRadius->clear();
        ui->lineDLon->setText(QString::number(rect[0]));
        ui->lineDLat->setText(QString::number(rect[1]));
    }
}


namespace {

/// What the VLKB panel's fields add up to: which shape, and whether it is one
/// the service can be asked about at all.
struct VlkbRegionRequest
{
    bool valid{ false };
    bool circle{ false };
    double lon{ 0.0 };
    double lat{ 0.0 };
    double radius{ 0.0 };
    double halfLon{ 0.0 };
    double halfLat{ 0.0 };
    QString problem;      ///< why it is not usable, when it is not
};

VlkbRegionRequest readRegion(const QString &lonText, const QString &latText,
                             const QString &radiusText, const QString &dLonText,
                             const QString &dLatText)
{
    VlkbRegionRequest request;
    if (lonText.trimmed().isEmpty() || latText.trimmed().isEmpty()) {
        request.problem = QStringLiteral(
                "Enter a galactic longitude and latitude, then a radius (for a cone) "
                "or dl / db (for a box).");
        return request;
    }
    bool lonOk = false;
    bool latOk = false;
    request.lon = lonText.trimmed().toDouble(&lonOk);
    request.lat = latText.trimmed().toDouble(&latOk);
    if (!lonOk || !latOk || !std::isfinite(request.lon) || !std::isfinite(request.lat)) {
        // A QDoubleValidator accepts intermediate text — "-", "1e" — and
        // toDouble() turns those into 0, which is a different, valid-looking
        // region on the sky.
        request.problem = QStringLiteral("The longitude and latitude must be numbers.");
        return request;
    }

    // The shape follows the fields, which is the existing behaviour — but it
    // was invisible, and an empty radius with empty dl/db silently produced a
    // RANGE of zero size that the service answers with nothing at all.
    const auto positiveNumber = [](const QString &text) {
        bool ok = false;
        const double value = text.trimmed().toDouble(&ok);
        return ok && std::isfinite(value) && value > 0.0;
    };
    const bool hasRadius = positiveNumber(radiusText);
    const bool hasBox = positiveNumber(dLonText) || positiveNumber(dLatText);
    if (!hasRadius && !hasBox) {
        request.problem = QStringLiteral(
                "Give the region a size: a radius for a cone search, or dl and db for a box.");
        return request;
    }

    request.circle = hasRadius;
    request.radius = radiusText.toDouble();
    request.halfLon = dLonText.toDouble() * 0.5;
    request.halfLat = dLatText.toDouble() * 0.5;
    if (!request.circle && !(positiveNumber(dLonText) && positiveNumber(dLatText))) {
        request.problem = QStringLiteral("A box needs both dl and db.");
        return request;
    }
    request.valid = true;
    return request;
}

}   // namespace

void MainWindow::refreshVlkbQueryPreview()
{
    if (!m_vlkbQueryPreview)
        return;
    const VlkbRegionRequest request =
            readRegion(ui->lineLon->text(), ui->lineLat->text(), ui->lineRadius->text(),
                       ui->lineDLon->text(), ui->lineDLat->text());
    if (!request.valid) {
        m_vlkbQueryPreview->setText(u"<i>%1</i>"_s.arg(request.problem.toHtmlEscaped()));
        ui->btnQuery->setEnabled(false);
        return;
    }

    const QString pos = request.circle
            ? visivo::circlePosString(request.lon, request.lat, request.radius)
            : visivo::rangePosString(request.lon, request.lat, request.halfLon, request.halfLat);
    QString text = u"Will query <b>%1</b>"_s.arg(request.circle ? u"a cone"_s : u"a box"_s)
            + u"<br><tt>%1</tt>"_s.arg(pos.toHtmlEscaped());
    if (!request.circle && visivo::rangeWrapsThroughZero(request.lon, request.halfLon))
        text += u"<br><i>crosses l = 0°, so the longitude range is sent wrapped</i>"_s;
    m_vlkbQueryPreview->setText(text);
    // …but not while one is already in flight.
    ui->btnQuery->setEnabled(!(m_vlkbQueryWatchdog && m_vlkbQueryWatchdog->isActive()));
}

void MainWindow::queryVlkb()
{
    if (m_vlkbQueryWatchdog && m_vlkbQueryWatchdog->isActive()) {
        // A second query would replace the sheet and the watchdog connection,
        // and the first one's completion would then close the second's sheet.
        QMessageBox::information(this, u"VLKB"_s,
                                 u"A VLKB query is already running."_s);
        return;
    }
    const VlkbRegionRequest request =
            readRegion(ui->lineLon->text(), ui->lineLat->text(), ui->lineRadius->text(),
                       ui->lineDLon->text(), ui->lineDLat->text());
    if (!request.valid) {
        QMessageBox::information(this, u"VLKB"_s, request.problem);
        return;
    }

    const double lon = request.lon;
    const double lat = request.lat;
    const bool useCircle = request.circle;
    const double radius = request.radius;
    const double dl = request.halfLon;
    const double db = request.halfLat;

    // Build the POS string that will later be forwarded to SODA. The wrap is
    // in visivo::normalisedGalacticLongitude (VlkbPosString.h, unit-tested):
    // this used to add 180 to a negative longitude instead of 360, so every
    // query near l = 0 — the Galactic centre — searched the far side of the
    // Galaxy instead.
    const QString posStr = useCircle ? visivo::circlePosString(lon, lat, radius)
                                     : visivo::rangePosString(lon, lat, dl, db);

    ui->btnQuery->setEnabled(false);
    // A query over a crowded region takes seconds, and a disabled button is not
    // feedback: the panel sat there looking broken.
    m_vlkbQuerySheet = visivo::showToolProgressSheet(this, u"Querying the VLKB archive"_s);

    auto *svc = new VlkbQueryService(this->settings->getVLKBUrl(), this->auth, this);

    // Neither signal is guaranteed to arrive: a connection that stalls leaves
    // the service waiting for ever, and with it the disabled button and the
    // progress sheet. The watchdog ends that, and says so.
    if (!m_vlkbQueryWatchdog) {
        m_vlkbQueryWatchdog = new QTimer(this);
        m_vlkbQueryWatchdog->setSingleShot(true);
    }
    m_vlkbQueryWatchdog->disconnect();
    QObject::connect(m_vlkbQueryWatchdog, &QTimer::timeout, this, [this, svc]() {
        visivo::closeToolProgressSheet(m_vlkbQuerySheet);
        ui->btnQuery->setEnabled(true);
        svc->disconnect(this);
        svc->deleteLater();
        QMessageBox::warning(this, u"VLKB query"_s,
                             u"The VLKB archive did not answer within %1 seconds."_s
                                     .arg(kVlkbQueryTimeoutMs / 1000));
    });
    m_vlkbQueryWatchdog->start(kVlkbQueryTimeoutMs);

    // On success — show the inventory tree; svc lives until it emits.
    QObject::connect(svc, &VlkbQueryService::searchFinished, this,
                     [this, svc, posStr](const QByteArray &votable) {
                         m_vlkbQueryWatchdog->stop();
                         visivo::closeToolProgressSheet(m_vlkbQuerySheet);
                         ui->btnQuery->setEnabled(true);
                         svc->deleteLater();

                         // Use nullptr parent so the tree appears as an independent window.
                         auto *tree = new VLKBInventoryTree(votable, posStr, nullptr);
                         tree->setAttribute(Qt::WA_DeleteOnClose);
                         tree->show();
                         tree->raise();

                         // When user double-clicks a dataset, ask the backend to download
                         // the FITS so it lands on the backend filesystem, not the client.
                         // QPointer: the inventory has WA_DeleteOnClose and the
                         // download watcher belongs to MainWindow, so closing
                         // the inventory while a cutout is in flight used to
                         // leave this callback dereferencing a freed window.
                         QPointer<VLKBInventoryTree> treeGuard(tree);
                         QObject::connect(tree, &VLKBInventoryTree::cutoutRequested, this,
                                          [this, treeGuard](const QString &ivoID, const QString &pos,
                                                       const QString &displayName,
                                                       const QString &type) {
                                              const QString sodaUrl =
                                                      this->settings->getVLKBUrl()
                                                      + u"/soda/sync?ID="_s
                                                      + QUrl::toPercentEncoding(ivoID)
                                                      + u"&POS="_s
                                                      + QUrl::toPercentEncoding(pos)
                                                      + u"&POSSYS=GALACTIC"_s;
                                              const QString bearer =
                                                      this->auth->bearerToken(AuthService::VLKB);

                                              auto *watcher =
                                                      new QFutureWatcher<BackendVlkbFetchResult>(this);
                                              const QPointer<VLKBInventoryTree> tree = treeGuard;
                                              QObject::connect(
                                                      watcher,
                                                      &QFutureWatcher<BackendVlkbFetchResult>::finished,
                                                      this,
                                                      [this, tree, displayName, type, watcher]() {
                                                          watcher->deleteLater();
                                                          const auto res = watcher->result();
                                                          if (!res.valid) {
                                                              // The text is the
                                                              // remote service's
                                                              // own: shown as
                                                              // PLAIN text, so
                                                              // markup in it is
                                                              // read, not
                                                              // rendered.
                                                              QMessageBox box(
                                                                      QMessageBox::Critical,
                                                                      u"VLKB cutout failed"_s,
                                                                      res.error,
                                                                      QMessageBox::Ok, this);
                                                              box.setTextFormat(Qt::PlainText);
                                                              box.exec();
                                                              return;
                                                          }
                                                          if (type == u"image"_s) {
                                                              // 2D: reuse the existing viewer when
                                                              // possible, add as layer instead of
                                                              // opening a new window each time.
                                                              if (tree && tree->linkedImageWindow()) {
                                                                  tree->linkedImageWindow()
                                                                          ->addLayerFromBackendPath(
                                                                                  res.path,
                                                                                  displayName);
                                                              } else {
                                                                  this->openVlkbImageLayer(
                                                                          res.path, displayName, tree);
                                                              }
                                                          } else {
                                                              // Cube / other: open in the appropriate
                                                              // specialist viewer.
                                                              this->doOpenDataPath(res.path);
                                                          }
                                                      });
                                              const QString backendUrl =
                                                      this->settings->getBackendUrl();
                                              const QString backendToken =
                                                      this->settings->getBackendToken();
                                              watcher->setFuture(QtConcurrent::run(
                                                      [backendUrl, backendToken,
                                                       sodaUrl, bearer]() {
                                                          BackendClient bc(backendUrl, backendToken);
                                                          return bc.vlkbFetchCutout(sodaUrl, bearer);
                                                      }));
                                          });
                     });

    QObject::connect(svc, &VlkbQueryService::searchFailed, this,
                     [this, svc](const QString &err) {
                         m_vlkbQueryWatchdog->stop();
                         visivo::closeToolProgressSheet(m_vlkbQuerySheet);
                         ui->btnQuery->setEnabled(true);
                         svc->deleteLater();
                         QMessageBox queryBox(QMessageBox::Critical, u"VLKB query failed"_s,
                                              err, QMessageBox::Ok, this);
                         queryBox.setTextFormat(Qt::PlainText);
                         queryBox.exec();
                     });

    // The very string the panel previewed, so preview, search and cutout are
    // one region rather than three constructions of it.
    svc->searchPos(posStr);
}

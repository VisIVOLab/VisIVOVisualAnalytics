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

#include <QWebEngineView>
#include "vtkWindowCatalogue3D.h"
#include "vtkWindowCube.h"
#include "vtkWindowImage.h"

#include <QLabel>
#include <QStatusBar>
#include "HiPSWindow.h"
#include "vtkWindowVbt.h"
#include "vtkWindowVbtVolume.h"
#include "McutoutJobDialog.h"
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
        // Re-frame on every reveal, not only the first: the pane's size is only
        // real once it is on screen, and it changes with the window.
        this->frameGalacticPlane();
        return;
    }

    // User had closed it earlier; re-add right after the Data Hub.
    this->m_vlkbPanel->setParent(this->m_tabs);
    this->m_vlkbPanel->show();
    const int idx = this->m_tabs->insertTab(1, this->m_vlkbPanel, u"VLKB"_s);
    this->m_tabs->setCurrentIndex(idx);
    this->frameGalacticPlane();
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
    const QUrl url = QUrl::fromUserInput(this->settings->getPanoramicView());
    this->m_panoramicUrl = url;

    // A failed load leaves a QWebEngineView showing nothing at all — the same
    // black rectangle as a working page that has not painted yet, with no hint
    // of which of the two it is. The panoramic view is a REMOTE page served by
    // an INAF host, so the ordinary reasons to see nothing are being off the
    // VPN, the service being down, or a wrong URL in the settings — all of them
    // things the user can act on, and none of them visible today.
    //
    // ONE persistent connection, not a fresh single-shot per call: loadFinished
    // belongs to the view, not to a particular load(), so changing the URL in
    // Settings while a load is still out would leave two handlers racing for
    // the same signal — the one that fired would report whichever URL its own
    // lambda had captured, which need not be the one that failed.
    if (!this->m_panoramicHandlerConnected) {
        this->m_panoramicHandlerConnected = true;
        QObject::connect(ui->view, &QWebEngineView::loadFinished, this, [this](bool ok) {
            // setHtml() below is itself a load, and reports back here. Let it
            // through without reacting, or a failure would redraw itself.
            if (ok) {
                this->m_panoramicShowingError = false;
                this->frameGalacticPlane();
                return;
            }
            if (this->m_panoramicShowingError) {
                this->m_panoramicShowingError = false;
                return;
            }
            this->m_panoramicShowingError = true;
            const QString html =
                    u"<html><body style='background:#12161c;color:#c9d3df;"
                    u"font-family:-apple-system,Segoe UI,sans-serif;padding:32px'>"
                    u"<h2 style='color:#e8a33d;margin:0 0 12px'>"
                    u"Panoramic view unavailable</h2>"
                    u"<p>Could not load <code>%1</code>.</p>"
                    u"<p>The panoramic view is served by a remote host: check that you "
                    u"are on a network that can reach it (some INAF services need the "
                    u"VPN), that the service is up, and that the URL under "
                    u"<b>Settings &rarr; Panoramic view</b> is the one you mean. The "
                    u"VLKB query on the right still works without it if you type the "
                    u"coordinates yourself.</p></body></html>"_s
                            .arg(this->m_panoramicUrl.toString().toHtmlEscaped());
            ui->view->setHtml(html, this->m_panoramicUrl);
            qWarning().noquote()
                    << u"[sky] panoramic view failed to load: %1"_s.arg(
                               this->m_panoramicUrl.toString());
        });
    }
    this->m_panoramicShowingError = false;
    ui->view->load(url);
}


void MainWindow::frameGalacticPlane()
{
    // The remote page opens on empty space: its own init() ends with
    //
    //   var mapBoundsInitialZoom = new OpenLayers.Bounds(135.337, 16000.0, 135.337, 20000.0);
    //   map.zoomToExtent(mapBoundsInitialZoom);
    //
    // and that rectangle is degenerate in x and lies entirely OUTSIDE the map,
    // which spans y -4398..0 — so the view lands nowhere and the user has to
    // hunt for the Galactic plane with the pan arrows before doing anything.
    // We cannot change the page, but we can tell it where to look once it has
    // loaded, through the same channel the selection controls already use.
    //
    // By default we FIT THE HEIGHT of the survey rather than pick a zoom level:
    // the strip is about 61 times wider than it is tall, so any fixed level is
    // right for exactly one window size — level 7 frames it thinly on a small
    // window and leaves half the pane empty on a large one. Fitting the height
    // centres the plane and fills the pane on both. A non-zero PanoramicView/zoom
    // in the settings overrides this with that level, for anyone who wants to
    // open somewhere specific.
    //
    // Written defensively: this runs inside a page we do not own, so it checks
    // that OpenLayers and the map are there and reports rather than throwing if
    // the page ever changes shape.
    const int zoom = this->settings ? this->settings->getPanoramicZoom() : 0;
    const QString js = uR"JS(
        (function () {
            try {
                if (typeof map === 'undefined' || !map || typeof OpenLayers === 'undefined')
                    return 'no-map';
                // The page is loaded at startup while the VLKB tab is still
                // hidden, so the map div has no usable size until it is shown:
                // OpenLayers caches that size and must be told to re-read it,
                // or every computation below is done against a zero-sized
                // viewport and the view lands nowhere.
                map.updateSize();
                var e = map.getMaxExtent();
                var size = map.getSize();
                if (!e || !size || !size.h || size.h < 50) return 'not-visible';
                var cx = (e.left + e.right) / 2, cy = (e.top + e.bottom) / 2;
                var fixed = %1;
                if (fixed > 0) {
                    map.setCenter(new OpenLayers.LonLat(cx, cy), fixed);
                    return 'ok';
                }
                var h = e.getHeight();
                var w = h * (size.w / size.h);      // same aspect as the pane
                map.zoomToExtent(new OpenLayers.Bounds(cx - w / 2, cy - h / 2,
                                                       cx + w / 2, cy + h / 2));
                return 'ok';
            } catch (err) { return 'error: ' + err; }
        })();
    )JS"_s.arg(zoom);
    // Deferred: called straight after show()/insertTab() the widget has not been
    // laid out yet, so the map would measure the size it is about to stop having.
    QTimer::singleShot(0, this, [this, js]() {
        if (!ui || !ui->view)
            return;
        ui->view->page()->runJavaScript(js, [](const QVariant &r) {
            const QString out = r.toString();
            if (out == u"not-visible"_s)
                return;   // hidden tab at startup; the reveal will do it
            if (out != u"ok"_s)
                qWarning().noquote()
                        << u"[sky] could not frame the Galactic plane: %1"_s.arg(out);
        });
    });
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

                         // The archive's batch API. One job for the whole
                         // selection, collected once — and it keeps running on
                         // the archive whether or not the window stays open.
                         QObject::connect(tree, &VLKBInventoryTree::batchRequested, this,
                                          [this](const QStringList &ids, const QString &pos,
                                                 const QStringList &names) {
                             QVector<BackendMcutoutItem> items;
                             items.reserve(ids.size());
                             for (const QString &id : ids)
                                 items.append({ id, pos });
                             auto *job = new McutoutJobDialog(
                                     items, names, this->settings->getVLKBUrl(),
                                     this->auth->bearerToken(AuthService::VLKB),
                                     this->settings->getBackendUrl(),
                                     this->settings->getBackendToken(), this);
                             QObject::connect(job, &McutoutJobDialog::cutoutReady, this,
                                              [this](const QString &backendPath) {
                                                  this->doOpenDataPath(backendPath);
                                              });
                             job->show();
                         });
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
                                                          // Tell the inventory dialog the wait is
                                                          // over, whichever way it ended: it is the
                                                          // window the user is looking at, and it
                                                          // has been saying "requesting…" since the
                                                          // double-click.
                                                          if (tree)
                                                              tree->noteCutoutFinished(displayName,
                                                                                       res.valid);
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

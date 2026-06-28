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


void MainWindow::queryVlkb()
{
    const double lon = ui->lineLon->text().toDouble();
    const double lat = ui->lineLat->text().toDouble();

    const bool useCircle = !ui->lineRadius->text().trimmed().isEmpty();
    const double radius  =  ui->lineRadius->text().toDouble();  // used when useCircle
    const double dl      =  ui->lineDLon->text().toDouble() * .5;
    const double db      =  ui->lineDLat->text().toDouble() * .5;

    if (ui->lineLon->text().isEmpty() || ui->lineLat->text().isEmpty()) {
        QMessageBox::information(this, u"VLKB"_s,
                                 u"Enter galactic longitude and latitude before querying."_s);
        return;
    }

    // Build the POS string that will later be forwarded to SODA.
    const QString posStr = useCircle
            ? u"CIRCLE %1 %2 %3"_s.arg(QString::number(lon < 0. ? lon + 180. : lon),
                                        QString::number(lat),
                                        QString::number(radius))
            : u"RANGE %1 %2 %3 %4"_s.arg(
                      QString::number(lon - dl < 0. ? lon - dl + 180. : lon - dl),
                      QString::number(lon + dl < 0. ? lon + dl + 180. : lon + dl),
                      QString::number(lat - db),
                      QString::number(lat + db));

    ui->btnQuery->setEnabled(false);

    auto *svc = new VlkbQueryService(this->settings->getVLKBUrl(), this->auth, this);

    // On success — show the inventory tree; svc lives until it emits.
    QObject::connect(svc, &VlkbQueryService::searchFinished, this,
                     [this, svc, posStr](const QByteArray &votable) {
                         ui->btnQuery->setEnabled(true);
                         svc->deleteLater();

                         // Use nullptr parent so the tree appears as an independent window.
                         auto *tree = new VLKBInventoryTree(votable, posStr, nullptr);
                         tree->setAttribute(Qt::WA_DeleteOnClose);
                         tree->show();
                         tree->raise();

                         // When user double-clicks a dataset, ask the backend to download
                         // the FITS so it lands on the backend filesystem, not the client.
                         QObject::connect(tree, &VLKBInventoryTree::cutoutRequested, this,
                                          [this, tree](const QString &ivoID, const QString &pos,
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
                                              QObject::connect(
                                                      watcher,
                                                      &QFutureWatcher<BackendVlkbFetchResult>::finished,
                                                      this,
                                                      [this, tree, displayName, type, watcher]() {
                                                          watcher->deleteLater();
                                                          const auto res = watcher->result();
                                                          if (!res.valid) {
                                                              QMessageBox::critical(
                                                                      this,
                                                                      u"VLKB cutout failed"_s,
                                                                      res.error);
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
                         ui->btnQuery->setEnabled(true);
                         svc->deleteLater();
                         QMessageBox::critical(this, u"VLKB query failed"_s, err);
                     });

    if (useCircle) {
        svc->searchCircle(lon, lat, radius);
    } else {
        svc->searchRange(lon, lat, dl, db);
    }
}

#include "DataHubWidget.h"
#include "ElideOnResize.h"
#include "app/BackendClient.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTimer>
#include <QtConcurrentRun>

#include <cmath>

using namespace Qt::StringLiterals;

// ── DataHubWidget: Exports (extracted from DataHubWidget.cpp) ──

namespace {

QString formatExportSize(qint64 bytes)
{
    if (bytes < 1024)         return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)  return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < qint64(1024) * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    return QStringLiteral("%1 GB").arg(bytes / (qint64(1024) * 1024 * 1024));
}

QString formatExportTime(const QString &iso8601)
{
    const QDateTime t = QDateTime::fromString(iso8601, Qt::ISODate);
    if (!t.isValid()) return iso8601;
    return t.toLocalTime().toString(u"yyyy-MM-dd hh:mm"_s);
}

}  // namespace

QFrame *DataHubWidget::buildWorkspaceExportsPanel()
{
    auto *card = VisivoTheme::makeCard(this);
    m_exportsCard = card;

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->addWidget(VisivoTheme::makeSectionLabel(
            u"Workspace Exports"_s, card));
    m_exportsCountLabel = new QLabel(QString(), card);
    m_exportsCountLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; "
                           "background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    header->addWidget(m_exportsCountLabel);
    header->addStretch(1);

    auto *cleanupBtn = new QPushButton(u"Clean up older than…"_s, card);
    cleanupBtn->setCursor(Qt::PointingHandCursor);
    cleanupBtn->setToolTip(
            u"Delete every workspace export whose mtime is older than the "
            u"chosen number of days. Useful to keep the workspace size in "
            u"check after a long analysis session."_s);
    QObject::connect(cleanupBtn, &QPushButton::clicked, this,
                     [this]() {
                         if (this->backendUrl.isEmpty()) return;
                         bool ok = false;
                         const int days = QInputDialog::getInt(
                                 this, u"Clean up Workspace Exports"_s,
                                 u"Delete exports older than (days):"_s,
                                 /*default*/ 7, /*min*/ 1, /*max*/ 3650,
                                 /*step*/ 1, &ok);
                         if (!ok) return;

                         // Re-list synchronously off the UI thread so the
                         // cleanup is always based on a fresh snapshot
                         // (avoids racing against the 4-s auto-refresh).
                         const QString url = this->backendUrl;
                         const QString tok = this->backendToken;
                         QPointer<DataHubWidget> guard(this);
                         auto *watcher = new QFutureWatcher<int>(this);
                         QObject::connect(watcher, &QFutureWatcher<int>::finished,
                                          this, [guard, watcher, days]() {
                                              const int deleted = watcher->result();
                                              watcher->deleteLater();
                                              if (!guard) return;
                                              QMessageBox::information(
                                                      guard, u"Workspace cleanup"_s,
                                                      u"Removed %1 export%2 older than %3 day%4."_s
                                                              .arg(deleted)
                                                              .arg(deleted == 1 ? "" : "s")
                                                              .arg(days)
                                                              .arg(days  == 1 ? "" : "s"));
                                              guard->refreshWorkspaceExports();
                                          });
                         watcher->setFuture(QtConcurrent::run([url, tok, days]() {
                             BackendClient c(url, tok);
                             const auto listing = c.listExports();
                             if (!listing.valid) return 0;
                             const QDateTime threshold =
                                     QDateTime::currentDateTimeUtc()
                                             .addDays(-days);
                             int deleted = 0;
                             for (const auto &e : listing.entries) {
                                 const QDateTime t = QDateTime::fromString(
                                         e.modifiedTime, Qt::ISODate);
                                 if (!t.isValid()) continue;
                                 if (t < threshold) {
                                     const auto r = c.deleteExport(e.filename);
                                     if (r.valid) ++deleted;
                                 }
                             }
                             return deleted;
                         }));
                     });
    header->addWidget(cleanupBtn);

    auto *refreshBtn = new QPushButton(u"Refresh"_s, card);
    refreshBtn->setCursor(Qt::PointingHandCursor);
    QObject::connect(refreshBtn, &QPushButton::clicked, this,
                     &DataHubWidget::refreshWorkspaceExports);
    header->addWidget(refreshBtn);
    vbox->addLayout(header);

    auto *scroll = new QScrollArea(card);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(u"QScrollArea { background: transparent; border: none; }"_s);

    auto *itemContainer = new QWidget(scroll);
    itemContainer->setStyleSheet(u"background: transparent;"_s);
    m_exportsItemsLayout = new QVBoxLayout(itemContainer);
    m_exportsItemsLayout->setContentsMargins(0, 0, 0, 0);
    m_exportsItemsLayout->setSpacing(6);

    m_exportsEmptyLabel = new QLabel(
            u"No exports yet. Use *Tools → Export Sub-Cube / Moment as FITS…* "
            u"from a cube viewer to populate this list."_s,
            itemContainer);
    m_exportsEmptyLabel->setWordWrap(true);
    m_exportsEmptyLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; "
                           "background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    m_exportsItemsLayout->addWidget(m_exportsEmptyLabel);
    m_exportsItemsLayout->addStretch(1);

    scroll->setWidget(itemContainer);
    scroll->setMinimumHeight(140);
    vbox->addWidget(scroll, 1);

    // First refresh deferred to next tick so the backend URL is already set
    // by MainWindow's wiring.
    QTimer::singleShot(0, this, &DataHubWidget::refreshWorkspaceExports);

    return card;
}


void DataHubWidget::refreshWorkspaceExports()
{
    if (!m_exportsItemsLayout) return;
    if (this->backendUrl.isEmpty()) return;   // nothing to call yet

    const QString url = this->backendUrl;
    const QString tok = this->backendToken;

    auto *watcher = new QFutureWatcher<BackendWorkspaceExportsList>(this);
    QPointer<DataHubWidget> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendWorkspaceExportsList>::finished, this,
                     [self, watcher]() {
                         const auto listing = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;

                         // Wipe every old row (keep stretch + empty label).
                         QVBoxLayout *lay = self->m_exportsItemsLayout;
                         if (!lay) return;
                         while (lay->count() > 0) {
                             QLayoutItem *li = lay->takeAt(0);
                             if (auto *w = li->widget()) {
                                 if (w == self->m_exportsEmptyLabel) {
                                     w->hide();
                                     // Re-attach later so we can show it
                                     // when the list comes back empty.
                                 } else {
                                     w->deleteLater();
                                 }
                             }
                             delete li;
                         }

                         if (!listing.valid) {
                             self->m_exportsCountLabel->setText(
                                     u"(failed: %1)"_s.arg(listing.error));
                             if (self->m_exportsEmptyLabel) {
                                 self->m_exportsEmptyLabel->show();
                                 lay->addWidget(self->m_exportsEmptyLabel);
                             }
                             lay->addStretch(1);
                             return;
                         }

                         if (listing.entries.isEmpty()) {
                             self->m_exportsCountLabel->setText(QString());
                         } else {
                             // Count + total disk usage so the user knows when
                             // the workspace is starting to fill up.
                             qint64 totalBytes = 0;
                             for (const auto &e : listing.entries)
                                 totalBytes += e.size;
                             self->m_exportsCountLabel->setText(
                                     u"(%1 · %2)"_s
                                             .arg(listing.entries.size())
                                             .arg(formatExportSize(totalBytes)));
                         }

                         if (listing.entries.isEmpty()) {
                             if (self->m_exportsEmptyLabel) {
                                 self->m_exportsEmptyLabel->show();
                                 lay->addWidget(self->m_exportsEmptyLabel);
                             }
                             lay->addStretch(1);
                             return;
                         }

                         for (const auto &entry : listing.entries) {
                             auto *row = new QFrame();
                             row->setStyleSheet(QStringLiteral(
                                     "QFrame {"
                                     "  background: %1;"
                                     "  border: 1px solid %2;"
                                     "  border-radius: 6px;"
                                     "}")
                                     .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                                          QLatin1StringView(VisivoTheme::kOutline())));
                             auto *h = new QHBoxLayout(row);
                             h->setContentsMargins(10, 6, 10, 6);
                             h->setSpacing(10);

                             auto *name = new QLabel(entry.filename, row);
                             name->setStyleSheet(
                                     QStringLiteral("color: %1; font-weight: 600; "
                                                    "background: transparent; border: none;")
                                             .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
                             name->setTextInteractionFlags(Qt::TextSelectableByMouse);
                             // A vlkb-cutout name is long enough to widen this
                             // row past the panel, and the horizontal scrollbar
                             // is off — so Delete ended up beyond the edge on
                             // EVERY row, not just the long one, because one
                             // wide row sets the container's width.
                             name->setMinimumWidth(1);
                             name->setSizePolicy(QSizePolicy::Ignored,
                                                 QSizePolicy::Preferred);
                             ElideOnResize::apply(name, entry.filename);
                             row->setToolTip(entry.filename);
                             h->addWidget(name, 1);

                             auto *meta = new QLabel(
                                     u"%1 · %2"_s
                                     .arg(formatExportSize(entry.size),
                                          formatExportTime(entry.modifiedTime)),
                                     row);
                             meta->setStyleSheet(
                                     QStringLiteral("color: %1; font-size: 10px; "
                                                    "background: transparent; border: none;")
                                             .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
                             h->addWidget(meta);

                             auto *openBtn     = new QPushButton(u"Open"_s, row);
                             auto *downloadBtn = new QPushButton(u"Download…"_s, row);
                             auto *deleteBtn   = new QPushButton(u"Delete"_s, row);
                             for (auto *b : { openBtn, downloadBtn, deleteBtn }) {
                                 b->setCursor(Qt::PointingHandCursor);
                                 b->setStyleSheet(QStringLiteral(
                                         "QPushButton {"
                                         " padding: 4px 10px; font-size: 11px;"
                                         " border: 1px solid %1; border-radius: 4px;"
                                         " background: transparent; color: %2;"
                                         "}"
                                         "QPushButton:hover { background: %3; }")
                                         .arg(QLatin1StringView(VisivoTheme::kOutline()),
                                              QLatin1StringView(VisivoTheme::kOnSurface()),
                                              QLatin1StringView(VisivoTheme::kSurfaceContainerHigh())));
                                 h->addWidget(b);
                             }

                             const QString abs = entry.absolutePath;
                             const QString fn  = entry.filename;
                             QObject::connect(openBtn, &QPushButton::clicked, self.data(),
                                              [self, abs]() {
                                                  if (!self) return;
                                                  emit self->openWorkspaceExportRequested(abs);
                                              });
                             QObject::connect(downloadBtn, &QPushButton::clicked, self.data(),
                                              [self, fn]() {
                                                  if (!self) return;
                                                  const QString defaultDir =
                                                          QStandardPaths::writableLocation(
                                                                  QStandardPaths::DocumentsLocation);
                                                  const QString suggested =
                                                          QDir(defaultDir.isEmpty()
                                                               ? QDir::homePath() : defaultDir)
                                                                  .filePath(fn);
                                                  const QString localPath = QFileDialog::getSaveFileName(
                                                          self, u"Download Workspace Export"_s,
                                                          suggested,
                                                          u"FITS files (*.fits *.fit);;All files (*)"_s);
                                                  if (localPath.isEmpty()) return;
                                                  const QString url2 = self->backendUrl;
                                                  const QString tok2 = self->backendToken;
                                                  QPointer<DataHubWidget> guard(self);
                                                  auto *dlWatcher = new QFutureWatcher<QString>(self);
                                                  QObject::connect(
                                                          dlWatcher, &QFutureWatcher<QString>::finished, self.data(),
                                                          [guard, dlWatcher, localPath]() {
                                                              const QString err = dlWatcher->result();
                                                              dlWatcher->deleteLater();
                                                              if (!guard) return;
                                                              if (err.isEmpty()) {
                                                                  QMessageBox::information(
                                                                          guard, u"Download"_s,
                                                                          u"Saved to:\n%1"_s.arg(localPath));
                                                              } else {
                                                                  QMessageBox::warning(
                                                                          guard, u"Download"_s,
                                                                          u"Download failed:\n%1"_s.arg(err));
                                                              }
                                                          });
                                                  dlWatcher->setFuture(QtConcurrent::run(
                                                          [url2, tok2, fn, localPath]() {
                                                              BackendClient c(url2, tok2);
                                                              QString err;
                                                              c.downloadExport(fn, localPath, err);
                                                              return err;
                                                          }));
                                              });
                             QObject::connect(deleteBtn, &QPushButton::clicked, self.data(),
                                              [self, fn]() {
                                                  if (!self) return;
                                                  const auto reply = QMessageBox::question(
                                                          self, u"Delete export"_s,
                                                          u"Remove '%1' from the Workspace Exports?\n"
                                                          u"This cannot be undone."_s.arg(fn));
                                                  if (reply != QMessageBox::Yes) return;
                                                  const QString url2 = self->backendUrl;
                                                  const QString tok2 = self->backendToken;
                                                  QPointer<DataHubWidget> guard(self);
                                                  auto *delWatcher = new QFutureWatcher<BackendWorkspaceDeleteResult>(self);
                                                  QObject::connect(
                                                          delWatcher, &QFutureWatcher<BackendWorkspaceDeleteResult>::finished,
                                                          self.data(), [guard, delWatcher]() {
                                                              const auto r = delWatcher->result();
                                                              delWatcher->deleteLater();
                                                              if (!guard) return;
                                                              if (!r.valid) {
                                                                  QMessageBox::warning(
                                                                          guard, u"Delete export"_s,
                                                                          u"Could not delete:\n%1"_s.arg(r.error));
                                                              }
                                                              guard->refreshWorkspaceExports();
                                                          });
                                                  delWatcher->setFuture(QtConcurrent::run(
                                                          [url2, tok2, fn]() {
                                                              BackendClient c(url2, tok2);
                                                              return c.deleteExport(fn);
                                                          }));
                                              });

                             lay->addWidget(row);
                         }
                         lay->addStretch(1);
                     });
    watcher->setFuture(QtConcurrent::run([url, tok]() {
        BackendClient c(url, tok);
        return c.listExports();
    }));
}

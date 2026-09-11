#include "DataHubWidget.h"
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

// ── DataHubWidget: Status (extracted from DataHubWidget.cpp) ──

namespace {

QString formatDurationShort(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return u"-"_s;
    if (seconds < 1.0)   return QStringLiteral("%1ms").arg(int(seconds * 1000.0));
    if (seconds < 60.0)  return QStringLiteral("%1s").arg(seconds, 0, 'f', seconds < 10.0 ? 1 : 0);
    if (seconds < 3600.0) {
        const int m = int(seconds / 60.0);
        const int s = int(seconds) % 60;
        return s == 0 ? QStringLiteral("%1m").arg(m)
                      : QStringLiteral("%1m %2s").arg(m).arg(s);
    }
    return QStringLiteral("%1h").arg(seconds / 3600.0, 0, 'f', 1);
}

QString formatAgoShort(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return u""_s;
    if (seconds < 5.0)    return u"just now"_s;
    if (seconds < 60.0)   return QStringLiteral("%1s ago").arg(int(seconds));
    if (seconds < 3600.0) return QStringLiteral("%1m ago").arg(int(seconds / 60.0));
    if (seconds < 86400.0) return QStringLiteral("%1h ago").arg(int(seconds / 3600.0));
    return QStringLiteral("%1d ago").arg(int(seconds / 86400.0));
}

QString statusGlyph(const QString &status)
{
    if (status == u"failed"_s)    return QStringLiteral("✗");
    if (status == u"cancelled"_s) return QStringLiteral("◌");
    return QStringLiteral("✓");
}

QString statusColor(const QString &status)
{
    if (status == u"failed"_s)    return u"#EF5350"_s;
    if (status == u"cancelled"_s) return u"#FFA726"_s;
    return u"#4CAF50"_s; // done
}

}  // namespace

QFrame *DataHubWidget::buildSystemInsightsPanel()
{
    auto *card = VisivoTheme::makeCard(this, true /* elevated */);
    card->setMinimumWidth(220);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(14);

    vbox->addWidget(VisivoTheme::makeSectionLabel(u"System Insights"_s, card));

    // ── Live activity: running tasks count + small caption ───────────────────
    // Replaces the old static "Workers/Sessions/Task Registry" trio that didn't
    // change during a session. Now shows what the backend is *currently doing*.
    {
        auto *metricBox = new QVBoxLayout;
        metricBox->setSpacing(2);

        auto *lbl = new QLabel(u"Active operations"_s, card);
        lbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; letter-spacing: 0.1em; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        metricBox->addWidget(lbl);

        this->labelRunningTasks = new QLabel(u"-"_s, card);
        QFont f(QStringLiteral("Lato"));
        f.setPixelSize(32);
        f.setWeight(QFont::Black);
        this->labelRunningTasks->setFont(f);
        this->labelRunningTasks->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        metricBox->addWidget(this->labelRunningTasks);

        // Sub-caption that switches between "idle" and "in flight…" depending
        // on running count. Stays a fixed-height label so the panel doesn't
        // jump as state changes.
        this->labelRunningCaption = new QLabel(u"Idle"_s, card);
        this->labelRunningCaption->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        metricBox->addWidget(this->labelRunningCaption);
        vbox->addLayout(metricBox);
    }

    // ── Recent operations list ───────────────────────────────────────────────
    // Up to N most recently finished tasks, each rendered as
    // "<icon> <operation>   <duration> · <age>". Populated in refreshStatus
    // from health.recentTasks. Hidden entirely when the list is empty.
    {
        auto *recentHdr = new QLabel(u"RECENT"_s, card);
        recentHdr->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; font-weight: 700; "
                                "letter-spacing: 0.18em; "
                                "background: transparent; margin-top: 4px;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        recentHdr->setObjectName(u"recentTasksHeader"_s);
        vbox->addWidget(recentHdr);
        this->labelRecentHeader = recentHdr;

        this->recentTasksContainer = new QWidget(card);
        this->recentTasksContainer->setStyleSheet(u"background: transparent;"_s);
        auto *recentLayout = new QVBoxLayout(this->recentTasksContainer);
        recentLayout->setContentsMargins(0, 0, 0, 0);
        recentLayout->setSpacing(4);
        vbox->addWidget(this->recentTasksContainer);

        // Empty-state placeholder shown when no recent tasks are reported.
        this->labelRecentEmpty = new QLabel(
                u"No operations yet. Open a dataset to start."_s, card);
        this->labelRecentEmpty->setWordWrap(true);
        this->labelRecentEmpty->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; font-style: italic; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        vbox->addWidget(this->labelRecentEmpty);
    }

    // ── Separator + Backend capacity footer ──────────────────────────────────
    {
        auto *sep = new QFrame(card);
        sep->setFixedHeight(1);
        sep->setStyleSheet(
                QStringLiteral("background-color: rgba(4, 138, 191, 0.18); border: none;"));
        vbox->addWidget(sep);

        this->labelCapacity = new QLabel(u"Backend capacity: -"_s, card);
        this->labelCapacity->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        vbox->addWidget(this->labelCapacity);
    }

    vbox->addStretch(1);
    return card;
}


void DataHubWidget::refreshStatus()
{
    // Don't refresh status while a modal dialog is up — see the same guard
    // in MainWindow::pollBackendDiagnosticsOnce(). Updating the badge's
    // stylesheet here causes a parent-window event burst on macOS that
    // dismisses native save dialogs spawned by secondary windows.
    if (QApplication::activeModalWidget() != nullptr) return;
    if (this->backendUrl.isEmpty()) {
        if (this->labelStatusBadge) {
            this->labelStatusBadge->setText(u"● Not configured"_s);
            this->labelStatusBadge->setStyleSheet(
                    QStringLiteral(
                            "QLabel {"
                            "  background-color: %1; color: %2;"
                            "  font-size: 10px; font-weight: 700;"
                            "  letter-spacing: 0.12em; padding: 3px 10px;"
                            "  border-radius: 10px; border: none;"
                            "}")
                            .arg(QLatin1StringView(VisivoTheme::kWarningContainer()),
                                 QLatin1StringView(VisivoTheme::kOnWarningContainer())));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    u"No backend URL configured. Set it in Edit → Settings."_s);
        }
        return;
    }

    if (this->btnCheckConnection) {
        this->btnCheckConnection->setEnabled(false);
        this->btnCheckConnection->setText(u"Checking…"_s);
    }
    if (this->labelStatusDetail) {
        this->labelStatusDetail->setText(u"Contacting backend…"_s);
    }

    BackendClient client(this->backendUrl, this->backendToken);
    const BackendHealthResult health = client.health();

    if (health.ok) {
        if (this->labelStatusBadge) {
            this->labelStatusBadge->setText(u"● Connected"_s);
            this->labelStatusBadge->setStyleSheet(
                    QStringLiteral(
                            "QLabel {"
                            "  background-color: %1; color: %2;"
                            "  font-size: 10px; font-weight: 700;"
                            "  letter-spacing: 0.12em; padding: 3px 10px;"
                            "  border-radius: 10px; border: none;"
                            "}")
                            .arg(QLatin1StringView(VisivoTheme::kTertiaryContainer()),
                                 QLatin1StringView(VisivoTheme::kOnTertiaryFixed())));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    QStringLiteral("Backend reachable - workers: %1, sessions: %2")
                            .arg(health.workers)
                            .arg(health.activeSessions));
        }
        // We're in the `health.ok` branch — every value is authoritative.
        // Populate the live activity readouts from the new health snapshot.
        this->updateSystemInsights(health);

        // Same cadence keeps the Workspace Exports list in sync — new
        // artefacts pushed by cube viewers (Tools → Export …) appear
        // here without the user having to click Refresh.
        this->refreshWorkspaceExports();

        // Bottom status bar: counts that aren't already in the top toolbar
        // (dataset count + currently-running tasks + sanity).
        const int datasetCount = this->recentMgr ? this->recentMgr->datasets().size() : 0;
        if (this->statusBarHealth) {
            this->statusBarHealth->setText(
                    datasetCount == 1
                            ? QStringLiteral("1 dataset")
                            : QStringLiteral("%1 datasets").arg(datasetCount));
            this->statusBarHealth->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarWorkers) {
            const int n = health.runningTasks;
            this->statusBarWorkers->setText(
                    n == 0 ? u"no jobs running"_s
                           : (n == 1 ? u"1 job running"_s
                                     : QStringLiteral("%1 jobs running").arg(n)));
            this->statusBarWorkers->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(
                                    n > 0 ? VisivoTheme::kRunning()
                                          : VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarSanity) {
            this->statusBarSanity->setText(u"sanity ok"_s);
            this->statusBarSanity->setVisible(true);
        }

        qDebug().noquote()
                << QStringLiteral("[DataHub] backend reachable, workers=%1 running=%2 sessions=%3")
                           .arg(health.workers)
                           .arg(health.runningTasks)
                           .arg(health.activeSessions);
    } else {
        if (this->labelStatusBadge) {
            this->labelStatusBadge->setText(u"● Unreachable"_s);
            this->labelStatusBadge->setStyleSheet(
                    QStringLiteral(
                            "QLabel {"
                            "  background-color: %1; color: %2;"
                            "  font-size: 10px; font-weight: 700;"
                            "  letter-spacing: 0.12em; padding: 3px 10px;"
                            "  border-radius: 10px; border: none;"
                            "}")
                            .arg(QLatin1StringView(VisivoTheme::kErrorContainer()),
                                 QLatin1StringView(VisivoTheme::kOnErrorContainer())));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    health.error.isEmpty()
                            ? u"No response from backend."_s
                            : health.error);
        }
        // Reset live activity readouts to "unknown" placeholder state.
        if (this->labelRunningTasks)   this->labelRunningTasks->setText(u"-"_s);
        if (this->labelRunningCaption) this->labelRunningCaption->setText(u"Backend unreachable"_s);
        if (this->labelCapacity)       this->labelCapacity->setText(u"Backend capacity: -"_s);
        this->clearRecentTasksUi();

        // Bottom status bar: dataset count is still meaningful even when the
        // backend is unreachable; the running-jobs cell becomes "-" and the
        // sanity badge hides.
        const int datasetCount = this->recentMgr ? this->recentMgr->datasets().size() : 0;
        if (this->statusBarHealth) {
            this->statusBarHealth->setText(
                    datasetCount == 1 ? QStringLiteral("1 dataset")
                                       : QStringLiteral("%1 datasets").arg(datasetCount));
            this->statusBarHealth->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarWorkers) {
            this->statusBarWorkers->setText(u"jobs -"_s);
            this->statusBarWorkers->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarSanity) this->statusBarSanity->setVisible(false);
        qDebug().noquote()
                << QStringLiteral("[DataHub] backend unreachable -%1").arg(health.error);
    }

    if (this->btnCheckConnection) {
        this->btnCheckConnection->setEnabled(true);
        this->btnCheckConnection->setText(u"Check Connection"_s);
    }
}


void DataHubWidget::clearRecentTasksUi()
{
    if (!this->recentTasksContainer) return;
    auto *layout = this->recentTasksContainer->layout();
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (auto *w = item->widget()) w->deleteLater();
        delete item;
    }
    if (this->labelRecentEmpty)  this->labelRecentEmpty->setVisible(true);
    if (this->labelRecentHeader) this->labelRecentHeader->setVisible(false);
    this->recentTasksContainer->setVisible(false);
}


void DataHubWidget::updateSystemInsights(const BackendHealthResult &health)
{
    // Running counter + caption switching between "Idle" and "in flight…".
    if (this->labelRunningTasks) {
        this->labelRunningTasks->setText(QString::number(health.runningTasks));
    }
    if (this->labelRunningCaption) {
        this->labelRunningCaption->setText(
                health.runningTasks == 0
                        ? u"Idle, backend ready."_s
                        : QStringLiteral("%1 operation%2 in flight…")
                                  .arg(health.runningTasks)
                                  .arg(health.runningTasks == 1 ? u""_s : u"s"_s));
    }

    // Backend capacity footer (workers + active sessions when > 0).
    if (this->labelCapacity) {
        QString text = QStringLiteral("Backend capacity: %1 worker%2")
                               .arg(health.workers)
                               .arg(health.workers == 1 ? u""_s : u"s"_s);
        if (health.activeSessions > 0) {
            text += QStringLiteral(" · %1 session%2")
                            .arg(health.activeSessions)
                            .arg(health.activeSessions == 1 ? u""_s : u"s"_s);
        }
        this->labelCapacity->setText(text);
    }

    // Recent operations list — rebuild from scratch each refresh so order
    // and ages stay accurate. Backend caps the list at 5 entries.
    if (!this->recentTasksContainer) return;
    auto *layout = qobject_cast<QVBoxLayout *>(this->recentTasksContainer->layout());
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (auto *w = item->widget()) w->deleteLater();
        delete item;
    }

    const bool hasItems = !health.recentTasks.empty();
    if (this->labelRecentEmpty)  this->labelRecentEmpty->setVisible(!hasItems);
    if (this->labelRecentHeader) this->labelRecentHeader->setVisible(hasItems);
    this->recentTasksContainer->setVisible(hasItems);

    for (const auto &task : health.recentTasks) {
        auto *row = new QWidget(this->recentTasksContainer);
        row->setStyleSheet(u"background: transparent;"_s);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto *icon = new QLabel(statusGlyph(task.status), row);
        icon->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12px; font-weight: 700; "
                                "background: transparent;").arg(statusColor(task.status)));
        icon->setFixedWidth(14);
        rowLayout->addWidget(icon);

        QString opLabel = task.operation;
        if (task.cacheHit) {
            opLabel += QStringLiteral(" (cache)");
        }
        auto *op = new QLabel(opLabel, row);
        op->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        rowLayout->addWidget(op, 1);

        const QString meta = QStringLiteral("%1 · %2")
                                     .arg(formatDurationShort(task.durationSeconds),
                                          formatAgoShort(task.ageSeconds));
        auto *metaLbl = new QLabel(meta, row);
        metaLbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        rowLayout->addWidget(metaLbl);

        layout->addWidget(row);
    }
}

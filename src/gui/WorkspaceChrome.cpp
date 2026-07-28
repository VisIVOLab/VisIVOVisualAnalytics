#include "WorkspaceChrome.h"

#include "MainWindow.h"
#include "DiagnosticsWindow.h"
#include "InspectorPanel.h"
#include "SessionDataTree.h"
#include "app/ProductRegistry.h"
#include "app/BackendClient.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "theme/VisivoTheme.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QShortcut>
#include <QStatusBar>
#include <QToolButton>
#include <QUrl>
#include <QtConcurrentRun>

#include <algorithm>

// Backend health snapshot plus the round-trip latency measured in the worker.
struct WorkspaceChrome::HealthSnapshot
{
    bool ok = false;
    qint64 ms = 0;
    int runningTasks = 0;
    int registryEntries = 0;
    int cacheEntries = 0;
    int cacheCapacity = 0;
};

namespace {

using namespace Qt::StringLiterals;

// Apply the neutral monospace rail-slot style using the current theme tokens.
void styleRailSlot(QLabel *lbl)
{
    if (!lbl)
        return;
    lbl->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  color: %1;"
                    "  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "  font-size: 10px; padding: 0 8px;"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
}

// Monospace fixed-slot label for the status rail.
QLabel *makeRailSlot(QWidget *parent)
{
    auto *lbl = new QLabel(u"—"_s, parent);
    styleRailSlot(lbl);
    return lbl;
}

void styleJobsButton(QToolButton *btn)
{
    if (!btn)
        return;
    btn->setStyleSheet(
            QStringLiteral(
                    "QToolButton {"
                    "  background: transparent; color: %1; border: none;"
                    "  font-size: 10px; font-weight: 600; padding: 0 8px;"
                    "}"
                    "QToolButton:hover { color: %2; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                         QLatin1StringView(VisivoTheme::kPrimary())));
}

void styleRailSep(QLabel *lbl)
{
    if (!lbl)
        return;
    lbl->setStyleSheet(QStringLiteral("QLabel { color: %1; }")
                               .arg(QLatin1StringView(VisivoTheme::kOutline())));
}

QLabel *makeRailSep(QWidget *parent)
{
    auto *lbl = new QLabel(u"│"_s, parent);
    lbl->setObjectName(u"wsRailSep"_s);   // so restyleForTheme() can find them
    styleRailSep(lbl);
    return lbl;
}

// Restyle the command-bar remote chip, keeping the tag-chip look but with a
// theme-token foreground colour that reflects backend state.
void styleChip(QLabel *chip, const char *fgToken)
{
    if (!chip)
        return;
    chip->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  background-color: %1; color: %2;"
                    "  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "  font-size: 10px; font-weight: 600;"
                    "  padding: 3px 8px; border-radius: 4px; border: 1px solid %3;"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                         QLatin1StringView(fgToken),
                         QLatin1StringView(VisivoTheme::kOutline())));
}

} // namespace

WorkspaceChrome::WorkspaceChrome(QMainWindow *window, QString backendUrl,
                                 QString backendToken, QString sessionId,
                                 QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_url(std::move(backendUrl))
    , m_token(std::move(backendToken))
    , m_session(std::move(sessionId))
{
    m_timer.setInterval(2500);   // gentle cadence; one request per window
    connect(&m_timer, &QTimer::timeout, this, &WorkspaceChrome::poll);
    m_watcher = new QFutureWatcher<HealthSnapshot>(this);
    connect(m_watcher, &QFutureWatcher<HealthSnapshot>::finished, this,
            &WorkspaceChrome::onPollFinished);

    // The rail widgets bake theme-token colours at build time (like the rest of
    // the viewer chrome) and don't follow the global QSS, so re-style them when
    // the effective theme flips.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](EffectiveTheme) { restyleForTheme(); });
}

void WorkspaceChrome::restyleForTheme()
{
    styleRailSlot(m_queueSlot);
    styleRailSlot(m_cacheSlot);
    styleRailSlot(m_sessionSlot);
    styleRailSlot(m_healthSlot);   // health colour is re-applied by the next poll
    styleJobsButton(m_jobsBtn);
    if (m_window && m_window->statusBar()) {
        const auto seps = m_window->statusBar()->findChildren<QLabel *>(u"wsRailSep"_s);
        for (QLabel *sep : seps)
            styleRailSep(sep);
    }
    // Refresh the chip + health colours immediately against the new theme.
    poll();
}

MainWindow *WorkspaceChrome::mainWindow() const
{
    if (m_mainWindow)
        return m_mainWindow;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *mw = qobject_cast<MainWindow *>(w)) {
            m_mainWindow = mw;
            return mw;
        }
    }
    return nullptr;
}

void WorkspaceChrome::attachCommandBar(const VisivoTheme::ViewerToolbarWidgets &w)
{
    m_remoteChip = w.remoteChip;

    // ⌘K — open the application command palette (cross-window, queued).
    if (w.command) {
        connect(w.command, &QToolButton::clicked, this, [this]() {
            if (auto *mw = mainWindow())
                QMetaObject::invokeMethod(mw, "openCommandPalette", Qt::QueuedConnection);
        });
    }

    // ▼ — recent-dataset switcher; menu rebuilt fresh each time it opens.
    if (w.datasetSwitch) {
        auto *menu = new QMenu(w.datasetSwitch);
        w.datasetSwitch->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
            menu->clear();
            MainWindow *mw = mainWindow();
            RecentDatasetsManager *mgr = mw ? mw->recentDatasetsManager() : nullptr;
            const QList<RecentDataset> list = mgr ? mgr->datasets() : QList<RecentDataset>();
            if (list.isEmpty()) {
                QAction *a = menu->addAction(tr("No recent datasets"));
                a->setEnabled(false);
                return;
            }
            for (const RecentDataset &ds : list) {
                const QString label = ds.type.isEmpty()
                        ? ds.displayName
                        : QStringLiteral("%1   ·   %2").arg(ds.displayName, ds.type.toUpper());
                QAction *a = menu->addAction(label);
                connect(a, &QAction::triggered, this, [this, ds]() {
                    if (auto *m = mainWindow())
                        m->reopenDataset(ds);
                });
            }
        });
    }

    // ◐ — toggle dark / light theme.
    if (w.theme) {
        connect(w.theme, &QToolButton::clicked, this, [this]() {
            if (auto *mw = mainWindow())
                mw->toggleColorScheme();
        });
    }
}

void WorkspaceChrome::installStatusRail()
{
    if (!m_window)
        return;
    QStatusBar *sb = m_window->statusBar();
    if (!sb)
        return;

    // Fixed slots are appended to the RIGHT of the existing permanent widgets
    // (cursor readout, WCS/sanity labels) — additive, nothing removed.
    m_healthSlot = makeRailSlot(sb);
    m_queueSlot = makeRailSlot(sb);
    m_cacheSlot = makeRailSlot(sb);
    m_sessionSlot = makeRailSlot(sb);

    sb->addPermanentWidget(makeRailSep(sb));
    sb->addPermanentWidget(m_healthSlot);
    sb->addPermanentWidget(makeRailSep(sb));
    sb->addPermanentWidget(m_queueSlot);
    sb->addPermanentWidget(makeRailSep(sb));
    sb->addPermanentWidget(m_cacheSlot);
    sb->addPermanentWidget(makeRailSep(sb));
    sb->addPermanentWidget(m_sessionSlot);

    // ⌥J — jobs / diagnostics panel (button + global shortcut).
    auto *jobsBtn = new QToolButton(sb);
    jobsBtn->setText(u"Jobs ⌥J"_s);
    jobsBtn->setCursor(Qt::PointingHandCursor);
    jobsBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    styleJobsButton(jobsBtn);
    m_jobsBtn = jobsBtn;
    connect(jobsBtn, &QToolButton::clicked, this,
            []() { DiagnosticsWindow::showSingleton(); });
    sb->addPermanentWidget(makeRailSep(sb));
    sb->addPermanentWidget(jobsBtn);

    auto *sc = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_J), m_window);
    connect(sc, &QShortcut::activated, this,
            []() { DiagnosticsWindow::showSingleton(); });

    // Session slot never changes after construction.
    if (m_sessionSlot) {
        const QString s = m_session.isEmpty()
                ? QStringLiteral("session —")
                : QStringLiteral("session %1…").arg(m_session.left(8));
        m_sessionSlot->setText(s);
    }
}

void WorkspaceChrome::installSessionDock(const QString &datasetKey,
                                         const QString &datasetLabel,
                                         const QString &datasetKind)
{
    if (!m_window || m_sessionDock)
        return;
    m_window->setDockNestingEnabled(true);   // no-op if already enabled (cube)

    m_sessionDock = new QDockWidget(QObject::tr("Session Data"), m_window);
    m_sessionDock->setObjectName(u"dockSessionData"_s);
    m_sessionDock->setFeatures(QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable);
    // Left column container: product tree on top, LAYERS block below (design #1d).
    auto *container = new QWidget(m_sessionDock);
    container->setMinimumWidth(220);
    container->setMaximumWidth(320);
    auto *vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_sessionTree = new SessionDataTree(container);
    vbox->addWidget(m_sessionTree, 1);

    // LAYERS header + host for the view/rendering controls (filled by the window
    // via addSessionLayers(); hidden until something is added).
    m_sessionLayersHeader = new QLabel(QObject::tr("LAYERS"), container);
    m_sessionLayersHeader->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-size:10px; font-weight:700;"
                           " letter-spacing:0.12em; padding:8px 10px 4px 10px; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    m_sessionLayersHeader->hide();
    vbox->addWidget(m_sessionLayersHeader);

    auto *layersScroll = new QScrollArea(container);
    layersScroll->setWidgetResizable(true);
    layersScroll->setFrameShape(QFrame::NoFrame);
    layersScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *layersInner = new QWidget(layersScroll);
    m_sessionLayersLayout = new QVBoxLayout(layersInner);
    m_sessionLayersLayout->setContentsMargins(0, 0, 0, 0);
    m_sessionLayersLayout->setSpacing(0);
    m_sessionLayersLayout->addStretch(1);
    layersScroll->setWidget(layersInner);
    vbox->addWidget(layersScroll, 2);

    m_sessionDock->setWidget(container);
    m_window->addDockWidget(Qt::LeftDockWidgetArea, m_sessionDock);
    m_sessionTree->setCurrentDataset(datasetKey, datasetLabel, datasetKind);
}

void WorkspaceChrome::addSessionLayers(QWidget *w)
{
    if (!w || !m_sessionLayersLayout)
        return;
    // Remove from its current layout (the hidden holder) first, then insert here.
    // insertWidget() reparents; an explicit removeWidget avoids leaving a stale
    // layout item behind in the holder.
    if (auto *p = w->parentWidget())
        if (auto *l = p->layout())
            l->removeWidget(w);
    m_sessionLayersLayout->insertWidget(m_sessionLayersLayout->count() - 1, w);
    w->show();
    if (m_sessionLayersHeader)
        m_sessionLayersHeader->show();
}

InspectorPanel *WorkspaceChrome::installInspectorDock()
{
    if (!m_window)
        return nullptr;
    if (m_inspector)
        return m_inspector;
    m_window->setDockNestingEnabled(true);

    m_inspectorDock = new QDockWidget(QObject::tr("Inspector"), m_window);
    m_inspectorDock->setObjectName(u"dockInspector"_s);
    m_inspectorDock->setFeatures(QDockWidget::DockWidgetMovable
                                 | QDockWidget::DockWidgetFloatable
                                 | QDockWidget::DockWidgetClosable);
    m_inspector = new InspectorPanel(m_inspectorDock);
    // Fixed right column (design #1d: 320 px) so the viewports keep the centre.
    m_inspector->setMinimumWidth(320);
    m_inspector->setMaximumWidth(340);
    m_inspectorDock->setWidget(m_inspector);
    m_window->addDockWidget(Qt::RightDockWidgetArea, m_inspectorDock);

    // Cloud chip is driven by the same health poll as the command-bar chip.
    m_cloudChip = m_inspector->cloudChip();

    // Provenance follows the product selected in the session tree.
    if (m_sessionTree) {
        connect(m_sessionTree, &SessionDataTree::productSelected, m_inspector,
                [this](const QString &productId) {
                    if (!m_inspector)
                        return;
                    if (productId.isEmpty()) {
                        m_inspector->clearProvenance();
                        return;
                    }
                    const AnalysisProduct p = ProductRegistry::instance().product(productId);
                    m_inspector->showProvenance(p.label.isEmpty() ? p.kind : p.label, p.params);
                });
    }
    return m_inspector;
}

void WorkspaceChrome::start()
{
    if (m_url.isEmpty())
        return;          // local/offline dataset — nothing to poll
    poll();              // immediate first fetch
    m_timer.start();
}

void WorkspaceChrome::poll()
{
    if (m_url.isEmpty() || m_watcher->isRunning())
        return;
    const QString url = m_url;
    const QString token = m_token;
    m_watcher->setFuture(QtConcurrent::run([url, token]() -> HealthSnapshot {
        BackendClient client(url, token);
        QElapsedTimer t;
        t.start();
        const BackendHealthResult h = client.health();
        HealthSnapshot s;
        s.ok = h.ok;
        s.ms = t.elapsed();
        s.runningTasks = h.runningTasks;
        s.registryEntries = h.taskRegistryEntries;
        s.cacheEntries = h.productCacheEntries;
        s.cacheCapacity = h.productCacheCapacity;
        return s;
    }));
}

void WorkspaceChrome::onPollFinished()
{
    const HealthSnapshot s = m_watcher->result();

    const QString host = QUrl(m_url).host();
    const QString hostLabel = host.isEmpty() ? QStringLiteral("backend") : host;

    // Command-bar chip + inspector cloud chip: "host · N running" (or offline).
    // Leading "●" status dot inherits the chip's foreground colour, so it reads
    // green (idle) / teal (running) / red (offline) alongside the host text.
    const auto updateChip = [&](QLabel *chip) {
        if (!chip)
            return;
        if (!s.ok) {
            chip->setText(QStringLiteral("● %1 · offline").arg(hostLabel));
            styleChip(chip, VisivoTheme::kError());
        } else if (s.runningTasks > 0) {
            chip->setText(QStringLiteral("● %1 · %2 running").arg(hostLabel).arg(s.runningTasks));
            styleChip(chip, VisivoTheme::kRunning());
        } else {
            chip->setText(QStringLiteral("● %1").arg(hostLabel));
            styleChip(chip, VisivoTheme::kSuccess());
        }
    };
    updateChip(m_remoteChip);
    updateChip(m_cloudChip);

    // Status rail.
    if (m_healthSlot) {
        m_healthSlot->setText(s.ok ? QStringLiteral("backend ok · %1 ms").arg(s.ms)
                                   : QStringLiteral("backend down"));
        m_healthSlot->setStyleSheet(
                QStringLiteral(
                        "QLabel { color: %1; font-family: 'JetBrains Mono','SF Mono',Menlo,"
                        "monospace; font-size: 10px; padding: 0 8px; }")
                        .arg(QLatin1StringView(s.ok ? VisivoTheme::kSuccess()
                                                    : VisivoTheme::kError())));
    }
    if (m_queueSlot) {
        const int queued = std::max(0, s.registryEntries - s.runningTasks);
        m_queueSlot->setText(
                QStringLiteral("%1 running · %2 queued").arg(s.runningTasks).arg(queued));
    }
    if (m_cacheSlot) {
        m_cacheSlot->setText(
                QStringLiteral("cache %1/%2").arg(s.cacheEntries).arg(s.cacheCapacity));
    }
}

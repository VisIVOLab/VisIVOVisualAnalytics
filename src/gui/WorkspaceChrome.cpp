#include "WorkspaceChrome.h"

#include "MainWindow.h"
#include "DiagnosticsWindow.h"
#include "DockRail.h"
#include "AssistantPanel.h"
#include "InspectorPanel.h"
#include "SessionDataTree.h"
#include "app/ProductRegistry.h"
#include "app/BackendClient.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "theme/VisivoTheme.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QHBoxLayout>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
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
    int queuedTasks = 0;
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
    auto *lbl = new QLabel(u"-"_s, parent);
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

WorkspaceChrome *WorkspaceChrome::installCommandBar(
        QMainWindow *window, const VisivoTheme::ViewerToolbarSpec &spec,
        const QString &backendUrl, const QString &backendToken, const QString &sessionId,
        VisivoTheme::ViewerToolbarWidgets *out)
{
    if (!window)
        return nullptr;

    VisivoTheme::ViewerToolbarWidgets widgets;
    QWidget *bar = VisivoTheme::makeViewerToolbar(spec, &widgets, window);

    auto *host = new QToolBar(window);
    host->setObjectName(QStringLiteral("viewerToolbarHost"));
    host->setMovable(false);
    host->setFloatable(false);
    host->setContextMenuPolicy(Qt::PreventContextMenu);
    host->setStyleSheet(QStringLiteral("QToolBar#viewerToolbarHost {"
                                       "  background-color: transparent;"
                                       "  border: none;"
                                       "  padding: 6px 10px 4px 10px;"
                                       "  spacing: 0px;"
                                       "}"));
    host->addWidget(bar);
    window->addToolBar(Qt::TopToolBarArea, host);

    auto *chrome = new WorkspaceChrome(window, backendUrl, backendToken, sessionId, window);
    chrome->attachCommandBar(widgets);
    if (out)
        *out = widgets;
    return chrome;
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

    // ⌥J — the diagnostics log (button + global shortcut). Labelled "Logs"
    // because that is what it opens: a filterable table of log records. It used
    // to say "Jobs", which promised the running/queued task list that actually
    // lives in the queue slot a few widgets to the left.
    auto *jobsBtn = new QToolButton(sb);
    jobsBtn->setText(u"Logs ⌥J"_s);
    jobsBtn->setToolTip(u"Open the diagnostics log (⌥J)"_s);
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

    refreshSessionSlot();
}

void WorkspaceChrome::setSessionId(const QString &sessionId)
{
    if (m_session == sessionId)
        return;
    m_session = sessionId;
    refreshSessionSlot();
}

void WorkspaceChrome::refreshSessionSlot()
{
    if (!m_sessionSlot)
        return;
    m_sessionSlot->setText(m_session.isEmpty()
                                   ? QStringLiteral("session -")
                                   : QStringLiteral("session %1…").arg(m_session.left(8)));
}

void WorkspaceChrome::installSessionDock(const QString &datasetKey,
                                         const QString &datasetLabel,
                                         const QString &datasetKind,
                                         const QString &ownerScope)
{
    if (!m_window || m_sessionDock)
        return;
    m_window->setDockNestingEnabled(true);   // no-op if already enabled (cube)

    m_sessionDock = new QDockWidget(QObject::tr("Session Data"), m_window);
    m_sessionDock->setObjectName(u"dockSessionData"_s);
    // Not closable — the dock collapses to a rail instead of hiding (§4.1).
    m_sessionDock->setFeatures(QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable);
    // Left column container: product tree on top, LAYERS block below (design #1d).
    // Fixed width from the spec constant (272 px) — checkable in one place.
    auto *container = new QWidget(m_sessionDock);
    container->setMinimumWidth(VisivoTheme::kLeftDockWidth);
    container->setMaximumWidth(VisivoTheme::kLeftDockWidth);
    auto *vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_sessionTree = new SessionDataTree(container);
    // Bind the tree to this window's product scope BEFORE seeding it, so a second viewer on the
    // same dataset never shows this one's products (session-wide registry).
    m_sessionTree->setOwnerScope(ownerScope);
    // Tree takes its natural height (flex:none) — NOT the layout stretch, or it
    // leaves a big gap above LAYERS. The stretch goes at the bottom of the panel.
    m_sessionTree->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    vbox->addWidget(m_sessionTree, 0);
    // Tree selection = pane activation: forward to the window so it can make the pane
    // showing that view/product the active one (or mount it).
    connect(m_sessionTree, &SessionDataTree::rowActivated, this,
            &WorkspaceChrome::sessionRowActivated);
    // Selecting a row focuses the pane that already shows it (no mounting).
    connect(m_sessionTree, &SessionDataTree::rowSelected, this,
            &WorkspaceChrome::sessionRowSelected);
    // (#4) A pane-number badge click activates that pane.
    connect(m_sessionTree, &SessionDataTree::paneBadgeActivated, this,
            &WorkspaceChrome::sessionPaneBadgeActivated);
    // Right-click a view or a product → the viewer builds the "Show in ▸" menu.
    connect(m_sessionTree, &SessionDataTree::rowContextMenuRequested, this,
            &WorkspaceChrome::sessionRowContextMenu);
    // Double-click on a product with no window to raise → the viewer decides
    // (a backend FITS can be reopened).
    connect(m_sessionTree, &SessionDataTree::productReopenRequested, this,
            &WorkspaceChrome::sessionProductReopen);

    // Host for the view/rendering controls (filled by the window via
    // addSessionLayers()). NOTE: no "LAYERS" header here — the flat controls the
    // window installs bring their own section headers (DISPLAY — 3D, LAYERS, …),
    // so a header here would read as the parent of the rendering controls.
    auto *layersScroll = new QScrollArea(container);
    layersScroll->setWidgetResizable(true);
    layersScroll->setFrameShape(QFrame::NoFrame);
    // A horizontal bar WHEN NEEDED, not never. The column is a fixed 272 px and
    // the content cannot always shrink to it — a combo box is as wide as its
    // longest entry — so "always off" did not mean "it fits", it meant the right
    // edge of the controls was cut off with nothing to say so. Everything the
    // window puts here is reachable now, whatever its natural width.
    layersScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto *layersInner = new QWidget(layersScroll);
    m_sessionLayersLayout = new QVBoxLayout(layersInner);
    m_sessionLayersLayout->setContentsMargins(0, 0, 0, 0);
    m_sessionLayersLayout->setSpacing(0);
    m_sessionLayersLayout->addStretch(1);
    layersScroll->setWidget(layersInner);
    vbox->addWidget(layersScroll, 2);

    // Two-state: page 0 the content, page 1 a 36 px rail (§4.1.2). Collapsing
    // swaps the page + pins the dock width — it does not resize the content.
    m_sessionStack = new QStackedWidget(m_sessionDock);
    m_sessionRail = new DockRail(QObject::tr("SESSION DATA"), u"◧"_s, m_sessionStack);
    m_sessionStack->addWidget(container);      // page 0
    m_sessionStack->addWidget(m_sessionRail);  // page 1
    m_sessionDock->setWidget(m_sessionStack);
    QObject::connect(m_sessionRail, &DockRail::expandRequested, this,
                     [this]() { setSessionCollapsed(false); });

    m_sessionDock->setTitleBarWidget(
            makeDockTitleBar(QObject::tr("Session Data"), u"◧"_s,
                             [this]() { setSessionCollapsed(true); }));

    m_window->addDockWidget(Qt::LeftDockWidgetArea, m_sessionDock);
    m_sessionTree->setCurrentDataset(datasetKey, datasetLabel, datasetKind);
    applyDockWidth(m_sessionDock, m_sessionStack, /*collapsed=*/false, VisivoTheme::kLeftDockWidth);
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
    // Let the controls fit the column, which is a fixed 272 px.
    //
    // What overflows is not the layout, it is the MINIMUM width of individual
    // fields: a QComboBox asks for its longest entry ("MIP (Max Intensity)",
    // "Supergalactic (SGL, SGB)"), and a QDoubleSpinBox with 12 decimals asks
    // for "0.383494066596" plus its buttons. Add a label and the card's margins
    // and the page cannot be drawn in the column however the layout is written,
    // so its right-hand edge was clipped. Each of these is allowed to shrink and
    // elide instead; the full value stays readable by clicking into the field,
    // and a combo's entries are all in its popup.
    // EXPANDING with a small minimum, not Ignored: a field has to be able to
    // shrink to fit the column, and then take every pixel the column leaves it.
    // Ignored does the first and not the second — the fields collapsed to their
    // minimum and the entries read "Ellipsoic", "Comput", "Planck1" with most
    // of the column empty beside them.
    const auto relax = [](QWidget *widget, int minimumWidth) {
        // A width the caller FIXED is a decision, not an accident: minimum ==
        // maximum is what setFixedWidth() leaves behind, and overwriting it
        // would silently undo it.
        if (widget->minimumWidth() == widget->maximumWidth())
            return;
        widget->setMinimumWidth(minimumWidth);
        widget->setSizePolicy(QSizePolicy::Expanding, widget->sizePolicy().verticalPolicy());
    };
    for (QComboBox *combo : w->findChildren<QComboBox *>()) {
        // The minimum is what the combo may shrink TO, not what it will show:
        // four characters plus the arrow, so a long entry no longer sets the
        // width of the whole page.
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(4);
        relax(combo, 70);
    }
    for (QAbstractSpinBox *spin : w->findChildren<QAbstractSpinBox *>())
        relax(spin, 70);
    for (QLineEdit *edit : w->findChildren<QLineEdit *>()) {
        // Not the editor INSIDE a spin box or an editable combo: that one is
        // sized by its owner, which has already been given a minimum here, and
        // forcing the same 70 px on the inner editor leaves no room for the
        // frame and the buttons around it.
        if (qobject_cast<QAbstractSpinBox *>(edit->parentWidget())
            || qobject_cast<QComboBox *>(edit->parentWidget()))
            continue;
        relax(edit, 70);
    }
    // And let the forms GIVE them that width. QFormLayout's default on macOS is
    // FieldsStayAtSizeHint: the field column stays at the size hint whatever
    // room there is, so relaxing the minimum alone left the controls at their
    // minimum — a 100 px combo reading "Comput" beside 150 px of empty column.
    for (QFormLayout *form : w->findChildren<QFormLayout *>())
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_sessionLayersLayout->insertWidget(m_sessionLayersLayout->count() - 1, w);
    w->show();
}

AssistantPanel *WorkspaceChrome::installCopilot(const QString &url, const QString &token,
                                                const QString &datasetId)
{
    if (!m_inspector)
        return nullptr;
    AssistantPanel *panel = m_inspector->ensureCopilotTab();
    panel->setBackend(url, token);
    panel->setDatasetId(datasetId);
    // Give the collapsed Inspector rail a 4th icon for the Copilot tab so it's
    // reachable when the dock is railed (matches Properties/Analysis/Provenance).
    if (m_inspectorRail) {
        m_inspectorRail->setEntries(
                { { u":/icons/rail_info.svg"_s, QObject::tr("Properties") },
                  { u":/icons/rail_stats.svg"_s, QObject::tr("Analysis") },
                  { u":/icons/rail_provenance.svg"_s, QObject::tr("Parameters") },
                  { u":/icons/rail_copilot.svg"_s, QObject::tr("Copilot") } });
    }
    return panel;
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
    // Not closable — collapses to a rail instead of hiding (§4.1).
    m_inspectorDock->setFeatures(QDockWidget::DockWidgetMovable
                                 | QDockWidget::DockWidgetFloatable);
    m_inspector = new InspectorPanel(m_inspectorDock);
    // Fixed right column from the spec constant (320 px).
    m_inspector->setMinimumWidth(VisivoTheme::kRightDockWidth);
    m_inspector->setMaximumWidth(VisivoTheme::kRightDockWidth);

    // Two-state: page 0 the panel, page 1 a rail carrying the three tab names as
    // clickable entries (§4.1.2). Right starts collapsed (§4.1.4).
    m_inspectorStack = new QStackedWidget(m_inspectorDock);
    m_inspectorRail = new DockRail(QObject::tr("INSPECTOR"), u"◨"_s, m_inspectorStack);
    // Icons only when collapsed (no rotated text): Properties→info, Analysis→stats,
    // Provenance→metadata; the tab name is the tooltip (§3.0c).
    m_inspectorRail->setEntries({ { u":/icons/rail_info.svg"_s, QObject::tr("Properties") },
                                  { u":/icons/rail_stats.svg"_s, QObject::tr("Analysis") },
                                  { u":/icons/rail_provenance.svg"_s, QObject::tr("Parameters") } });
    m_inspectorStack->addWidget(m_inspector);       // page 0
    m_inspectorStack->addWidget(m_inspectorRail);   // page 1
    m_inspectorDock->setWidget(m_inspectorStack);
    QObject::connect(m_inspectorRail, &DockRail::expandRequested, this,
                     [this]() { setInspectorCollapsed(false); });
    QObject::connect(m_inspectorRail, &DockRail::entryClicked, this,
                     [this](int i) { revealInspectorTab(i); });

    m_inspectorDock->setTitleBarWidget(
            makeDockTitleBar(QObject::tr("Inspector"), u"◨"_s,
                             [this]() { setInspectorCollapsed(true); }));

    m_window->addDockWidget(Qt::RightDockWidgetArea, m_inspectorDock);

    // Keep the rail's active entry in sync when the tab changes by any route.
    if (auto *tabs = m_inspector->findChild<QTabWidget *>()) {
        QObject::connect(tabs, &QTabWidget::currentChanged, this, [this](int i) {
            if (m_inspectorRail)
                m_inspectorRail->setActiveEntry(i);
        });
    }

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
                    m_inspector->showProvenance(p);
                });
    }

    // Launching an Analysis tool auto-expands the Inspector onto the Analysis tab.
    connect(m_inspector, &InspectorPanel::toolLaunched, this,
            [this]() { revealInspectorTab(1); });

    // Right dock starts as a rail (§4.1.4) — the Inspector is only needed when
    // starting a task; threshold/LUT/blend on the left are touched constantly.
    setInspectorCollapsed(true);
    return m_inspector;
}

QWidget *WorkspaceChrome::makeDockTitleBar(const QString &title, const QString &glyph,
                                           const std::function<void()> &onToggle)
{
    auto *bar = new QWidget();
    bar->setFixedHeight(34);
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(14, 0, 8, 0);
    auto *label = new QLabel(title, bar);
    label->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:12px;")
                                 .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
    h->addWidget(label);
    h->addStretch(1);
    auto *btn = new QToolButton(bar);
    btn->setText(glyph);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(QObject::tr("Collapse panel"));
    btn->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;"
                                      "color:%1;font-size:13px;}QToolButton:hover{color:%2;}")
                               .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                                    QLatin1StringView(VisivoTheme::kPrimary())));
    QObject::connect(btn, &QToolButton::clicked, bar, onToggle);
    h->addWidget(btn);
    return bar;
}

void WorkspaceChrome::applyDockWidth(QDockWidget *dock, QStackedWidget *stack, bool collapsed,
                                     int openWidth)
{
    if (!stack)
        return;
    const int w = collapsed ? VisivoTheme::kRailWidth : openWidth;
    stack->setCurrentIndex(collapsed ? 1 : 0);
    // Relax page 0's pinned width while hidden — otherwise its 272/320 minimum
    // leaks through QStackedWidget::minimumSizeHint() (max over all pages) and Qt
    // resolves min>max in favour of the minimum, keeping the rail wide.
    if (auto *page = stack->widget(0)) {
        page->setMinimumWidth(collapsed ? 0 : openWidth);
        page->setMaximumWidth(collapsed ? QWIDGETSIZE_MAX : openWidth);
    }
    stack->setMinimumWidth(w);
    stack->setMaximumWidth(w);

    // Deferred so the widths are read AFTER the layout settles — an inline read here
    // reports the pre-layout geometry. Collapsed must land 36 / 36 / 36 (+ handle).
    QPointer<QDockWidget> dockp(dock);
    QPointer<QStackedWidget> stackp(stack);
    QTimer::singleShot(0, this, [this, dockp, stackp, collapsed]() {
        if (!dockp || !stackp)
            return;
        qDebug().noquote()
                << QStringLiteral("[dock] %1 collapsed=%2 stack=%3 dock=%4 area=%5")
                           .arg(dockp->objectName())
                           .arg(collapsed)
                           .arg(stackp->width())
                           .arg(dockp->width())
                           .arg(m_window && m_window->centralWidget()
                                        ? m_window->centralWidget()->x()
                                        : -1);
    });
}

void WorkspaceChrome::setSessionCollapsed(bool collapsed)
{
    if (!m_sessionStack || !m_sessionDock || m_sessionCollapsed == collapsed)
        return;
    m_sessionCollapsed = collapsed;
    applyDockWidth(m_sessionDock, m_sessionStack, collapsed, VisivoTheme::kLeftDockWidth);
    // Swap (not hide) the title bar: a hidden bar still contributes its minimum
    // width to the dock (§4.5). Collapsed → an empty 0-height bar; the rail carries
    // the name + glyph.
    QWidget *oldBar = m_sessionDock->titleBarWidget();
    if (collapsed) {
        auto *empty = new QWidget(m_sessionDock);
        empty->setFixedHeight(0);
        m_sessionDock->setTitleBarWidget(empty);
    } else {
        m_sessionDock->setTitleBarWidget(
                makeDockTitleBar(QObject::tr("Session Data"), u"◧"_s,
                                 [this]() { setSessionCollapsed(true); }));
    }
    if (oldBar)
        oldBar->deleteLater();
    // Never closable (acceptance §4: a dock can never reach 0 px); a 36 px rail is
    // not draggable/floatable either.
    m_sessionDock->setFeatures(collapsed ? QDockWidget::NoDockWidgetFeatures
                                         : (QDockWidget::DockWidgetMovable
                                            | QDockWidget::DockWidgetFloatable));
    emit dockCollapseChanged();
}

void WorkspaceChrome::setInspectorCollapsed(bool collapsed)
{
    if (!m_inspectorStack || !m_inspectorDock || m_inspectorCollapsed == collapsed)
        return;
    m_inspectorCollapsed = collapsed;
    applyDockWidth(m_inspectorDock, m_inspectorStack, collapsed, VisivoTheme::kRightDockWidth);
    // Swap (not hide) the title bar — see setSessionCollapsed (§4.5).
    QWidget *oldBar = m_inspectorDock->titleBarWidget();
    if (collapsed) {
        auto *empty = new QWidget(m_inspectorDock);
        empty->setFixedHeight(0);
        m_inspectorDock->setTitleBarWidget(empty);
    } else {
        m_inspectorDock->setTitleBarWidget(
                makeDockTitleBar(QObject::tr("Inspector"), u"◨"_s,
                                 [this]() { setInspectorCollapsed(true); }));
    }
    if (oldBar)
        oldBar->deleteLater();
    m_inspectorDock->setFeatures(collapsed ? QDockWidget::NoDockWidgetFeatures
                                           : (QDockWidget::DockWidgetMovable
                                              | QDockWidget::DockWidgetFloatable));
    emit dockCollapseChanged();
}

InspectorPanel *WorkspaceChrome::inspectorPanel() const
{
    return m_inspector;
}

void WorkspaceChrome::revealInspectorTab(int index)
{
    setInspectorCollapsed(false);
    if (m_inspector)
        m_inspector->showTab(index);
    if (m_inspectorRail)
        m_inspectorRail->setActiveEntry(index);
}

void WorkspaceChrome::setSessionRailCount(int count)
{
    if (m_sessionRail)
        m_sessionRail->setCount(count);
}

void WorkspaceChrome::selectSessionProduct(const QString &productId)
{
    if (m_sessionTree)
        m_sessionTree->selectProduct(productId);
}

void WorkspaceChrome::selectSessionRowForView(const QString &viewTag, const QString &productId)
{
    if (m_sessionTree)
        m_sessionTree->selectForView(viewTag, productId);
}

void WorkspaceChrome::setSessionProductPaneAssignments(
        const QHash<QString, QList<int>> &assignments)
{
    if (m_sessionTree)
        m_sessionTree->setProductPaneAssignments(assignments);
}

void WorkspaceChrome::setStatusRailCompact(bool compact)
{
    // Health + queue always stay; cache + session are the ones focus mode drops.
    if (m_cacheSlot)
        m_cacheSlot->setVisible(!compact);
    if (m_sessionSlot)
        m_sessionSlot->setVisible(!compact);
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
        s.queuedTasks = h.queuedTasks;
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
        // Tasks not started yet, as the backend reports them. (entries − running
        // counted every FINISHED task still in the registry as "queued": a live PV
        // edit made it climb into the dozens while nothing waited at all.)
        const int queued = std::max(0, s.queuedTasks);
        // The backend's running figure counts its task REGISTRY (the async moment /
        // PV jobs). Every other compute — line-width, isosurface, stacking, noise,
        // baseline — is a synchronous POST that appears nowhere in it, so this
        // read "0 running" for minutes while one was in flight (user report).
        // The client knows about its own, so add them.
        const int local = BackendClient::heavyRequestsInFlight();
        m_queueSlot->setText(QStringLiteral("%1 running · %2 queued")
                                     .arg(std::max(0, s.runningTasks - queued) + local)
                                     .arg(queued));
    }
    // Right rail count reuses the running-task figure the poll already computed
    // (§4.1.4 — do not add a second poll), including this client's own
    // synchronous computes for the same reason as the status slot above.
    if (m_inspectorRail)
        m_inspectorRail->setCount(s.runningTasks + BackendClient::heavyRequestsInFlight());
    if (m_cacheSlot) {
        m_cacheSlot->setText(
                QStringLiteral("cache %1/%2").arg(s.cacheEntries).arg(s.cacheCapacity));
    }
}

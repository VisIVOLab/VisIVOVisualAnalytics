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
#include "UpdateNotifier.h"
#include "theme/VisivoTheme.h"
#include "SourceFindDialog.h"
#include "StackDialog.h"
#include "WebViewProcess.h"
#include "vtkWindowCatalogue3D.h"
#include "vtkWindowCube.h"
#include "vtkWindowImage.h"

#include <QGridLayout>
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

namespace {

QPixmap makeDialogQuestionPixmap(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor fg(QString::fromLatin1(VisivoTheme::kOnSurface()));
    QPen pen(fg);
    pen.setWidthF(3.2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    QFont f(QStringLiteral("Lato"));
    f.setPixelSize(int(size * 0.78));
    f.setWeight(QFont::Black);
    p.setFont(f);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QStringLiteral("?"));

    return pm;
}

QString sampDisplayName(const QString &publicId)
{
    if (publicId.trimmed().isEmpty()) {
        return QStringLiteral("VisIVO");
    }
    return QStringLiteral("VisIVO [%1]").arg(publicId);
}

QString sampPeerLabel(const SampClientInfo &info)
{
    const QString name = info.name.trimmed().isEmpty() ? QStringLiteral("Unnamed") : info.name.trimmed();
    return QStringLiteral("%1 [%2]").arg(name, info.id);
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    qInfo().noquote() << qApp->applicationName() << qApp->applicationVersion();

    const QString workDir = QDir::home().absoluteFilePath(qApp->applicationName());
    qInfo() << "Working directory:" << workDir;

    // Create core objects
    this->settings = new Settings(workDir, this);
    // Wire the backend-client factory to Settings so it can resolve the
    // multi-backend registry. After this point any code may call
    // BackendClientFactory::instance()->get(id) without further setup.
    BackendClientFactory::instance()->setSettings(this->settings);
    BackendClient::setLocalBackendUrl(this->settings->getBackendUrl());
    this->auth = new AuthWrapper(this);
    this->m_sampBackendClient = std::make_unique<BackendClient>(
            this->settings->getBackendUrl(), this->settings->getBackendToken());
    this->m_sampClient = new SampClient(this->m_sampBackendClient.get(), this);
    this->m_sampHandler =
            new SampMessageHandler(this->m_sampClient, this->m_sampBackendClient.get(), this);
    QObject::connect(this->settings, &Settings::updated, this, &MainWindow::setApplicationTheme);
    QObject::connect(this->settings, &Settings::updated, this, &MainWindow::loadPanoramicView);
    QObject::connect(this->settings, &Settings::updated, this, &MainWindow::syncDataHubSettings);
    QObject::connect(this->settings, &Settings::updated, this, &MainWindow::syncSampSettings);

    // ── Replace the .ui central widget with a tab container ─────────────────
    // The .ui-defined central widget (panoramic view + coordinate panel) is
    // detached and becomes "VLKB" tab 1.  DataHubWidget becomes tab 0 so
    // the app opens on the backend-first landing page.
    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->setTabPosition(QTabWidget::North);
    tabs->setTabsClosable(true); // shows close button on user-added tabs
    this->m_tabs = tabs;

    this->dataHub = new DataHubWidget(tabs);
    tabs->addTab(this->dataHub, u"Data Hub"_s);

    // ── Recent datasets ───────────────────────────────────────────────────────
    this->recentMgr = new RecentDatasetsManager(
            QDir::home().absoluteFilePath(qApp->applicationName()), this);
    this->dataHub->setRecentDatasetsManager(this->recentMgr);
    QObject::connect(this->dataHub, &DataHubWidget::reopenDatasetRequested,
                     this, &MainWindow::reopenDataset);
    // Workspace Exports: "Open in VisIVO" routes through the standard
    // open-by-path flow so the artefact is registered as a session
    // dataset and lands in a cube / image viewer just like any other
    // dataset.
    QObject::connect(this->dataHub, &DataHubWidget::openWorkspaceExportRequested,
                     this, &MainWindow::doOpenDataPath);

    // ── SKAVA Discovery tab ───────────────────────────────────────────
    // Persistent tab next to Data Hub. The panel handles its own state
    // (filters, results, provenance) and emits one signal on Open:
    //   - openSkavaDatasetRequested → backend-aware routing via
    //     doOpenSkavaDataset() which checks visivo-backend service
    //     descriptors against the registry (compute next to data)
    {
        auto *skava = new SkavaSearchPanel(this->settings, tabs);
        tabs->addTab(skava, u"SKAVA"_s);
        QObject::connect(skava, &SkavaSearchPanel::openSkavaDatasetRequested,
                         this, &MainWindow::doOpenSkavaDataset);
        QObject::connect(skava, &SkavaSearchPanel::nodeOperationRequested,
                         this, &MainWindow::doSkavaNodeOperation);
    }

    // Detach the .ui-defined central widget (the VLKB sky panel) but DO NOT
    // add it to the tab strip on startup. VLKB now opens on demand only —
    // via the Data Hub Quick action or the ⌘K command palette, both routed
    // through revealVLKBTab(). The widget is kept alive (parent = tabs so it
    // shares the lifecycle) and hidden until first reveal, so the inventory
    // tree state survives between open/close cycles.
    auto *skyPanel = this->centralWidget();
    skyPanel->setParent(tabs);
    skyPanel->hide();
    this->m_vlkbPanel = skyPanel;

    this->setCentralWidget(tabs);

    // Data Hub is permanent (no close button). VLKB, when present, can be
    // closed via the standard X — see the close handler below.
    tabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);

    QObject::connect(tabs, &QTabWidget::tabCloseRequested, this,
                     [this, tabs](int index) {
        if (index == 0) return;                  // Data Hub is permanent
        QWidget *w = tabs->widget(index);
        tabs->removeTab(index);
        if (w == this->m_vlkbPanel) {
            // Keep the VLKB panel alive (just hide it) so reopening preserves
            // the user's inventory tree expansion / selection.
            w->setParent(nullptr);
            w->hide();
        } else {
            w->deleteLater();
        }
    });

    // ── Setup Menu File (existing actions) ───────────────────────────────────
    // File ▸ Open… takes anything: the file is identified on the backend and
    // routed to the viewer that fits it (see MainWindow::dispatchOpenForPath).
    QObject::connect(ui->actionOpenFile, &QAction::triggered, this,
                     &MainWindow::openAnything);
    QObject::connect(ui->actionOpenVbt, &QAction::triggered, this, &MainWindow::openVbt);
    QObject::connect(ui->actionOpenCatalogue3D, &QAction::triggered, this,
                     &MainWindow::openCatalogue3D);
    QObject::connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    // ── Setup Menu Edit ───────────────────────────────────────────────────────
    QObject::connect(ui->actionSettings, &QAction::triggered, this,
                     &MainWindow::openSettingsDialog);

    // ── Data menu (new, primary navigation) ──────────────────────────────────
    this->buildDataMenu();

    // ── View menu ─────────────────────────────────────────────────────────────
    auto *viewMenu = new QMenu(u"View"_s, this);
    this->menuBar()->insertMenu(ui->menuHelp->menuAction(), viewMenu);
    m_viewMenu = viewMenu;
    auto *diagnosticsAction = viewMenu->addAction(u"Diagnostics"_s);
    diagnosticsAction->setToolTip(
            u"Open the diagnostics window: backend requests, task states and the "
            u"event log."_s);
    QObject::connect(diagnosticsAction, &QAction::triggered, this,
                     [this]() { this->openDiagnosticsWindow(); });
    viewMenu->setToolTipsVisible(true);

    // ── SAMP menu ─────────────────────────────────────────────────────────────
    this->buildSampMenu();

    // ── Science menu ──────────────────────────────────────────────────────────
    this->buildCombineMenu();

    // ── Setup Menu About ──────────────────────────────────────────────────────
    QObject::connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::openAboutDialog);

    // ── New-version notice ────────────────────────────────────────────────────
    // Once a day at startup (if enabled in Settings) and on demand from Help.
    this->updateNotifier = new UpdateNotifier(this->settings, this);
    QObject::connect(ui->actionCheckUpdates, &QAction::triggered, this,
                     [this]() { if (this->updateNotifier) this->updateNotifier->check(true); });
    this->updateNotifier->scheduleStartupCheck();

    // ── Setup Panoramic View (VLKB tab) ──────────────────────────────────────
    this->loadPanoramicView();
    auto webobj = new WebViewProcess(ui->view);
    auto webChannel = new QWebChannel(ui->view);
    webChannel->registerObject(u"webobj"_s, webobj);
    ui->view->page()->setWebChannel(webChannel);
    QObject::connect(webobj, &WebViewProcess::processJavascript, this,
                     &MainWindow::skyRegionSelected);

    // What these radios do was the single most confusing thing about the panel:
    // they are a DRAWING mode for the sky view, not the shape of the query, and
    // nothing said so. The query's shape follows the fields below.
    ui->boxSelection->setToolTip(
            u"Choose what you can draw on the sky view. Drawing fills in the "
            u"coordinates below — it does not decide the shape of the query."_s);
    ui->radioSelNone->setToolTip(u"Don't draw on the sky view; type the coordinates instead."_s);
    ui->radioSelPoint->setToolTip(u"Click the sky view to set the centre."_s);
    ui->radioSelRect->setToolTip(u"Drag a rectangle on the sky view to set the centre and dl / db."_s);

    auto groupSelection = new QButtonGroup(this);
    groupSelection->addButton(ui->radioSelNone);
    groupSelection->addButton(ui->radioSelPoint);
    groupSelection->addButton(ui->radioSelRect);
    QObject::connect(groupSelection, &QButtonGroup::buttonToggled, this,
                     &MainWindow::changeViewSelectionMode);

    // A live description of the region the Query button would send. The panel
    // otherwise gave no way to tell a cone from a box — the shape is decided by
    // which of radius / dl / db happen to be filled, and the "Selection" radios
    // above only choose what the sky view lets you draw.
    m_vlkbQueryPreview = new QLabel(this);
    m_vlkbQueryPreview->setWordWrap(true);
    m_vlkbQueryPreview->setTextFormat(Qt::RichText);
    if (auto *regionLayout = qobject_cast<QGridLayout *>(ui->boxRegion->layout()))
        regionLayout->addWidget(m_vlkbQueryPreview, 6, 0, 1, 3);
    for (QLineEdit *field : { ui->lineLon, ui->lineLat, ui->lineRadius,
                              ui->lineDLon, ui->lineDLat }) {
        QObject::connect(field, &QLineEdit::textChanged, this,
                         &MainWindow::refreshVlkbQueryPreview);
    }

    ui->lineLon->setValidator(new QDoubleValidator(ui->lineLon));
    ui->lineLat->setValidator(new QDoubleValidator(ui->lineLat));
    ui->lineRadius->setValidator(new QDoubleValidator(ui->lineRadius));
    ui->lineDLon->setValidator(new QDoubleValidator(ui->lineDLon));
    ui->lineDLat->setValidator(new QDoubleValidator(ui->lineDLat));
    refreshVlkbQueryPreview();

    QObject::connect(ui->btnQuery, &QPushButton::clicked, this, &MainWindow::queryVlkb);

    // ── Wire DataHub signals to MainWindow actions ────────────────────────────
    QObject::connect(this->dataHub, &DataHubWidget::openAnythingRequested,
                     this, &MainWindow::openAnything);
    QObject::connect(this->dataHub, &DataHubWidget::openDataRequested,
                     this, &MainWindow::openData);
    QObject::connect(this->dataHub, &DataHubWidget::openCatalogueRequested,
                     this, &MainWindow::openCatalogue3D);
    QObject::connect(this->dataHub, &DataHubWidget::openVbtRequested,
                     this, &MainWindow::openVbt);
    QObject::connect(this->dataHub, &DataHubWidget::openHiPSRequested,
                     this, &MainWindow::openHiPS);
    QObject::connect(this->dataHub, &DataHubWidget::openVLKBRequested,
                     this, [this]() { this->revealVLKBTab(); });
    QObject::connect(this->dataHub, &DataHubWidget::commandPaletteRequested,
                     this, &MainWindow::openCommandPalette);

    // ── Initial DataHub state ─────────────────────────────────────────────────
    this->syncDataHubSettings();
    this->syncSampSettings();
    this->updateSampActions();

    QObject::connect(this->m_sampHandler, &SampMessageHandler::fitsDatasetOpened,
                     this, &MainWindow::handleSampFitsDataset);
    QObject::connect(this->m_sampHandler, &SampMessageHandler::votableLoaded,
                     this, &MainWindow::handleSampTableLoad);
    QObject::connect(this->m_sampHandler, &SampMessageHandler::skyCoordReceived,
                     this, &MainWindow::handleSampSkyCoord);
    QObject::connect(this->m_sampHandler, &SampMessageHandler::sampConnected, this,
                     [this](const QString &publicId, const QString &hubUrl) {
                         const QString displayName = sampDisplayName(publicId);
                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                 QStringLiteral("SAMP"),
                                 QStringLiteral("Connected to SAMP Hub as %1 (%2).")
                                         .arg(displayName, hubUrl),
                                 QString(), QString(), QStringLiteral("connect"));
                         this->statusBar()->showMessage(
                                 QStringLiteral("SAMP connected: %1").arg(displayName), 3000);
                         this->updateSampActions();
                     });
    QObject::connect(this->m_sampHandler, &SampMessageHandler::sampDisconnected, this, [this]() {
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("SAMP"),
                QStringLiteral("Disconnected from SAMP Hub."),
                QString(), QString(), QStringLiteral("disconnect"));
        this->statusBar()->showMessage(u"SAMP disconnected"_s, 3000);
        this->updateSampActions();
    });
    QObject::connect(this->m_sampHandler, &SampMessageHandler::sampError, this,
                     [this](const QString &description) {
                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Warning, DiagnosticsCategory::Remote,
                                 QStringLiteral("SAMP"), description,
                                 QString(), QString(), QStringLiteral("error"));
                         this->statusBar()->showMessage(description, 5000);
                         this->updateSampActions();
                     });
    QObject::connect(this->m_sampClient, &SampClient::clientListChanged, this,
                     [this](const QList<SampClientInfo> &clients) {
                         QString message;
                         if (clients.isEmpty()) {
                             message = QStringLiteral("SAMP Hub peers: none.");
                         } else {
                             QStringList labels;
                             labels.reserve(clients.size());
                             for (const SampClientInfo &info : clients) {
                                 labels.push_back(sampPeerLabel(info));
                             }
                             message = QStringLiteral("SAMP Hub peers: %1").arg(labels.join(QStringLiteral(", ")));
                         }
                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                 QStringLiteral("SAMP"), message,
                                 QString(), QString(), QStringLiteral("peers"));
                     });

    // Load theme
    this->setApplicationTheme();

    // ── Command palette (⌘K / Ctrl+K) ────────────────────────────────────────
    {
        auto *paletteAction = new QAction(u"Command Palette"_s, this);
        paletteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
        paletteAction->setShortcutContext(Qt::ApplicationShortcut);
        paletteAction->setText(u"Command Palette…"_s);
        paletteAction->setToolTip(
                u"Search every action in this window by name."_s);
        QObject::connect(paletteAction, &QAction::triggered,
                         this, &MainWindow::openCommandPalette);
        this->addAction(paletteAction);
        // …and in the menu, like every other window: a shortcut nobody can see
        // is a feature only its author knows about.
        if (m_viewMenu)
            m_viewMenu->addAction(paletteAction);
    }

    // ── Backend diagnostics mirror ───────────────────────────────────────────
    // Periodically pull /v1/diagnostics/events from the backend and republish
    // each new entry into DiagnosticsManager so the desktop window shows one
    // unified log (client events + backend logger output). Long enough not
    // to thrash the network, short enough to feel real-time.
    m_backendDiagPollTimer.setInterval(2000);
    QObject::connect(&m_backendDiagPollTimer, &QTimer::timeout,
                     this, &MainWindow::pollBackendDiagnosticsOnce);
    m_backendDiagPollTimer.start();

    // ── Active-backend badge in the status bar ──────────────────────────────
    // Persistent indicator on the right side of the status bar showing
    // which backend is currently serving compute (local vs. one of the
    // registered remote SRCs). Updated by ``setActiveBackendBadge()``
    // every time a dataset open routes through a specific backend.
    this->m_backendBadge = new QLabel(this);
    this->m_backendBadge->setContentsMargins(8, 0, 8, 0);
    this->m_backendBadge->setToolTip(
            u"Backend currently serving compute. Click Settings → Remote "
            u"Backends to register additional SRC nodes."_s);
    this->statusBar()->addPermanentWidget(this->m_backendBadge);
    this->setActiveBackendBadge(this->settings->getLocalBackend());

    this->setMinimumSize(1200, 840);
    this->resize(1280, 840);
}

void MainWindow::setActiveBackendBadge(const BackendNode &node, const QString &reason)
{
    if (!this->m_backendBadge) {
        return;
    }
    // The default backend slot is "Local" unless this run was started on a
    // remote backend (--backend): then that one serves everything by default.
    const Settings::SessionBackend *session = Settings::sessionBackend();
    const bool local = node.isLocal && !session;
    const QString name = node.isLocal && session ? session->name
                                                 : (node.name.isEmpty() ? node.id : node.name);
    const QString labelText = local
            ? u"Backend: Local"_s
            : u"Backend: %1"_s.arg(name);
    // Accent colour for remote backends so the user can tell at a glance
    // that compute is happening off-host; muted for the default local
    // path so it does not draw attention.
    const QString colour = local ? u"#888"_s : u"#2c7be5"_s;
    this->m_backendBadge->setText(
            u"<span style='color:%1'><b>%2</b></span>"_s.arg(colour, labelText));
    // Why this backend: SKAVA's ranking reason for the node that serves the
    // dataset, where the open came from discovery.
    this->m_backendBadge->setToolTip(reason.isEmpty() ? QString()
                                                      : u"%1 serves this dataset: %2"_s.arg(name, reason));
}

void MainWindow::pollBackendDiagnosticsOnce()
{
    if (m_backendDiagPollInFlight) return;       // skip if the previous poll hasn't returned
    // Don't poll while a modal dialog is up. On macOS the network-reply UI
    // burst (events being appended to the diagnostics manager, possible
    // UI repaints on the DataHub status pill, etc.) is enough to make the
    // OS steal focus back to MainWindow — which dismisses native save
    // dialogs spawned by the secondary windows (Spectrum probe, PV plot,
    // …). Pausing during modal dialogs makes those saves stable.
    if (QApplication::activeModalWidget() != nullptr) return;
    const QString url   = this->settings ? this->settings->getBackendUrl()   : QString();
    const QString token = this->settings ? this->settings->getBackendToken() : QString();
    if (url.isEmpty()) return;

    m_backendDiagPollInFlight = true;
    const qint64 since = m_backendDiagSince;
    auto *watcher = new QFutureWatcher<BackendDiagnosticsEventsResult>(this);
    QPointer<MainWindow> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendDiagnosticsEventsResult>::finished,
                     this, [self, watcher]() {
                         const auto res = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;
                         self->m_backendDiagPollInFlight = false;
                         if (!res.valid) return;
                         for (const auto &e : res.events) {
                             // Map level / category strings → enums; default to
                             // Info / Backend if unknown so a future backend
                             // adds new tags without breaking the GUI.
                             DiagnosticsLevel lvl = DiagnosticsLevel::Info;
                             const QString L = e.level.toLower();
                             if (L == QLatin1String("debug"))        lvl = DiagnosticsLevel::Debug;
                             else if (L == QLatin1String("warning")) lvl = DiagnosticsLevel::Warning;
                             else if (L == QLatin1String("error"))   lvl = DiagnosticsLevel::Error;

                             DiagnosticsCategory cat = DiagnosticsCategory::Backend;
                             const QString C = e.category;
                             if (C == QLatin1String("Scientific"))      cat = DiagnosticsCategory::Scientific;
                             else if (C == QLatin1String("Client"))     cat = DiagnosticsCategory::Client;
                             else if (C == QLatin1String("Task"))       cat = DiagnosticsCategory::Task;
                             else if (C == QLatin1String("WCS"))        cat = DiagnosticsCategory::WCS;
                             else if (C == QLatin1String("Remote"))     cat = DiagnosticsCategory::Remote;
                             else if (C == QLatin1String("Rendering"))  cat = DiagnosticsCategory::Rendering;
                             else if (C == QLatin1String("Performance"))cat = DiagnosticsCategory::Performance;

                             DiagnosticsManager::instance()->publish(
                                     lvl, cat, e.source.isEmpty()
                                                 ? QStringLiteral("backend")
                                                 : QStringLiteral("backend:") + e.source,
                                     e.message, e.datasetId, e.sessionId, e.operationTag);
                         }
                         self->m_backendDiagSince =
                                 std::max(self->m_backendDiagSince, res.lastId);
                     });
    watcher->setFuture(QtConcurrent::run([url, token, since]() {
        BackendClient c(url, token);
        return c.pollBackendDiagnostics(since);
    }));
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::syncDataHubSettings()
{
    if (this->dataHub) {
        this->dataHub->setBackendUrl(this->settings->getBackendUrl(),
                                     this->settings->getBackendToken());
    }
}

void MainWindow::syncSampSettings()
{
    BackendClient::setLocalBackendUrl(this->settings->getBackendUrl());
    if (!this->m_sampBackendClient) {
        return;
    }
    this->m_sampBackendClient->setBaseUrl(this->settings->getBackendUrl());
    this->m_sampBackendClient->setToken(this->settings->getBackendToken());
}

void MainWindow::openDiagnosticsWindow()
{
    // Route through the process-wide singleton so child windows (cube/image
    // viewers, ⌘K palette) and MainWindow's own View menu all share one
    // instance — and one model — instead of opening multiple panels.
    this->diagnosticsWindow = DiagnosticsWindow::showSingleton();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QMessageBox box(this);
    box.setWindowTitle(u"Exit"_s);
    box.setText(u"Do you want to exit?"_s);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);

    box.setIconPixmap(makeDialogQuestionPixmap(40));

    const auto res = box.exec();
    if (res == QMessageBox::No) {
        event->ignore();
        return;
    }

    // User confirmed exit.  Accept this window's close, then force the whole
    // application to terminate.
    //
    // We hide() — not close() — any remaining top-level windows so that their
    // individual closeEvent busy-guards ("Loading image…", "Computing moment…"
    // etc.) cannot block the shutdown.  hide() never sends a QCloseEvent.
    //
    // BackendLauncher lives on main()'s stack; its destructor runs as soon as
    // QCoreApplication::exec() returns, terminating the backend process cleanly.
    event->accept();
    const auto topLevel = QApplication::topLevelWidgets();
    for (QWidget *w : topLevel) {
        if (w != this)
            w->hide();
    }
    QCoreApplication::exit(0);
}

void MainWindow::setApplicationTheme()
{
    ThemeManager::instance().apply(this->settings->getColorScheme(), qApp);
}

void MainWindow::openSettingsDialog()
{
    SettingsDialog dialog(this->settings, this->auth, this);
    dialog.exec();
}

void MainWindow::openAboutDialog()
{
    AboutDialog about(this);
    about.exec();
}

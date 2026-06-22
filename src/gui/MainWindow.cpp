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

namespace {

// Renders a "?" glyph at the current theme's primary text colour so it stays
// readable on either dark or light dialog backgrounds.
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

bool backendUrlIsLocal(const QString &url)
{
    const QUrl parsed(url);
    const QString host = parsed.host().trimmed().toLower();
    return host.isEmpty() || host == QStringLiteral("127.0.0.1")
           || host == QStringLiteral("localhost") || host == QStringLiteral("::1");
}

QString catalogueFormatForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("csv")) {
        return QStringLiteral("csv");
    }
    if (suffix == QStringLiteral("vot") || suffix == QStringLiteral("votable")
        || suffix == QStringLiteral("xml")) {
        return QStringLiteral("votable");
    }
    if (suffix == QStringLiteral("fits") || suffix == QStringLiteral("fit")) {
        return QStringLiteral("fits");
    }
    return QStringLiteral("csv");
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

} // namespace

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
    // (filters, results, provenance) and emits two signals on Open:
    //   - openSkavaDatasetRequested → backend-aware routing via
    //     doOpenSkavaDataset() which checks visivo-backend service
    //     descriptors against the registry (compute next to data)
    //   - openDatasetUrlRequested → legacy download-via-local fallback
    // We connect only the rich signal so we do not open twice.
    {
        auto *skava = new SkavaSearchPanel(this->settings,
                                            this->m_sampBackendClient.get(),
                                            tabs);
        tabs->addTab(skava, u"SKAVA"_s);
        QObject::connect(skava, &SkavaSearchPanel::openSkavaDatasetRequested,
                         this, &MainWindow::doOpenSkavaDataset);
        QObject::connect(skava, &SkavaSearchPanel::sodaSubsetRequested,
                         this, &MainWindow::doSkavaSodaSubset);
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
    QObject::connect(ui->actionOpenFile, &QAction::triggered, this, &MainWindow::openData);
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
    auto *diagnosticsAction = viewMenu->addAction(u"Diagnostics"_s);
    QObject::connect(diagnosticsAction, &QAction::triggered, this,
                     [this]() { this->openDiagnosticsWindow(); });

    // ── SAMP menu ─────────────────────────────────────────────────────────────
    this->buildSampMenu();

    // ── Science menu ──────────────────────────────────────────────────────────
    this->buildScienceMenu();

    // ── Setup Menu About ──────────────────────────────────────────────────────
    QObject::connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::openAboutDialog);

    // ── Setup Panoramic View (VLKB tab) ──────────────────────────────────────
    this->loadPanoramicView();
    auto webobj = new WebViewProcess(ui->view);
    auto webChannel = new QWebChannel(ui->view);
    webChannel->registerObject(u"webobj"_s, webobj);
    ui->view->page()->setWebChannel(webChannel);
    QObject::connect(webobj, &WebViewProcess::processJavascript, this,
                     &MainWindow::skyRegionSelected);

    auto groupSelection = new QButtonGroup(this);
    groupSelection->addButton(ui->radioSelNone);
    groupSelection->addButton(ui->radioSelPoint);
    groupSelection->addButton(ui->radioSelRect);
    QObject::connect(groupSelection, &QButtonGroup::buttonToggled, this,
                     &MainWindow::changeViewSelectionMode);

    ui->lineLon->setValidator(new QDoubleValidator(ui->lineLon));
    ui->lineLat->setValidator(new QDoubleValidator(ui->lineLat));
    ui->lineRadius->setValidator(new QDoubleValidator(ui->lineRadius));
    ui->lineDLon->setValidator(new QDoubleValidator(ui->lineDLon));
    ui->lineDLat->setValidator(new QDoubleValidator(ui->lineDLat));

    QObject::connect(ui->btnQuery, &QPushButton::clicked, this, &MainWindow::queryVlkb);

    // ── Wire DataHub signals to MainWindow actions ────────────────────────────
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
        QObject::connect(paletteAction, &QAction::triggered,
                         this, &MainWindow::openCommandPalette);
        this->addAction(paletteAction);
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

void MainWindow::setActiveBackendBadge(const BackendNode &node)
{
    if (!this->m_backendBadge) {
        return;
    }
    const QString name = node.name.isEmpty() ? node.id : node.name;
    const QString labelText = node.isLocal
            ? u"Backend: Local"_s
            : u"Backend: %1"_s.arg(name);
    // Accent colour for remote backends so the user can tell at a glance
    // that compute is happening off-host; muted for the default local
    // path so it does not draw attention.
    const QString colour = node.isLocal ? u"#888"_s : u"#2c7be5"_s;
    this->m_backendBadge->setText(
            u"<span style='color:%1'><b>%2</b></span>"_s.arg(colour, labelText));
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

// ── VLKB tab reveal (closeable but persistent) ───────────────────────────────

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

// ── Command palette (⌘K) ─────────────────────────────────────────────────────

void MainWindow::openCommandPalette()
{
    if (!this->m_commandPalette) {
        this->m_commandPalette = new CommandPalette(this);
    }
    this->rebuildCommandPalette();
    this->m_commandPalette->show();
    this->m_commandPalette->raise();
    this->m_commandPalette->activateWindow();
}

void MainWindow::rebuildCommandPalette()
{
    if (!this->m_commandPalette) return;

    // Static actions are added once; dynamic ones (Recent datasets) get
    // refreshed each call by clearing their group first.
    static bool s_staticAdded = false;
    if (!s_staticAdded) {
        // ── File ────────────────────────────────────────────────────────────
        this->m_commandPalette->addAction(
                {u"⌘"_s, u"Open Remote Dataset…"_s,
                 u"File · backend filesystem · FITS"_s, u"⌘O"_s, u"File"_s,
                 [this]() { this->openData(); }});
        this->m_commandPalette->addAction(
                {u"✦"_s, u"Open 3D Catalogue…"_s,
                 u"File · CSV / VOTable in 3D viewer"_s, QString(), u"File"_s,
                 [this]() { this->openCatalogue3D(); }});
        this->m_commandPalette->addAction(
                {u"▦"_s, u"Open VBT…"_s,
                 u"File · VisIVO Binary Table point-cloud"_s, QString(), u"File"_s,
                 [this]() { this->openVbt(); }});
        this->m_commandPalette->addAction(
                {u"◐"_s, u"HiPS Viewer…"_s,
                 u"File · hierarchical sky surveys"_s, QString(), u"File"_s,
                 [this]() { this->openHiPS(); }});
        this->m_commandPalette->addAction(
                {u"◑"_s, u"VLKB Inventory"_s,
                 u"File · browse remote VLKB sources"_s, QString(), u"File"_s,
                 [this]() { this->revealVLKBTab(); }});

        // ── Application ─────────────────────────────────────────────────────
        this->m_commandPalette->addAction(
                {u"⚙"_s, u"Open Settings…"_s,
                 u"Edit · backend URL, theme, token"_s, u"⌘,"_s,
                 u"Application"_s,
                 [this]() { this->openSettingsDialog(); }});
        this->m_commandPalette->addAction(
                {u"ⓘ"_s, u"About VisIVO Visual Analytics"_s,
                 u"Version, credits"_s, QString(), u"Application"_s,
                 [this]() { this->openAboutDialog(); }});
        this->m_commandPalette->addAction(
                {u"⏻"_s, u"Quit VisIVO Visual Analytics"_s,
                 u"Close the application"_s, u"⌘Q"_s, u"Application"_s,
                 [this]() { this->close(); }});

        // ── View ────────────────────────────────────────────────────────────
        // Diagnostics is in MainWindow's View menu but easy to miss; expose it
        // in the palette so it's reachable from anywhere via ⌘K + "diag".
        this->m_commandPalette->addAction(
                {u"⚠"_s, u"Open Diagnostics Window"_s,
                 u"View · backend / SAMP / scientific log feed"_s, QString(),
                 u"View"_s,
                 [this]() { this->openDiagnosticsWindow(); }});

        // ── Science ─────────────────────────────────────────────────────────
        // Each entry calls QAction::trigger(), which respects the action's
        // enabled state (disabled actions silently no-op when no dataset is
        // open). The menubar's Science menu shows the actual enabled state.
        this->m_commandPalette->addAction(
                {u"∿"_s, u"Line-Width Map…"_s,
                 u"Science · FWHM / equivalent-width per pixel"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actLinewidth) this->m_actLinewidth->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"⌇"_s, u"Baseline Subtraction…"_s,
                 u"Science · polynomial or median spectral baseline"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actBaseline) this->m_actBaseline->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"⌥"_s, u"Spectral Stacking…"_s,
                 u"Science · stack multiple session cubes into 1-D spectrum"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actStack) this->m_actStack->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"⌖"_s, u"Source Finding (SoFiA-2)…"_s,
                 u"Science · run SoFiA-2 on the current dataset"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actSourceFind) this->m_actSourceFind->trigger();
                 }});

        // ── SAMP ────────────────────────────────────────────────────────────
        // Cross-application messaging hub. Actions delegate to the QAction
        // toggled in buildSampMenu(); the menu mirror surfaces hub state.
        this->m_commandPalette->addAction(
                {u"⇄"_s, u"SAMP: Connect to Hub"_s,
                 u"SAMP · join the running hub"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampConnect) this->m_actSampConnect->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"⇎"_s, u"SAMP: Disconnect from Hub"_s,
                 u"SAMP · leave the hub"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampDisconnect) this->m_actSampDisconnect->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"↗"_s, u"SAMP: Send Current FITS Dataset"_s,
                 u"SAMP · broadcast current cube/image to peers"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampSendCurrentFits)
                         this->m_actSampSendCurrentFits->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"↗"_s, u"SAMP: Send Current Catalogue"_s,
                 u"SAMP · broadcast current catalogue to peers"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampSendCurrentCatalogue)
                         this->m_actSampSendCurrentCatalogue->trigger();
                 }});

        s_staticAdded = true;
    }

    // ── Recent datasets (dynamic) ───────────────────────────────────────────
    this->m_commandPalette->clearGroup(u"Recent datasets"_s);
    if (this->recentMgr) {
        const auto recent = this->recentMgr->datasets();
        // Cap at 6 to keep the palette tidy.
        const int limit = std::min<int>(6, recent.size());
        for (int i = 0; i < limit; ++i) {
            const RecentDataset ds = recent[i];
            const QString sub =
                    ds.type.isEmpty()
                            ? u"remote"_s
                            : QStringLiteral("%1 · remote").arg(ds.type.toUpper());
            this->m_commandPalette->addAction(
                    {u"▣"_s, ds.displayName, sub, QString(),
                     u"Recent datasets"_s,
                     [this, ds]() { this->reopenDataset(ds); }});
        }
    }
}

void MainWindow::buildDataMenu()
{
    // Insert "Data" menu between File and Edit so it is the primary navigation.
    auto *dataMenu = new QMenu(u"Data"_s, this);
    this->menuBar()->insertMenu(ui->menuEdit->menuAction(), dataMenu);

    // Shortcuts are already defined on the File-menu actions in the .ui file;
    // do not duplicate them here to avoid ambiguous shortcut warnings.
    auto *actOpenData = dataMenu->addAction(u"Open Remote Dataset…"_s);
    QObject::connect(actOpenData, &QAction::triggered, this, &MainWindow::openData);

    auto *actOpenCatalogue = dataMenu->addAction(u"Open 3D Catalogue (CSV)…"_s);
    QObject::connect(actOpenCatalogue, &QAction::triggered, this, &MainWindow::openCatalogue3D);

    auto *actOpenVbt = dataMenu->addAction(u"Open VBT…"_s);
    QObject::connect(actOpenVbt, &QAction::triggered, this, &MainWindow::openVbt);

    dataMenu->addSeparator();

    auto *actHiPS = dataMenu->addAction(u"HiPS Viewer…"_s);
    actHiPS->setToolTip(u"Open the HiPS panoramic sky viewer (coming soon)."_s);
    QObject::connect(actHiPS, &QAction::triggered, this, &MainWindow::openHiPS);
}

void MainWindow::buildSampMenu()
{
    auto *sampMenu = new QMenu(u"SAMP"_s, this);
    this->menuBar()->insertMenu(ui->menuHelp->menuAction(), sampMenu);
    m_sampMenu = sampMenu;

    m_actSampConnect = sampMenu->addAction(u"Connect To Hub"_s);
    QObject::connect(m_actSampConnect, &QAction::triggered, this, [this]() {
        this->syncSampSettings();
        const auto health = this->m_sampBackendClient->health();
        if (!health.ok) {
            QMessageBox::critical(
                    this, u"Backend unavailable"_s,
                    u"Cannot connect SAMP integration because the backend is not reachable.\n\n"
                    u"Error: %1"_s.arg(health.error.isEmpty() ? u"No response"_s : health.error));
            return;
        }
        this->m_sampClient->connectToHub();
    });

    m_actSampDisconnect = sampMenu->addAction(u"Disconnect From Hub"_s);
    QObject::connect(m_actSampDisconnect, &QAction::triggered, this, [this]() {
        this->m_sampClient->disconnectFromHub();
    });

    sampMenu->addSeparator();

    m_actSampSendCurrentFits = sampMenu->addAction(u"Send Current FITS Dataset"_s);
    QObject::connect(m_actSampSendCurrentFits, &QAction::triggered,
                     this, &MainWindow::sendCurrentFitsViaSamp);

    m_actSampSendCurrentCatalogue = sampMenu->addAction(u"Send Current Catalogue"_s);
    QObject::connect(m_actSampSendCurrentCatalogue, &QAction::triggered,
                     this, &MainWindow::sendCurrentCatalogueViaSamp);
}

// ─────────────────────────────────────────────────────────────────────────────
// Science menu
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::buildScienceMenu()
{
    auto *sciMenu = new QMenu(u"Science"_s, this);
    this->menuBar()->insertMenu(ui->menuHelp->menuAction(), sciMenu);
    m_scienceMenu = sciMenu;

    m_actLinewidth = sciMenu->addAction(u"Line-Width Map…"_s);
    m_actLinewidth->setEnabled(false);
    m_actLinewidth->setToolTip(u"Compute FWHM / Equivalent-Width map (S-02). Open a dataset first."_s);
    QObject::connect(m_actLinewidth, &QAction::triggered, this, [this]() {
        auto *dlg = new LinewidthDialog(m_sciBackendUrl, m_sciToken,
                                        m_sciSessionId, m_sciDatasetId,
                                        m_sciDepth, m_sciThreshold,
                                        m_sciSpectralAxisType, m_sciSpectralAxisUnit,
                                        this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });

    m_actBaseline = sciMenu->addAction(u"Baseline Subtraction…"_s);
    m_actBaseline->setEnabled(false);
    m_actBaseline->setToolTip(u"Subtract a polynomial or median spectral baseline (S-03). Open a dataset first."_s);
    QObject::connect(m_actBaseline, &QAction::triggered, this, [this]() {
        auto *dlg = new BaselineDialog(m_sciBackendUrl, m_sciToken,
                                       m_sciSessionId, m_sciDatasetId,
                                       m_sciDepth, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });

    m_actStack = sciMenu->addAction(u"Spectral Stacking…"_s);
    m_actStack->setEnabled(true);  // no dataset needed upfront — user selects from list
    m_actStack->setToolTip(u"Stack multiple session datasets into a 1-D spectrum (S-04)."_s);
    QObject::connect(m_actStack, &QAction::triggered, this, [this]() {
        const QString url       = m_sciBackendUrl;
        const QString token     = m_sciToken;
        const QString sessionId = m_sciSessionId;
        const QString currentId = m_sciDatasetId;
        if (sessionId.isEmpty()) {
            QMessageBox::information(this, u"Spectral Stacking"_s,
                                     u"Open a dataset first to establish a backend session, "
                                     u"then open the additional cubes you want to stack."_s);
            return;
        }
        auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>(this);
        QPointer<MainWindow> self(this);
        QObject::connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished,
                         this, [self, watcher, url, token, sessionId, currentId]() {
                             const auto listing = watcher->result();
                             watcher->deleteLater();
                             if (!self) return;
                             if (!listing.valid) {
                                 QMessageBox::warning(self, u"Spectral Stacking"_s,
                                                      u"Could not list session datasets: %1"_s
                                                              .arg(listing.error));
                                 return;
                             }
                             std::vector<StackDialog::DatasetEntry> entries;
                             entries.reserve(listing.datasets.size());
                             for (const auto &d : listing.datasets) {
                                 const QString fileName = QFileInfo(d.path).fileName();
                                 const QString label = fileName.isEmpty() ? d.datasetId : fileName;
                                 entries.push_back({ d.datasetId, d.kind, label,
                                                     d.width, d.height, d.depth });
                             }
                             auto *client = new BackendClient(url, token);
                             client->setSessionId(sessionId);
                             auto *dlg = new StackDialog(client, sessionId, entries,
                                                         currentId, self);
                             QObject::connect(dlg, &QDialog::finished,
                                              [client](int) { delete client; });
                             dlg->setAttribute(Qt::WA_DeleteOnClose);
                             // Non-modal — consistent with the other tool dialogs.
                             dlg->show();
                         });
        watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
            BackendClient c(url, token);
            c.setSessionId(sessionId);
            return c.listSessionDatasets(sessionId);
        }));
    });

    m_actSourceFind = sciMenu->addAction(u"Source Finding (SoFiA-2)…"_s);
    m_actSourceFind->setEnabled(false);
    m_actSourceFind->setToolTip(u"Run SoFiA-2 on the current dataset (S-05). Open a dataset first."_s);
    QObject::connect(m_actSourceFind, &QAction::triggered, this, [this]() {
        auto *client = new BackendClient(m_sciBackendUrl, m_sciToken);
        auto *dlg = new SourceFindDialog(client, m_sciDatasetId, this);
        QObject::connect(dlg, &QObject::destroyed, this, [client]() { delete client; });
        // Open the SoFiA-2 mask cube in the 3D viewer (mask → 3D inspection of
        // the detections) on the same node backend that ran the source finder.
        const QString url = m_sciBackendUrl, token = m_sciToken;
        QObject::connect(dlg, &SourceFindDialog::openMaskCubeRequested, this,
                         [this, url, token](const QString &maskPath) {
                             this->openBackendProductFits(url, token, maskPath,
                                                          u"SoFiA mask cube"_s);
                         });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        // Non-modal — consistent with the other tool dialogs.
        dlg->show();
    });

    sciMenu->addSeparator();

    // ── Advanced 2-D products (SKAVA visualization use cases) ────────────
    m_actPubFigure = sciMenu->addAction(u"Publication Figure…"_s);
    m_actPubFigure->setEnabled(false);
    m_actPubFigure->setToolTip(u"Render a publication-quality figure with WCS grid + colorbar (UC 1.3). Open a dataset first."_s);
    QObject::connect(m_actPubFigure, &QAction::triggered, this, &MainWindow::runPublicationFigure);

    m_actImageQuality = sciMenu->addAction(u"Image Quality / Artifacts…"_s);
    m_actImageQuality->setEnabled(false);
    m_actImageQuality->setToolTip(u"Compute quality/artifact metrics: blanked %, dynamic range, striping, negative bowl (UC 4.2)."_s);
    QObject::connect(m_actImageQuality, &QAction::triggered, this, &MainWindow::runImageQuality);

    m_actPixelMath = sciMenu->addAction(u"Pixel Math / Spectral Index…"_s);
    m_actPixelMath->setEnabled(true);  // selects datasets from the session list
    m_actPixelMath->setToolTip(u"Pixel-wise math on one/two maps, incl. spectral index (UC 3.2)."_s);
    QObject::connect(m_actPixelMath, &QAction::triggered, this, &MainWindow::runPixelMath);

    m_actMosaic = sciMenu->addAction(u"Mosaic (noise-weighted)…"_s);
    m_actMosaic->setEnabled(true);
    m_actMosaic->setToolTip(u"Reproject + co-add multiple images into a noise-weighted mosaic (UC 3.3)."_s);
    QObject::connect(m_actMosaic, &QAction::triggered, this, &MainWindow::runMosaic);
}

// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::syncDataHubSettings()
{
    if (this->dataHub) {
        this->dataHub->setBackendUrl(this->settings->getBackendUrl(),
                                     this->settings->getBackendToken());
    }
}

void MainWindow::syncSampSettings()
{
    if (!this->m_sampBackendClient) {
        return;
    }
    this->m_sampBackendClient->setBaseUrl(this->settings->getBackendUrl());
    this->m_sampBackendClient->setToken(this->settings->getBackendToken());
}

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

void MainWindow::reopenDataset(const RecentDataset &dataset)
{
    // ── SKAVA-origin entries route through discovery on reopen ──
    // Re-querying SKAVA (rather than reusing the stale local path) lets
    // the operator's file move across nodes, or the visivo-backend at a
    // node be replaced, without breaking the Recent Datasets entry.
    if (dataset.source == u"skava"_s && !dataset.obsId.isEmpty()) {
        SkavaClient skava(this->settings->getSkavaUrl(),
                           this->settings->getSkavaToken(), this);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const auto datalink = skava.getDatalink(dataset.obsId);
        QApplication::restoreOverrideCursor();
        if (!datalink.valid) {
            QMessageBox::warning(
                    this, u"Reopen from SKAVA"_s,
                    u"Could not reopen '%1':\n\nSKAVA query failed: %2"_s
                            .arg(dataset.displayName,
                                  datalink.error.isEmpty()
                                          ? u"unknown error"_s : datalink.error));
            return;
        }
        doOpenSkavaDataset(dataset.obsId, datalink);
        return;
    }

    if (dataset.type == u"cube"_s || dataset.type == u"image"_s) {
        doOpenDataPath(dataset.id);
    } else if (dataset.type == u"catalogue"_s) {
        doOpenCataloguePath(dataset.id);
    } else if (dataset.type == u"vbt"_s || dataset.type == u"vbt-volume"_s) {
        doOpenVbtPath(dataset.id);
    } else if (dataset.type == u"hips"_s) {
        openHiPS();
    } else {
        QMessageBox::information(
                this, u"Cannot reopen"_s,
                QStringLiteral("Reopening '%1' (type: %2) is not yet supported.")
                        .arg(dataset.displayName, dataset.type));
    }
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

void MainWindow::openData()
{
    // All datasets are opened via the backend. The backend may be a localhost
    // instance or a remote server — that distinction is a deployment detail,
    // not a client-side mode. There is no "local FITS open" path.
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a dataset: the backend is not reachable.\n\n"
                u"Start a local backend or configure a remote backend URL in Settings, "
                u"then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString path = dialog.selectedFilePath();
    if (path.isEmpty()) {
        return;
    }

    doOpenDataPath(path);
}

namespace {

/// Try to resolve a SKAVA visivo-backend service descriptor against the
/// desktop's known-backends registry.
///
/// Resolution order:
///   1. exact URL match between descriptor.endpoint and a registered
///      BackendNode.url (after trimming trailing slashes)
///   2. matching SKAVA node_code against BackendNode.srcCode
///   3. empty (caller falls back to download via local backend)
BackendNode pickBackendForSkavaDataset(
        Settings *settings,
        const SkavaDatalinkResponse &datalink)
{
    if (!settings || datalink.visivoBackends.isEmpty()) {
        return {};
    }
    const auto registry = settings->getBackendNodes();
    const auto trim = [](QString s) {
        while (s.endsWith(u'/')) s.chop(1);
        return s;
    };
    for (const auto &desc : datalink.visivoBackends) {
        const QString descUrl = trim(desc.endpoint);
        // (1) exact URL match
        for (const auto &node : registry) {
            if (trim(node.url) == descUrl && !descUrl.isEmpty()) {
                return node;
            }
        }
        // (2) node_code match
        if (!desc.nodeCode.isEmpty()) {
            for (const auto &node : registry) {
                if (node.srcCode == desc.nodeCode) {
                    return node;
                }
            }
        }
    }
    return {};
}

}  // namespace

void MainWindow::doOpenSkavaDataset(const QString &obsId,
                                      const SkavaDatalinkResponse &datalink)
{
    // Backend-aware routing per docs/distributed-backend-design.md §4.2.
    const BackendNode chosen = pickBackendForSkavaDataset(this->settings, datalink);
    // ── Routing decision ────────────────────────────────────────────
    // pickBackendForSkavaDataset returns:
    //   * a remote BackendNode → compute-next-to-data via that node
    //   * the local BackendNode → still use compute-next-to-data, just
    //     against the local backend (no special-case needed — the
    //     desktop's "local" backend supports /v1/datasets/open_skava
    //     same as any remote one)
    //   * nothing → no SKAVA visivo-backend descriptor matched; we
    //     fall through to the download-via-local path below, which
    //     handles HTTP-served archives.
    // Strip ``file://`` from the access URL so downstream viewer code can
    // both call backend endpoints that expect a real path (fileHeader,
    // moment exports, etc.) and render a friendly window title via
    // QFileInfo(path).fileName(). Returns an empty string when the URL
    // isn't a file:// one — callers fall back to obs_id in that case.
    auto pathFromFileUrl = [](const QString &url) -> QString {
        if (!url.startsWith(QStringLiteral("file://"))) {
            return QString();
        }
        QString tail = url.mid(7);  // strip "file://"
        // Allow file://localhost/path → /path
        if (tail.startsWith(QStringLiteral("localhost/"))) {
            tail = tail.mid(9);  // strip "localhost"
        }
        return QUrl::fromPercentEncoding(tail.toUtf8());
    };
    const QString fsPath = pathFromFileUrl(datalink.primaryAccessUrl);

    if (!chosen.id.isEmpty()) {
        this->setActiveBackendBadge(chosen);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        BackendClient client(chosen.url, chosen.token);
        const auto opened = client.openSkavaDataset(obsId, datalink.primaryAccessUrl);
        QApplication::restoreOverrideCursor();
        if (!opened.valid) {
            qWarning().noquote() << QStringLiteral(
                    "[skava] remote backend %1 refused open: %2 — falling back to local download")
                    .arg(chosen.id, opened.error);
            // Revert the badge: the remote refused, so the imminent
            // download/open below is served by the local backend.
            this->setActiveBackendBadge(this->settings->getLocalBackend());
            // Fall through to the local-download path so the user still
            // gets the dataset rather than a hard failure.
        } else {
            // Prefer the real filesystem path so window title shows the
            // FITS basename and "View FITS header" works without round-
            // tripping the backend through an obs_id string.
            const QString displayPath = !fsPath.isEmpty()
                    ? fsPath
                    : (obsId.isEmpty() ? datalink.primaryAccessUrl : obsId);
            openViewerFromOpenedDataset(displayPath, opened, client);
            // Re-record the just-opened dataset as a SKAVA entry: the
            // generic ``openViewerFromOpenedDataset`` recorded it under
            // its path with source=remote, but we want source=skava so
            // the DataHub badge fires and the next reopen routes through
            // the discovery flow instead of a dead local path.
            if (this->recentMgr) {
                this->recentMgr->remove(displayPath);
                const QString name = !fsPath.isEmpty()
                        ? QFileInfo(fsPath).fileName() : displayPath;
                this->recentMgr->record(
                        QStringLiteral("skava:") + obsId,
                        name, opened.kind, u"skava"_s, obsId);
            }
            return;
        }
    }
    // ── Fallback: download via local backend cache → open ──────────────
    doOpenDataFromUrl(datalink.primaryAccessUrl, obsId);
}

void MainWindow::doSkavaSodaSubset(const QString &obsId,
                                     const SkavaDatalinkResponse &datalink)
{
    // The cutout must run where the data lives: pick the same co-located
    // backend that would open the dataset. A SODA subset only makes sense
    // when that backend exists AND serves a file:// replica it can open in
    // place — otherwise there is nothing to compute next to.
    const BackendNode chosen = pickBackendForSkavaDataset(this->settings, datalink);
    const bool isFileUrl =
            datalink.primaryAccessUrl.startsWith(QStringLiteral("file://"));
    if (chosen.id.isEmpty() || !isFileUrl) {
        QMessageBox::information(
                this, u"SODA subset unavailable"_s,
                u"This dataset has no co-located VisIVO backend serving a "
                u"file:// replica, so a server-side SODA cutout cannot run.\n\n"
                u"Use “Open selected in VisIVO” to download and open the full "
                u"dataset instead."_s);
        return;
    }

    // ── Region dialog (POS + BAND), prefilled from the dataset metadata ──
    const SkavaDatasetMetadata &m = datalink.dataset;
    QDialog dlg(this);
    dlg.setWindowTitle(u"SODA subset — %1"_s.arg(obsId));
    auto *form = new QFormLayout(&dlg);
    auto mkEdit = [&](double value, const QString &placeholder) {
        auto *e = new QLineEdit(&dlg);
        e->setPlaceholderText(placeholder);
        e->setValidator(new QDoubleValidator(&dlg));
        if (std::isfinite(value)) e->setText(QString::number(value, 'g', 10));
        return e;
    };
    auto *raEdit   = mkEdit(m.sRa,  u"RA (deg)"_s);
    auto *decEdit  = mkEdit(m.sDec, u"Dec (deg)"_s);
    auto *radEdit  = mkEdit(std::isfinite(m.sFov) ? m.sFov * 0.5 : 0.1,
                            u"radius (deg)"_s);
    auto *bMinEdit = mkEdit(m.emMin, u"min (m)"_s);
    auto *bMaxEdit = mkEdit(m.emMax, u"max (m)"_s);
    form->addRow(u"POS — RA (deg)"_s, raEdit);
    form->addRow(u"Dec (deg)"_s, decEdit);
    form->addRow(u"Radius (deg)"_s, radEdit);
    form->addRow(u"BAND — min (m)"_s, bMinEdit);
    form->addRow(u"BAND — max (m)"_s, bMaxEdit);
    auto *hint = new QLabel(
            u"Leave all POS fields or both BAND fields blank to omit that "
            u"axis. At least one of POS / BAND is required. The cutout runs on "
            u"node %1."_s.arg(chosen.name.isEmpty() ? chosen.id : chosen.name),
            &dlg);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Run cutout"_s);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    // ── Build the SODA POS / BAND strings ────────────────────────────────
    auto readNum = [](QLineEdit *e, double &out) {
        const QString t = e->text().trimmed();
        if (t.isEmpty()) return false;
        bool ok = false;
        out = t.toDouble(&ok);
        return ok;
    };
    double ra = 0, dec = 0, rad = 0, bMin = 0, bMax = 0;
    QString pos, band;
    if (readNum(raEdit, ra) && readNum(decEdit, dec) && readNum(radEdit, rad)) {
        pos = u"CIRCLE %1 %2 %3"_s.arg(QString::number(ra, 'g', 10),
                                        QString::number(dec, 'g', 10),
                                        QString::number(rad, 'g', 10));
    }
    if (readNum(bMinEdit, bMin) && readNum(bMaxEdit, bMax)) {
        band = u"%1 %2"_s.arg(QString::number(std::min(bMin, bMax), 'g', 10),
                               QString::number(std::max(bMin, bMax), 'g', 10));
    }
    if (pos.isEmpty() && band.isEmpty()) {
        QMessageBox::information(
                this, u"SODA subset"_s,
                u"Provide a POS (RA/Dec/radius) and/or a BAND (min/max) to "
                u"subset."_s);
        return;
    }

    // ── Run the cutout on the chosen backend, then open it in place ──────
    this->setActiveBackendBadge(chosen);
    const QString accessUrl = datalink.primaryAccessUrl;
    const QString url = chosen.url;
    const QString token = chosen.token;

    auto *watcher = new QFutureWatcher<BackendSodaCutoutResult>(this);
    QObject::connect(
            watcher, &QFutureWatcher<BackendSodaCutoutResult>::finished, this,
            [this, watcher, url, token, obsId]() {
                QApplication::restoreOverrideCursor();
                watcher->deleteLater();
                const auto res = watcher->result();
                if (!res.valid) {
                    QMessageBox::critical(
                            this, u"SODA subset failed"_s,
                            res.error.isEmpty() ? u"The backend returned no detail."_s
                                                : res.error);
                    return;
                }
                // The cutout lives on the backend filesystem — open it there,
                // never downloading it to the desktop (compute next to data).
                BackendClient client(url, token);
                const auto opened = client.openDataset(res.path);
                if (!opened.valid) {
                    QMessageBox::critical(
                            this, u"Could not open SODA cutout"_s,
                            opened.error.isEmpty()
                                    ? u"Backend returned no error detail."_s
                                    : opened.error);
                    return;
                }
                openViewerFromOpenedDataset(res.path, opened, client);
                if (this->recentMgr) {
                    this->recentMgr->record(res.path,
                                            u"%1 [SODA cutout]"_s.arg(obsId),
                                            opened.kind);
                }
            });

    QApplication::setOverrideCursor(Qt::WaitCursor);
    watcher->setFuture(QtConcurrent::run(
            [url, token, obsId, accessUrl, pos, band]() {
                BackendClient bc(url, token);
                return bc.sodaCutout(obsId, accessUrl, pos, band, QString());
            }));
}

// ── Advanced 2-D product use cases (UC 1.3 / 4.2 / 3.2 / 3.3) ───────────────

void MainWindow::openBackendProductFits(const QString &backendUrl, const QString &backendToken,
                                          const QString &path, const QString &displayName)
{
    BackendClient client(backendUrl, backendToken);
    const auto opened = client.openDataset(path);
    if (!opened.valid) {
        QMessageBox::critical(
                this, u"Could not open product"_s,
                opened.error.isEmpty() ? u"Backend returned no error detail."_s : opened.error);
        return;
    }
    openViewerFromOpenedDataset(path, opened, client);
    if (this->recentMgr) this->recentMgr->record(path, displayName, opened.kind);
}

void MainWindow::runPublicationFigure()
{
    if (m_sciDatasetId.isEmpty()) {
        QMessageBox::information(this, u"Publication Figure"_s, u"Open a dataset first."_s);
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(u"Publication Figure"_s);
    auto *form = new QFormLayout(&dlg);
    auto *stretch = new QComboBox(&dlg);
    stretch->addItems(QStringList{ u"linear"_s, u"log"_s, u"sqrt"_s, u"asinh"_s, u"power"_s });
    auto *cmap = new QComboBox(&dlg);
    cmap->addItems(QStringList{ u"inferno"_s, u"viridis"_s, u"magma"_s, u"gray"_s, u"hot"_s, u"cubehelix"_s });
    auto *title = new QLineEdit(&dlg);
    auto *grid = new QCheckBox(u"WCS sky grid"_s, &dlg); grid->setChecked(true);
    auto *cbar = new QCheckBox(u"Colorbar (BUNIT)"_s, &dlg); cbar->setChecked(true);
    form->addRow(u"Stretch"_s, stretch);
    form->addRow(u"Colormap"_s, cmap);
    form->addRow(u"Title"_s, title);
    form->addRow(QString(), grid);
    form->addRow(QString(), cbar);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Render"_s);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString url = m_sciBackendUrl, token = m_sciToken, ds = m_sciDatasetId;
    const QString sessionId = m_sciSessionId;
    const QString s = stretch->currentText(), cm = cmap->currentText(), t = title->text();
    const bool g = grid->isChecked(), c = cbar->isChecked();
    auto *watcher = new QFutureWatcher<BackendPublicationFigureResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendPublicationFigureResult>::finished, this,
                     [this, watcher]() {
        QApplication::restoreOverrideCursor();
        watcher->deleteLater();
        const auto r = watcher->result();
        if (!r.valid) {
            QMessageBox::critical(this, u"Publication Figure failed"_s,
                    r.error.isEmpty() ? u"No detail."_s : r.error);
            return;
        }
        const auto choice = QMessageBox::information(
                this, u"Publication Figure"_s,
                u"Figure saved on the backend:\n%1\n\nOpen it now? (works when the "
                u"backend shares this machine's filesystem)"_s.arg(r.outputPath),
                QMessageBox::Open | QMessageBox::Close, QMessageBox::Open);
        if (choice == QMessageBox::Open)
            QDesktopServices::openUrl(QUrl::fromLocalFile(r.outputPath));
    });
    QApplication::setOverrideCursor(Qt::WaitCursor);
    watcher->setFuture(QtConcurrent::run([url, token, sessionId, ds, s, cm, t, g, c]() {
        BackendClient bc(url, token);
        bc.setSessionId(sessionId);
        return bc.publicationFigure(ds, s, t, g, c, cm, 200);
    }));
}

void MainWindow::runImageQuality()
{
    if (m_sciDatasetId.isEmpty()) {
        QMessageBox::information(this, u"Image Quality"_s, u"Open a dataset first."_s);
        return;
    }
    const QString url = m_sciBackendUrl, token = m_sciToken, ds = m_sciDatasetId;
    const QString sessionId = m_sciSessionId;
    auto *watcher = new QFutureWatcher<BackendImageQualityResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendImageQualityResult>::finished, this,
                     [this, watcher]() {
        QApplication::restoreOverrideCursor();
        watcher->deleteLater();
        const auto r = watcher->result();
        if (!r.valid) {
            QMessageBox::critical(this, u"Image Quality failed"_s,
                    r.error.isEmpty() ? u"No detail."_s : r.error);
            return;
        }
        const QStringList order = {
            u"total_pixels"_s, u"blanked_fraction"_s, u"zero_fraction"_s,
            u"negative_fraction"_s, u"min"_s, u"max"_s, u"mean"_s, u"median"_s,
            u"robust_std"_s, u"dynamic_range"_s, u"striping_metric"_s,
            u"negative_bowl"_s };
        QString text;
        for (const QString &k : order) {
            if (!r.metrics.contains(k)) continue;
            text += u"%1 : %2\n"_s.arg(k, -18).arg(r.metrics.value(k).toString());
        }
        const QStringList flags = r.metrics.value(u"flags"_s).toStringList();
        text += u"\nflags : %1"_s.arg(flags.isEmpty() ? u"(none — looks clean)"_s
                                                       : flags.join(u", "_s));
        QDialog box(this);
        box.setWindowTitle(u"Image Quality / Artifacts"_s);
        auto *lay = new QVBoxLayout(&box);
        auto *edit = new QPlainTextEdit(&box);
        edit->setReadOnly(true);
        edit->setPlainText(text);
        edit->setMinimumSize(380, 280);
        lay->addWidget(edit);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &box);
        QObject::connect(bb, &QDialogButtonBox::rejected, &box, &QDialog::reject);
        QObject::connect(bb, &QDialogButtonBox::accepted, &box, &QDialog::accept);
        lay->addWidget(bb);
        box.exec();
    });
    QApplication::setOverrideCursor(Qt::WaitCursor);
    watcher->setFuture(QtConcurrent::run([url, token, sessionId, ds]() {
        BackendClient bc(url, token);
        bc.setSessionId(sessionId);
        return bc.imageQuality(ds);
    }));
}

void MainWindow::runPixelMath()
{
    if (m_sciSessionId.isEmpty()) {
        QMessageBox::information(this, u"Pixel Math"_s,
                u"Open a dataset first to establish a backend session, then open the "
                u"map(s) you want to combine."_s);
        return;
    }
    const QString url = m_sciBackendUrl, token = m_sciToken, sessionId = m_sciSessionId;
    const QString currentId = m_sciDatasetId;
    auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>(this);
    QPointer<MainWindow> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished, this,
                     [self, watcher, url, token, sessionId, currentId]() {
        const auto listing = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        if (!listing.valid) {
            QMessageBox::warning(self, u"Pixel Math"_s,
                    u"Could not list session datasets: %1"_s.arg(listing.error));
            return;
        }
        QDialog dlg(self);
        dlg.setWindowTitle(u"Pixel Math / Spectral Index"_s);
        auto *form = new QFormLayout(&dlg);
        auto *comboA = new QComboBox(&dlg);
        auto *comboB = new QComboBox(&dlg);
        for (const auto &d : listing.datasets) {
            const QString label = QFileInfo(d.path).fileName().isEmpty()
                    ? d.datasetId : QFileInfo(d.path).fileName();
            comboA->addItem(label, d.datasetId);
            comboB->addItem(label, d.datasetId);
        }
        const int curIdx = comboA->findData(currentId);
        if (curIdx >= 0) comboA->setCurrentIndex(curIdx);
        if (comboB->count() > 1) comboB->setCurrentIndex(comboA->currentIndex() == 0 ? 1 : 0);
        auto *op = new QComboBox(&dlg);
        op->addItems(QStringList{ u"ratio"_s, u"divide"_s, u"subtract"_s, u"add"_s,
                                   u"multiply"_s, u"spectral_index"_s, u"log10"_s,
                                   u"abs"_s, u"square"_s });
        auto *freqA = new QLineEdit(&dlg); freqA->setPlaceholderText(u"freq A (Hz)"_s);
        freqA->setValidator(new QDoubleValidator(&dlg));
        auto *freqB = new QLineEdit(&dlg); freqB->setPlaceholderText(u"freq B (Hz)"_s);
        freqB->setValidator(new QDoubleValidator(&dlg));
        form->addRow(u"Map A"_s, comboA);
        form->addRow(u"Map B"_s, comboB);
        form->addRow(u"Operation"_s, op);
        form->addRow(u"Freq A (spectral index)"_s, freqA);
        form->addRow(u"Freq B (spectral index)"_s, freqB);
        auto syncEnabled = [&]() {
            const QString o = op->currentText();
            const bool unary = (o == u"log10"_s || o == u"abs"_s || o == u"square"_s);
            const bool si = (o == u"spectral_index"_s);
            comboB->setEnabled(!unary);
            freqA->setEnabled(si);
            freqB->setEnabled(si);
        };
        QObject::connect(op, &QComboBox::currentTextChanged, &dlg, [syncEnabled](const QString &){ syncEnabled(); });
        syncEnabled();
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(u"Compute"_s);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;

        const QString idA = comboA->currentData().toString();
        const QString o = op->currentText();
        const bool unary = (o == u"log10"_s || o == u"abs"_s || o == u"square"_s);
        const QString idB = unary ? QString() : comboB->currentData().toString();
        const double fA = freqA->text().toDouble();
        const double fB = freqB->text().toDouble();

        auto *w2 = new QFutureWatcher<BackendProductFitsResult>(self);
        QObject::connect(w2, &QFutureWatcher<BackendProductFitsResult>::finished, self,
                         [self, w2, url, token, o]() {
            QApplication::restoreOverrideCursor();
            w2->deleteLater();
            if (!self) return;
            const auto r = w2->result();
            if (!r.valid) {
                QMessageBox::critical(self, u"Pixel Math failed"_s,
                        r.error.isEmpty() ? u"No detail."_s : r.error);
                return;
            }
            self->openBackendProductFits(url, token, r.outputPath, u"Pixel math: %1"_s.arg(o));
        });
        QApplication::setOverrideCursor(Qt::WaitCursor);
        w2->setFuture(QtConcurrent::run([url, token, sessionId, idA, o, idB, fA, fB]() {
            BackendClient bc(url, token);
            bc.setSessionId(sessionId);  // dataset_ids live in this session
            return bc.mapMath(idA, o, idB, fA, fB);
        }));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
        BackendClient c(url, token);
        c.setSessionId(sessionId);
        return c.listSessionDatasets(sessionId);
    }));
}

void MainWindow::runMosaic()
{
    if (m_sciSessionId.isEmpty()) {
        QMessageBox::information(this, u"Mosaic"_s,
                u"Open a dataset first to establish a backend session, then open the "
                u"images you want to mosaic."_s);
        return;
    }
    const QString url = m_sciBackendUrl, token = m_sciToken, sessionId = m_sciSessionId;
    auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>(this);
    QPointer<MainWindow> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished, this,
                     [self, watcher, url, token, sessionId]() {
        const auto listing = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        if (!listing.valid) {
            QMessageBox::warning(self, u"Mosaic"_s,
                    u"Could not list session datasets: %1"_s.arg(listing.error));
            return;
        }
        QDialog dlg(self);
        dlg.setWindowTitle(u"Mosaic (noise-weighted)"_s);
        auto *lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel(u"Select two or more images to mosaic:"_s, &dlg));
        auto *list = new QListWidget(&dlg);
        list->setSelectionMode(QAbstractItemView::MultiSelection);
        for (const auto &d : listing.datasets) {
            const QString label = QFileInfo(d.path).fileName().isEmpty()
                    ? d.datasetId : QFileInfo(d.path).fileName();
            auto *item = new QListWidgetItem(label, list);
            item->setData(Qt::UserRole, d.datasetId);
        }
        lay->addWidget(list);
        auto *weight = new QComboBox(&dlg);
        weight->addItem(u"Noise-weighted (1/σ²)"_s, u"noise"_s);
        weight->addItem(u"Uniform"_s, u"uniform"_s);
        auto *wrow = new QFormLayout;
        wrow->addRow(u"Weighting"_s, weight);
        lay->addLayout(wrow);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(u"Build mosaic"_s);
        lay->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;

        QStringList ids;
        for (QListWidgetItem *it : list->selectedItems())
            ids << it->data(Qt::UserRole).toString();
        if (ids.size() < 2) {
            QMessageBox::information(self, u"Mosaic"_s, u"Select at least two images."_s);
            return;
        }
        const QString wmode = weight->currentData().toString();

        auto *w2 = new QFutureWatcher<BackendProductFitsResult>(self);
        QObject::connect(w2, &QFutureWatcher<BackendProductFitsResult>::finished, self,
                         [self, w2, url, token]() {
            QApplication::restoreOverrideCursor();
            w2->deleteLater();
            if (!self) return;
            const auto r = w2->result();
            if (!r.valid) {
                QMessageBox::critical(self, u"Mosaic failed"_s,
                        r.error.isEmpty() ? u"No detail."_s : r.error);
                return;
            }
            self->openBackendProductFits(url, token, r.outputPath, u"Mosaic"_s);
        });
        QApplication::setOverrideCursor(Qt::WaitCursor);
        w2->setFuture(QtConcurrent::run([url, token, sessionId, ids, wmode]() {
            BackendClient bc(url, token);
            bc.setSessionId(sessionId);  // dataset_ids live in this session
            return bc.mosaic(ids, wmode);
        }));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
        BackendClient c(url, token);
        c.setSessionId(sessionId);
        return c.listSessionDatasets(sessionId);
    }));
}

void MainWindow::doOpenDataFromUrl(const QString &url, const QString &obsId)
{
    if (url.trimmed().isEmpty()) {
        QMessageBox::warning(this, u"SKAVA"_s,
                u"No access URL was returned by SKAVA for this dataset."_s);
        return;
    }
    this->setActiveBackendBadge(this->settings->getLocalBackend());
    BackendClient client(this->settings->getBackendUrl(),
                          this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(this, u"Backend unavailable"_s,
                u"Cannot download the SKAVA dataset: the VisIVO backend is "
                u"not reachable.\n\nError: %1"_s.arg(
                        health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto opened = client.openDatasetFromUrl(url, obsId);
    QApplication::restoreOverrideCursor();
    if (!opened.valid) {
        QMessageBox::critical(this, u"Open from SKAVA"_s,
                u"Could not open the dataset:\n\n%1"_s.arg(
                        opened.error.isEmpty() ? u"unknown error"_s : opened.error));
        return;
    }
    // The viewer wants a displayable path — for SKAVA-originated opens we
    // use the obs_id so the window title, Recent Datasets entry, SAMP
    // metadata etc. carry meaningful labels. The actual data is pulled
    // by the viewer via opened.datasetId (already session-registered).
    const QString displayPath = obsId.isEmpty() ? url : obsId;
    openViewerFromOpenedDataset(displayPath, opened, client);
}

void MainWindow::doOpenDataPath(const QString &path)
{
    this->setActiveBackendBadge(this->settings->getLocalBackend());
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a dataset: the backend is not reachable.\n\n"
                u"Error: %1"_s.arg(health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }

    const auto opened = client.openDataset(path);
    if (!opened.valid) {
        QMessageBox::critical(
                this, u"Could not open dataset"_s,
                u"The backend could not open this dataset.\n\n%1\n\n"
                u"Supported formats: FITS (image, cube). For HDF5, Gadget, VOTable, "
                u"or simulation data, convert with VisIVOServer first."_s.arg(
                        opened.error.isEmpty() ? u"Backend returned no error detail."_s
                                               : opened.error));
        return;
    }
    openViewerFromOpenedDataset(path, opened, client);
}

void MainWindow::openViewerFromOpenedDataset(const QString &path,
                                              const BackendOpenDatasetResult &opened,
                                              const BackendClient &client)
{
    QWidget *win = nullptr;
    if (opened.kind == u"image"_s || opened.kind == u"dynspec"_s) {
        const bool isDynspec = opened.kind == u"dynspec"_s;
        qDebug().noquote()
                << QStringLiteral("[fits] active_axes=%1 -> opening as %2 | %3")
                           .arg(opened.activeAxes)
                           .arg(isDynspec ? QStringLiteral("dynamic spectrum")
                                          : QStringLiteral("2D image"))
                           .arg(opened.degenerateAxesSummary.isEmpty()
                                        ? QStringLiteral("no collapsed axes")
                                        : opened.degenerateAxesSummary);
        auto *imgWin = new vtkWindowImage(path, client.baseUrl(), opened.datasetId,
                                 opened.ctype, opened.cunit, opened.crval, opened.crpix,
                                 opened.cdelt, opened.degenerateAxesSummary,
                                 opened.wcsStatus, opened.wcsWarningMessage,
                                 client.sessionId(), client.token(), this);
        if (!isDynspec) {
            imgWin->setBeamInfo(opened.beamMajorDeg, opened.beamMinorDeg,
                                opened.beamPaDeg);
        }
        imgWin->setDynamicSpectrumMode(isDynspec);
        win = imgWin;
    } else if (opened.kind == u"cube"_s) {
        qDebug().noquote()
                << QStringLiteral("[fits] active_axes=%1 -> opening as cube | %2")
                           .arg(opened.activeAxes)
                           .arg(opened.degenerateAxesSummary.isEmpty()
                                        ? QStringLiteral("no collapsed axes")
                                        : opened.degenerateAxesSummary);
        auto *cubeWin = new vtkWindowCube(path, client.baseUrl(), opened.datasetId, opened.width,
                                          opened.height, opened.depth, opened.spacing,
                                          opened.origin, opened.ctype, opened.cunit, opened.crval,
                                          opened.crpix, opened.cdelt, opened.degenerateAxesSummary,
                                          opened.spectralAxisType, opened.spectralAxisUnit,
                                          opened.wcsStatus, opened.wcsWarningMessage,
                                          client.sessionId(), client.token(),
                                          // Pattern B: propagate the backend's
                                          // size-aware loading policy so the
                                          // viewer can skip the auto full-res
                                          // fetch for cubes above the threshold.
                                          opened.recommendedLoadMode,
                                          opened.fileSizeBytes,
                                          opened.fullResThresholdBytes,
                                          this);
        cubeWin->setDarkPlots(this->settings->getPlotTheme() == 0);
        cubeWin->setBeamInfo(opened.beamMajorDeg, opened.beamMinorDeg,
                             opened.beamPaDeg);
        // Live refresh: when this cube viewer pushes a new FITS into the
        // Workspace Exports, the Data Hub panel updates within frames of
        // the operation instead of on the next backend health tick.
        if (this->dataHub) {
            QObject::connect(cubeWin, &vtkWindowCube::workspaceExportsChanged,
                             this->dataHub, &DataHubWidget::refreshWorkspaceExports);
        }
        // Cube → Image contour relay: when the cube viewer emits
        // contourDataReady, find the first open image viewer and forward.
        QObject::connect(cubeWin, &vtkWindowCube::contourDataReady,
                         this, [](const QString &label, vtkImageData *data) {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *img = qobject_cast<vtkWindowImage *>(w)) {
                    img->overlayExternalContourData(label, data);
                    return;
                }
            }
        });
        win = cubeWin;
    } else {
        QMessageBox::information(
                this, u"Unsupported dataset"_s,
                u"The backend reported an unsupported dataset kind: '%1'.\n\n"
                u"Only FITS images and spectral cubes are supported."_s.arg(opened.kind));
        return;
    }

    this->m_sampCurrentFitsPath = path;
    this->m_sampCurrentFitsName = QFileInfo(path).fileName();
    this->m_sampCurrentCataloguePath.clear();
    this->m_sampCurrentCatalogueName.clear();
    this->m_sampCurrentCatalogueFormat.clear();
    this->updateSampActions();

    // Record in recent datasets (uses the final kind returned by the backend).
    if (this->recentMgr) {
        const QString name = QFileInfo(path).fileName();
        this->recentMgr->record(path, name, opened.kind);
    }

    // ── Science menu context ──────────────────────────────────────────────────
    // Keep last-opened dataset info so Science menu actions can use it.
    m_sciBackendUrl        = client.baseUrl();
    m_sciSessionId         = client.sessionId();
    m_sciToken             = client.token();
    m_sciDatasetId         = opened.datasetId;
    m_sciDepth             = opened.depth > 0 ? opened.depth : 1;
    m_sciSpectralAxisType  = opened.spectralAxisType;
    m_sciSpectralAxisUnit  = opened.spectralAxisUnit;
    if (m_actLinewidth)   m_actLinewidth->setEnabled(true);
    if (m_actBaseline)    m_actBaseline->setEnabled(true);
    if (m_actSourceFind)  m_actSourceFind->setEnabled(true);
    if (m_actPubFigure)    m_actPubFigure->setEnabled(true);
    if (m_actImageQuality) m_actImageQuality->setEnabled(true);

    win->showMaximized();
    win->raise();
    win->activateWindow();
}

// ── VLKB image layer open ─────────────────────────────────────────────────────

void MainWindow::openVlkbImageLayer(const QString &backendPath, const QString &displayName,
                                     VLKBInventoryTree *tree)
{
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open VLKB dataset: the backend is not reachable.\n\nError: %1"_s.arg(
                        health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }

    const auto opened = client.openDataset(backendPath);
    if (!opened.valid) {
        QMessageBox::critical(
                this, u"Could not open VLKB dataset"_s,
                (opened.error.isEmpty() ? u"Backend returned no error detail."_s : opened.error));
        return;
    }

    if (opened.kind != u"image"_s) {
        // Not a 2D image — fall back to the generic open so cubes land in the right viewer.
        doOpenDataPath(backendPath);
        return;
    }

    auto *win = new vtkWindowImage(backendPath, client.baseUrl(), opened.datasetId,
                                    opened.ctype, opened.cunit, opened.crval, opened.crpix,
                                    opened.cdelt, opened.degenerateAxesSummary,
                                    opened.wcsStatus, opened.wcsWarningMessage,
                                    client.sessionId(), client.token(), this);
    // Override the master layer name with the human-readable inventory label.
    win->setLayerDisplayName(0, displayName);

    // Associate this window with the inventory tree so subsequent 2D items from
    // the same session are stacked as additional layers.
    if (tree) tree->setLinkedImageWindow(win);

    this->m_sampCurrentFitsPath = backendPath;
    this->m_sampCurrentFitsName = displayName;
    this->m_sampCurrentCataloguePath.clear();
    this->m_sampCurrentCatalogueName.clear();
    this->m_sampCurrentCatalogueFormat.clear();
    this->updateSampActions();

    // Record in recent datasets using the display name, not the uuid path.
    if (this->recentMgr)
        this->recentMgr->record(backendPath, displayName, u"image"_s);

    win->showMaximized();
    win->raise();
    win->activateWindow();
}

void MainWindow::openCatalogue3D()
{
    BackendClient client(settings->getBackendUrl(), settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a catalogue: the backend is not reachable.\n\n"
                u"Start a local backend or configure a remote backend URL in Settings, "
                u"then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    dialog.setExtensionFilter({u".csv"_s, u".vot"_s, u".votable"_s, u".xml"_s,
                               u".fits"_s, u".fit"_s},
                              u"Catalogue tables"_s);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString filepath = dialog.selectedFilePath();
    if (filepath.isEmpty())
        return;

    doOpenCataloguePath(filepath);
}

void MainWindow::doOpenCataloguePath(const QString &filepath)
{
    this->m_sampCurrentFitsPath.clear();
    this->m_sampCurrentFitsName.clear();
    this->m_sampCurrentCataloguePath = filepath;
    this->m_sampCurrentCatalogueName = QFileInfo(filepath).fileName();
    this->m_sampCurrentCatalogueFormat = catalogueFormatForPath(filepath);
    this->updateSampActions();
    auto *win = new vtkWindowCatalogue3D(filepath, settings->getBackendUrl(),
                                         settings->getBackendToken(), this);
    win->setAttribute(Qt::WA_DeleteOnClose);
    // On session expiry the window emits reopenRequested; reuse the normal open flow.
    QObject::connect(win, &vtkWindowCatalogue3D::reopenRequested,
                     this, &MainWindow::doOpenCataloguePath);
    win->showMaximized();
    win->raise();
    win->activateWindow();

    if (this->recentMgr) {
        this->recentMgr->record(filepath, QFileInfo(filepath).fileName(), u"catalogue"_s);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// VBT open task – runs entirely on a background thread
// ─────────────────────────────────────────────────────────────────────────────

struct VbtOpenTaskResult
{
    bool valid{ false };
    QString error;
    BackendVbtOpenResult openResult;
    BackendVbtSubsetResult subsetResult;
    QString backendUrl;
    QString backendToken;
    QString sessionId;
};

static VbtOpenTaskResult runVbtOpenTask(const QString &headerPath,
                                        const QString &backendUrl,
                                        const QString &backendToken)
{
    VbtOpenTaskResult out;
    out.backendUrl = backendUrl;
    out.backendToken = backendToken;

    BackendClient client(backendUrl, backendToken);

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
            QStringLiteral("Opening VBT: %1").arg(headerPath), QString(), QString(),
            QStringLiteral("open"));

    out.openResult = client.openVbt(headerPath);
    if (!out.openResult.valid) {
        out.error = out.openResult.error.isEmpty()
                ? QStringLiteral("The backend could not open this VBT dataset.")
                : out.openResult.error;
        DiagnosticsManager::instance()->publish(
                DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                QStringLiteral("Open failed: %1").arg(out.error), QString(), QString(),
                QStringLiteral("open"));
        return out;
    }

    out.sessionId = client.sessionId();

    if (out.openResult.kind == QStringLiteral("volume")) {
        const QString firstField = out.openResult.fields.empty()
                ? QString()
                : out.openResult.fields[0].name;
        out.subsetResult = client.requestVbtSubset(out.openResult.datasetId, 0, firstField);
        if (!out.subsetResult.valid) {
            out.error = out.subsetResult.error.isEmpty()
                    ? QStringLiteral("Backend returned no volume data.")
                    : out.subsetResult.error;
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                    QStringLiteral("Volume subset failed: %1").arg(out.error),
                    out.openResult.datasetId, out.sessionId, QStringLiteral("open"));
            return out;
        }
    } else {
        out.subsetResult = client.requestVbtSubset(out.openResult.datasetId);
        if (!out.subsetResult.valid) {
            out.error = out.subsetResult.error.isEmpty()
                    ? QStringLiteral("Backend returned no table data.")
                    : out.subsetResult.error;
            DiagnosticsManager::instance()->publish(
                    DiagnosticsLevel::Warning, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
                    QStringLiteral("Table subset failed: %1").arg(out.error),
                    out.openResult.datasetId, out.sessionId, QStringLiteral("open"));
            return out;
        }
    }

    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote, QStringLiteral("VBT"),
            QStringLiteral("Opened %1 (%2 rows, %3 fields).")
                    .arg(out.openResult.kind)
                    .arg(out.openResult.numRows)
                    .arg(out.openResult.fields.size()),
            out.openResult.datasetId, out.sessionId, QStringLiteral("open"));

    out.valid = true;
    return out;
}

void MainWindow::openVbt()
{
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();

    BackendClient client(backendUrl, backendToken);
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a VBT dataset: the backend is not reachable.\n\n"
                u"Start a local backend or configure a remote backend URL in Settings, "
                u"then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), client.token(), this);
    dialog.setExtensionFilter({u".bin.head"_s}, u"VBT files only"_s);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString headerPath = dialog.selectedFilePath();
    if (headerPath.isEmpty()) {
        return;
    }

    doOpenVbtPath(headerPath);
}

void MainWindow::doOpenVbtPath(const QString &headerPath)
{
    this->m_sampCurrentFitsPath.clear();
    this->m_sampCurrentFitsName.clear();
    this->m_sampCurrentCataloguePath.clear();
    this->m_sampCurrentCatalogueName.clear();
    this->m_sampCurrentCatalogueFormat.clear();
    this->updateSampActions();
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();

    // Disable the action while loading to prevent double-submit
    if (ui->actionOpenVbt) {
        ui->actionOpenVbt->setEnabled(false);
    }
    this->statusBar()->showMessage(u"Opening VBT dataset…"_s);

    auto *watcher = new QFutureWatcher<VbtOpenTaskResult>(this);
    QObject::connect(
            watcher, &QFutureWatcher<VbtOpenTaskResult>::finished, this,
            [this, watcher, headerPath]() {
                watcher->deleteLater();

                if (ui->actionOpenVbt) {
                    ui->actionOpenVbt->setEnabled(true);
                }
                this->statusBar()->clearMessage();

                const auto result = watcher->result();
                if (!result.valid) {
                    QMessageBox::critical(this, u"Could not open VBT"_s, result.error);
                    return;
                }

                QWidget *win = nullptr;
                const QString vbtType = result.openResult.kind == u"volume"_s
                        ? u"vbt-volume"_s : u"vbt"_s;
                if (result.openResult.kind == u"volume"_s) {
                    win = new vtkWindowVbtVolume(
                            result.openResult, result.subsetResult,
                            result.backendUrl, result.backendToken,
                            result.sessionId, this);
                } else {
                    auto *vbtWin = new vtkWindowVbt(
                            result.openResult, result.subsetResult,
                            result.backendUrl, result.backendToken,
                            result.sessionId, headerPath, this);
                    // On session expiry the window emits reopenRequested; reuse normal open flow.
                    QObject::connect(vbtWin, &vtkWindowVbt::reopenRequested,
                                     this, &MainWindow::doOpenVbtPath);
                    win = vbtWin;
                }
                win->showMaximized();
                win->raise();
                win->activateWindow();

                // Record after successful open (inside the watcher callback
                // because kind is only known after the async open completes).
                if (this->recentMgr) {
                    // Strip .bin.head suffix for the display name.
                    QString name = QFileInfo(headerPath).fileName();
                    if (name.endsWith(u".bin.head"_s)) {
                        name.chop(9);
                    }
                    this->recentMgr->record(headerPath, name, vbtType);
                }
            });

    watcher->setFuture(
            QtConcurrent::run(&runVbtOpenTask, headerPath, backendUrl, backendToken));
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

// ── VLKB query ────────────────────────────────────────────────────────────────

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

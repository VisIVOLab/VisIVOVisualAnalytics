#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "AboutDialog.h"
#include "AuthWrapper.h"
#include "app/BackendClient.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "BaselineDialog.h"
#include "DataHubWidget.h"
#include "DiagnosticsWindow.h"
#include "LinewidthDialog.h"
#include "RemoteFileBrowserDialog.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "SourceFindDialog.h"
#include "StackDialog.h"
#include "WebViewProcess.h"
#include "vtkWindowCatalogue3D.h"
#include "vtkWindowCube.h"
#include "vtkWindowImage.h"
#include "HiPSWindow.h"
#include "vtkWindowVbt.h"
#include "vtkWindowVbtVolume.h"
#include "VLKBInventoryTree.h"
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
#include <QFutureWatcher>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QTabBar>
#include <QTabWidget>
#include <QWebChannel>
#include <QPixmap>
#include <QStyle>
#include <QPainter>
#include <QtConcurrent>

#include "app/DiagnosticsManager.h"

using namespace Qt::StringLiterals;

namespace {

QPixmap makeLightDialogQuestionPixmap(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor fg(QStringLiteral("#E6F4F9"));
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

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    qInfo().noquote() << qApp->applicationName() << qApp->applicationVersion();

    const QString workDir = QDir::home().absoluteFilePath(qApp->applicationName());
    qInfo() << "Working directory:" << workDir;

    // Create core objects
    this->settings = new Settings(workDir, this);
    this->auth = new AuthWrapper(this);
    QObject::connect(this->settings, &Settings::updated, this, &MainWindow::setApplicationTheme);
    QObject::connect(this->settings, &Settings::updated, this, &MainWindow::loadPanoramicView);
    QObject::connect(this->settings, &Settings::updated, this, &MainWindow::syncDataHubSettings);

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

    // Reparent the existing central widget (built by ui->setupUi) into the tab.
    auto *skyPanel = this->centralWidget();
    skyPanel->setParent(tabs);
    tabs->addTab(skyPanel, u"VLKB"_s);

    this->setCentralWidget(tabs);

    // Permanent tabs (Data Hub + VLKB) must not show a close button.
    tabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    tabs->tabBar()->setTabButton(1, QTabBar::RightSide, nullptr);

    // Close user-added tabs (index ≥ 2) on request; ignore permanent ones.
    QObject::connect(tabs, &QTabWidget::tabCloseRequested, this, [tabs](int index) {
        if (index >= 2) {
            QWidget *w = tabs->widget(index);
            tabs->removeTab(index);
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

    // ── Initial DataHub state ─────────────────────────────────────────────────
    this->syncDataHubSettings();

    // Load theme
    this->setApplicationTheme();

    this->setMinimumSize(1200, 840);
    this->resize(1280, 840);
}

MainWindow::~MainWindow()
{
    delete ui;
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
        auto *client = new BackendClient(m_sciBackendUrl, m_sciToken);
        client->setSessionId(m_sciSessionId);
        auto *dlg = new BaselineDialog(client, m_sciSessionId, m_sciDatasetId, m_sciDepth, this);
        QObject::connect(dlg, &QDialog::finished, [client](int) { delete client; });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });

    m_actStack = sciMenu->addAction(u"Spectral Stacking…"_s);
    m_actStack->setEnabled(true);  // no dataset needed upfront — user selects from list
    m_actStack->setToolTip(u"Stack multiple session datasets into a 1-D spectrum (S-04)."_s);
    QObject::connect(m_actStack, &QAction::triggered, this, [this]() {
        QStringList datasets;
        if (!m_sciDatasetId.isEmpty()) {
            datasets << m_sciDatasetId;
        }
        auto *client = new BackendClient(m_sciBackendUrl, m_sciToken);
        client->setSessionId(m_sciSessionId);
        auto *dlg = new StackDialog(client, m_sciSessionId, datasets, this);
        QObject::connect(dlg, &QDialog::finished, [client](int) { delete client; });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });

    m_actSourceFind = sciMenu->addAction(u"Source Finding (SoFiA-2)…"_s);
    m_actSourceFind->setEnabled(false);
    m_actSourceFind->setToolTip(u"Run SoFiA-2 on the current dataset (S-05). Open a dataset first."_s);
    QObject::connect(m_actSourceFind, &QAction::triggered, this, [this]() {
        auto *client = new BackendClient(m_sciBackendUrl, m_sciToken);
        auto *dlg = new SourceFindDialog(client, m_sciDatasetId, this);
        QObject::connect(dlg, &QObject::destroyed, this, [client]() { delete client; });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });
}

// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::syncDataHubSettings()
{
    if (this->dataHub) {
        this->dataHub->setBackendUrl(this->settings->getBackendUrl(),
                                     this->settings->getBackendToken());
    }
}

void MainWindow::openHiPS()
{
    const QString backendUrl   = this->settings->getBackendUrl();
    const QString backendToken = this->settings->getBackendToken();
    auto *hipsWidget = new HiPSWindow(backendUrl, backendToken, m_tabs);
    const int idx = m_tabs->addTab(hipsWidget, u"HiPS Viewer"_s);
    m_tabs->setCurrentIndex(idx);

    if (this->recentMgr) {
        this->recentMgr->record(u"hips"_s, u"HiPS Viewer"_s, u"hips"_s);
    }
}

void MainWindow::reopenDataset(const RecentDataset &dataset)
{
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
    if (!this->diagnosticsWindow) {
        this->diagnosticsWindow = new DiagnosticsWindow(nullptr, Qt::Window);
    }
    this->diagnosticsWindow->show();
    this->diagnosticsWindow->raise();
    this->diagnosticsWindow->activateWindow();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QMessageBox box(this);
    box.setWindowTitle(u"Exit"_s);
    box.setText(u"Do you want to exit?"_s);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);

    box.setIconPixmap(makeLightDialogQuestionPixmap(40));

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
                "Start a local backend or configure a remote backend URL in Settings, "
                "then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString path = dialog.selectedFilePath();
    if (path.isEmpty()) {
        return;
    }

    doOpenDataPath(path);
}

void MainWindow::doOpenDataPath(const QString &path)
{
    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const auto health = client.health();
    if (!health.ok) {
        QMessageBox::critical(
                this, u"Backend unavailable"_s,
                u"Cannot open a dataset: the backend is not reachable.\n\n"
                "Error: %1"_s.arg(health.error.isEmpty() ? u"No response"_s : health.error));
        return;
    }

    const auto opened = client.openDataset(path);
    if (!opened.valid) {
        QMessageBox::critical(
                this, u"Could not open dataset"_s,
                u"The backend could not open this dataset.\n\n%1\n\n"
                "Supported formats: FITS (image, cube). For HDF5, Gadget, VOTable, "
                "or simulation data, convert with VisIVOServer first."_s.arg(
                        opened.error.isEmpty() ? u"Backend returned no error detail."_s
                                               : opened.error));
        return;
    }

    QWidget *win = nullptr;
    if (opened.kind == u"image"_s) {
        qDebug().noquote()
                << QStringLiteral("[fits] active_axes=%1 -> opening as 2D image | %2")
                           .arg(opened.activeAxes)
                           .arg(opened.degenerateAxesSummary.isEmpty()
                                        ? QStringLiteral("no collapsed axes")
                                        : opened.degenerateAxesSummary);
        win = new vtkWindowImage(path, client.baseUrl(), opened.datasetId,
                                 opened.ctype, opened.cunit, opened.crval, opened.crpix,
                                 opened.cdelt, opened.degenerateAxesSummary,
                                 opened.wcsStatus, opened.wcsWarningMessage,
                                 client.sessionId(), client.token(), this);
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
                                          client.sessionId(), client.token(), this);
        cubeWin->setDarkPlots(this->settings->getPlotTheme() == 0);
        win = cubeWin;
    } else {
        QMessageBox::information(
                this, u"Unsupported dataset"_s,
                u"The backend reported an unsupported dataset kind: '%1'.\n\n"
                "Only FITS images and spectral cubes are supported."_s.arg(opened.kind));
        return;
    }

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
                "Start a local backend or configure a remote backend URL in Settings, "
                "then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), this);
    dialog.setExtensionFilter({u".csv"_s}, u"CSV catalogues only"_s);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString filepath = dialog.selectedFilePath();
    if (filepath.isEmpty())
        return;

    doOpenCataloguePath(filepath);
}

void MainWindow::doOpenCataloguePath(const QString &filepath)
{
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
                "Start a local backend or configure a remote backend URL in Settings, "
                "then try again.\n\nError: %1"_s.arg(health.error.isEmpty()
                                                              ? u"No response"_s
                                                              : health.error));
        return;
    }

    RemoteFileBrowserDialog dialog(client.baseUrl(), this);
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


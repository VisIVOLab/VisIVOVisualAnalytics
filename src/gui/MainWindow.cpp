#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "AboutDialog.h"
#include "AuthWrapper.h"
#include "app/BackendClient.h"
#include "DataHubWidget.h"
#include "DiagnosticsWindow.h"
#include "RemoteFileBrowserDialog.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "WebViewProcess.h"
#include "vtkWindowCatalogue3D.h"
#include "vtkWindowCube.h"
#include "vtkWindowImage.h"
#include "vtkWindowVbt.h"
#include "vtkWindowVbtVolume.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QTabWidget>
#include <QWebChannel>
#include <qstylehints.h>
#include <QPixmap>
#include <QStyle>
#include <QPainter>

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
    // detached and becomes "Sky View" tab 1.  DataHubWidget becomes tab 0 so
    // the app opens on the backend-first landing page.
    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->setTabPosition(QTabWidget::North);

    this->dataHub = new DataHubWidget(tabs);
    tabs->addTab(this->dataHub, u"Data Hub"_s);

    // Reparent the existing central widget (built by ui->setupUi) into the tab.
    auto *skyPanel = this->centralWidget();
    skyPanel->setParent(tabs);
    tabs->addTab(skyPanel, u"Sky View"_s);

    this->setCentralWidget(tabs);

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

    // ── Setup Menu About ──────────────────────────────────────────────────────
    QObject::connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::openAboutDialog);

    // ── Setup Panoramic View (Sky View tab) ───────────────────────────────────
    this->loadPanoramicView();
    auto webobj = new WebViewProcess(ui->view);
    auto webChannel = new QWebChannel(ui->view);
    webChannel->registerObject(u"webobj"_s, webobj);
    ui->view->page()->setWebChannel(webChannel);
    QObject::connect(webobj, &WebViewProcess::processJavascript, this,
                     &MainWindow::skyRegionSelected);

    ui->btnLoadData->setIcon(QIcon(u":/icons/FILE_OPEN.png"_s));
    QObject::connect(ui->btnLoadData, &QPushButton::clicked, this, &MainWindow::openData);

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

void MainWindow::syncDataHubSettings()
{
    if (this->dataHub) {
        this->dataHub->setBackendUrl(this->settings->getBackendUrl(),
                                     this->settings->getBackendToken());
    }
}

void MainWindow::openHiPS()
{
    QMessageBox::information(
            this,
            u"HiPS Viewer"_s,
            u"The HiPS panoramic sky viewer is not yet available in this build.\n\n"
            "It will be added in a future release."_s);
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
    }
}

void MainWindow::setApplicationTheme()
{
    QGuiApplication::styleHints()->setColorScheme(this->settings->getColorScheme());
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
        win = new vtkWindowCube(path, client.baseUrl(), opened.datasetId, opened.width,
                                opened.height, opened.depth, opened.spacing, opened.origin,
                                opened.ctype, opened.cunit, opened.crval, opened.crpix,
                                opened.cdelt, opened.degenerateAxesSummary,
                                opened.spectralAxisType, opened.spectralAxisUnit,
                                opened.wcsStatus, opened.wcsWarningMessage,
                                client.sessionId(), client.token(), this);
    } else {
        QMessageBox::information(
                this, u"Unsupported dataset"_s,
                u"The backend reported an unsupported dataset kind: '%1'.\n\n"
                "Only FITS images and spectral cubes are supported."_s.arg(opened.kind));
        return;
    }

    win->show();
    win->raise();
    win->activateWindow();
}

void MainWindow::openCatalogue3D()
{
    const QString filepath = QFileDialog::getOpenFileName(
            this, u"Open 3D Catalogue (CSV)"_s, QString(),
            u"CSV Catalogues (*.csv);;All files (*)"_s);
    if (filepath.isEmpty())
        return;

    auto *win = new vtkWindowCatalogue3D(filepath, settings->getBackendUrl(),
                                         settings->getBackendToken(), this);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->show();
    win->raise();
    win->activateWindow();
}

void MainWindow::openVbt()
{
    const QString headerPath = QFileDialog::getOpenFileName(
            this, u"Open VBT header"_s, QString(),
            u"VisIVO Binary Table header (*.bin.head);;All files (*)"_s);
    if (headerPath.isEmpty()) {
        return;
    }

    BackendClient client(this->settings->getBackendUrl(), this->settings->getBackendToken());
    const BackendVbtOpenResult openResult = client.openVbt(headerPath);
    if (!openResult.valid) {
        QMessageBox::critical(
                this, u"Could not open VBT"_s,
                openResult.error.isEmpty()
                        ? u"The backend could not open this VBT dataset."_s
                        : openResult.error);
        return;
    }

    QWidget *win = nullptr;
    if (openResult.kind == u"volume"_s) {
        const QString firstField =
                openResult.fields.empty() ? QString() : openResult.fields[0].name;
        const BackendVbtSubsetResult initialSubset =
                client.requestVbtSubset(openResult.datasetId, 0, firstField);
        if (!initialSubset.valid) {
            QMessageBox::critical(this, u"Could not load VBT volume"_s,
                                  initialSubset.error.isEmpty()
                                          ? u"Backend returned no volume data."_s
                                          : initialSubset.error);
            return;
        }
        win = new vtkWindowVbtVolume(openResult, initialSubset, client.baseUrl(), client.token(),
                                     client.sessionId(), this);
    } else {
        const BackendVbtSubsetResult subsetResult =
                client.requestVbtSubset(openResult.datasetId);
        if (!subsetResult.valid) {
            QMessageBox::critical(this, u"Could not load VBT table"_s,
                                  subsetResult.error.isEmpty()
                                          ? u"Backend returned no table data."_s
                                          : subsetResult.error);
            return;
        }
        win = new vtkWindowVbt(openResult, subsetResult, client.baseUrl(), client.token(),
                               client.sessionId(), this);
    }
    win->show();
    win->raise();
    win->activateWindow();
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

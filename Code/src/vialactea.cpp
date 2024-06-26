#include "vialactea.h"
#include "ui_vialactea.h"

#include "aboutform.h"
#include "astroutils.h"
#include "pqwindowcube.h"
#include "pqwindowimage.h"
#include "sed.h"
#include "sedvisualizerplot.h"
#include "sessionloader.h"
#include "settingform.h"
#include "singleton.h"
#include "subsetselectordialog.h"
#include "usertablewindow.h"
#include "vialacteainitialquery.h"
#include "vialacteastringdictwidget.h"
#include "vlkbsimplequerycomposer.h"

#include <pqFileDialog.h>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QWebChannel>

#include "pqActiveObjects.h" //Do not remove, it is used
#include "pqApplicationCore.h"
#include "pqObjectBuilder.h"
#include "pqServerResource.h"

#include "vtkSMPluginManager.h"
#include "vtkSMProxyManager.h"
#include "vtkSMReaderFactory.h"

WebProcess::WebProcess(QObject *parent) : QObject(parent) { }

void WebProcess::jsCall(const QString &point, const QString &radius)
{
    emit processJavascript(point, radius);
}

const QString ViaLactea::ONLINE_TILE_PATH = "http://vlkb.ia2.inaf.it/panoramicview/openlayers.html";

const QString ViaLactea::VLKB_URL_IA2 = "https://vlkb.ia2.inaf.it:8443/vlkb/datasets";
const QString ViaLactea::TAP_URL_IA2 = "http://ia2-vialactea.oats.inaf.it:8080/vlkb";

const QString ViaLactea::VLKB_URL_NEANIAS = "https://vlkb.neanias.eu/vlkb-datasets-1.2/";
const QString ViaLactea::TAP_URL_NEANIAS = "https://vlkb.neanias.eu/vlkb/tap";

ViaLactea::ViaLactea(QWidget *parent) : QMainWindow(parent), ui(new Ui::ViaLactea)
{

    ui->setupUi(this);

    ui->saveToDiskCheckBox->setVisible(false);
    ui->fileNameLineEdit->setVisible(false);
    ui->selectFsPushButton->setVisible(false);
    // ui->loadTableButton->hide();

    // Cleanup previous run tmp
    QDir dir_tmp(
            QDir::homePath().append(QDir::separator()).append("VisIVODesktopTemp/tmp_download"));
    foreach (QString dirFile, dir_tmp.entryList()) {
        dir_tmp.remove(dirFile);
    }

    m_sSettingsFile = QDir::homePath()
                              .append(QDir::separator())
                              .append("VisIVODesktopTemp")
                              .append("/setting.ini");

    connectToPVServer();

    updateVLKBSetting();

    QSettings settings(m_sSettingsFile, QSettings::NativeFormat);

    /*
     if (settings.value("vlkbtype", "ia2").toString() == "neanias") {
     // The user has to login through SSO to continue or change access method
     QMessageBox *msgBox = new QMessageBox(this);
     msgBox->setIcon(QMessageBox::Question);
     msgBox->setWindowTitle("Login required");
     msgBox->setInformativeText(
     "You have selected private access to VLKB using NEANIAS SSO.\n\nSign in to "
     "continue using this private access or change access method in the Settings.");
     msgBox->addButton("Login", QMessageBox::AcceptRole);
     msgBox->addButton("Open settings", QMessageBox::DestructiveRole);
     msgBox->show();
     connect(msgBox, &QMessageBox::buttonClicked, [=](QAbstractButton *button) {
     QMessageBox::ButtonRole btn = msgBox->buttonRole(button);
     if (btn == QMessageBox::AcceptRole) {
     // Open NEANIAS login page
     AuthWrapper *auth = &NeaniasVlkbAuth::Instance();
     connect(auth, &AuthWrapper::authenticated,
     &Singleton<VialacteaStringDictWidget>::Instance(),
     &VialacteaStringDictWidget::buildDict);
     auth->grant();
     } else {
     // Open settings window
     on_actionSettings_triggered();
     }
     msgBox->deleteLater();
     });
     }
     */

    if (settings.value("online", true) == true) {
        tilePath = settings.value("onlinetilepath", ONLINE_TILE_PATH).toString();
        ui->webView->load(QUrl(tilePath));

    } else {
        tilePath = settings.value("tilepath", "").toString();
        ui->webView->load(QUrl::fromLocalFile(tilePath));
    }
    ui->webView->setContextMenuPolicy(Qt::CustomContextMenu);

    // create an object for javascript communication
    webobj = new WebProcess(this);
    connect(webobj, &WebProcess::processJavascript, this, &ViaLactea::on_webViewRegionSelected);

    QWebChannel *channel = new QWebChannel(this);
    channel->registerObject("webobj", webobj);
    ui->webView->page()->setWebChannel(channel);

    // QObject::connect(this, SIGNAL(destroyed()), qApp, SLOT(quit()));

    // VialacteaStringDictWidget *stringDictWidget =
    // &Singleton<VialacteaStringDictWidget>::Instance(); stringDictWidget->buildDict();

    qDebug() << "----------tilePath: " << tilePath;

    mapSurvey.insert(0, QPair<QString, QString>("MIPSGAL", "24 um"));
    mapSurvey.insert(1, QPair<QString, QString>("GLIMPSE I", "8.0 um"));
    mapSurvey.insert(2, QPair<QString, QString>("GLIMPSE I", "5.8 um"));
    mapSurvey.insert(3, QPair<QString, QString>("GLIMPSE I", "4.5 um"));
    mapSurvey.insert(4, QPair<QString, QString>("GLIMPSE I", "3.6 um"));
    mapSurvey.insert(5, QPair<QString, QString>("Hi-GAL", "500 um"));
    mapSurvey.insert(6, QPair<QString, QString>("Hi-GAL", "350 um"));
    mapSurvey.insert(7, QPair<QString, QString>("Hi-GAL", "250 um"));
    mapSurvey.insert(8, QPair<QString, QString>("Hi-GAL", "70 um"));
    mapSurvey.insert(9, QPair<QString, QString>("Hi-GAL", "160 um"));
    mapSurvey.insert(10, QPair<QString, QString>("WISE", "22 um"));
    mapSurvey.insert(11, QPair<QString, QString>("WISE", "12 um"));
    mapSurvey.insert(12, QPair<QString, QString>("WISE", "4.6 um"));
    mapSurvey.insert(13, QPair<QString, QString>("WISE", "3.4 um"));
    mapSurvey.insert(14, QPair<QString, QString>("ATLASGAL", "870 um"));
    mapSurvey.insert(15, QPair<QString, QString>("CSO BGPS", "1.1 mm"));
    mapSurvey.insert(16, QPair<QString, QString>("CORNISH", "5 GHz"));
}

ViaLactea::~ViaLactea()
{
    delete ui;
}

void ViaLactea::quitApp()
{
    // Problem not only in this
    QWebEnginePage *p = ui->webView->page();
    p->disconnect(ui->webView);
    delete p;
}

bool ViaLactea::connectToPVServer()
{
    if (server) {
        qDebug() << Q_FUNC_INFO << "Already connected";
        return true;
    }

    QSettings settings(m_sSettingsFile, QSettings::NativeFormat);
    QString serverUrl = settings.value("pvserver.url", "cs://localhost:11111").toString();

    server = pqApplicationCore::instance()->getObjectBuilder()->createServer(
            pqServerResource(serverUrl), 3);

    vtkSMReaderFactory *readerFactory = vtkSMProxyManager::GetProxyManager()->GetReaderFactory();
    readerFactory->RegisterPrototype("sources", "FitsReader");

    return server != nullptr;
}

void ViaLactea::onDataLoaded(const QString &filepath)
{
    auto subsetSelector = new SubsetSelectorDialog(this);
    subsetSelector->setAttribute(Qt::WA_DeleteOnClose);
    connect(subsetSelector, &SubsetSelectorDialog::subsetSelected, this,
            [=](const CubeSubset &subset) {
                auto win = new pqWindowCube(filepath, subset);
                win->showMaximized();
                win->raise();
                win->activateWindow();
            });

    subsetSelector->show();
    subsetSelector->activateWindow();
    subsetSelector->raise();
}

void ViaLactea::onImageDataLoaded(const QString &filepath)
{
    auto subsetSelector = new SubsetSelectorDialog(this);
    subsetSelector->setAttribute(Qt::WA_DeleteOnClose);
    connect(subsetSelector, &SubsetSelectorDialog::subsetSelected, this,
            [=](const CubeSubset &subset) {
                auto win = new pqWindowImage(filepath, subset);
                win->showMaximized();
                win->raise();
                win->activateWindow();
            });

    subsetSelector->show();
    subsetSelector->activateWindow();
    subsetSelector->raise();
}

void ViaLactea::updateVLKBSetting()
{
    QSettings settings(m_sSettingsFile, QSettings::NativeFormat);
    QString vlkbtype = settings.value("vlkbtype", "ia2").toString();

    if (vlkbtype == "ia2") {
        qDebug() << "VLKB instance: IA2";
        settings.setValue("vlkburl", VLKB_URL_IA2);
        settings.setValue("vlkbtableurl", TAP_URL_IA2);
    } else /*(vlkbtype == "neanias")*/ {
        qDebug() << "VLKB instance: NEANIAS";
        settings.setValue("vlkburl", VLKB_URL_NEANIAS);
        settings.setValue("vlkbtableurl", TAP_URL_NEANIAS);
    }
    settings.sync();

    VialacteaStringDictWidget *stringDictWidget = &Singleton<VialacteaStringDictWidget>::Instance();
    stringDictWidget->buildDict();
}

void ViaLactea::on_queryPushButton_clicked()
{
    if (masterWin != nullptr) {
        QMessageBox::information(
                this, QObject::tr(""),
                QObject::tr(
                        "A session is already open. Close the session window to start a new one."));
        return;
    }

    VialacteaInitialQuery *vq;
    if (ui->saveToDiskCheckBox->isChecked()) {
        if (ui->fileNameLineEdit->text() != "")
            vq = new VialacteaInitialQuery(ui->fileNameLineEdit->text());
        else {
            QMessageBox::critical(this, "Error", "Insert filename");
            return;
        }
    } else
        vq = new VialacteaInitialQuery();

    vq->setL(ui->glonLineEdit->text());
    vq->setB(ui->glatLineEdit->text());
    if (ui->radiumLineEdit->text() != "")
        vq->setR(ui->radiumLineEdit->text());
    else {
        vq->setDeltaRect(ui->dlLineEdit->text(), ui->dbLineEdit->text());
    }

    QList<QPair<QString, QString>> selectedSurvey;

    QList<QCheckBox *> allButtons = ui->surveySelectorGroupBox->findChildren<QCheckBox *>();
    for (int i = 0; i < allButtons.size(); ++i) {
        qDebug() << "i: " << i << " " << allButtons.at(i);

        if (allButtons.at(i)->isChecked()) {

            selectedSurvey.append(mapSurvey.value(i));
        }
    }

    // connettere la banda selezionata
    vq->setSpecies("Continuum");
    vq->setSurveyname(selectedSurvey.at(0).first);
    vq->setTransition(selectedSurvey.at(0).second);
    vq->setSelectedSurveyMap(selectedSurvey);
    vq->on_queryPushButton_clicked();
}

void ViaLactea::on_noneRadioButton_clicked(bool checked)
{
    if (checked) {
        ui->webView->page()->runJavaScript("activatePointSelection(false)");
        ui->webView->page()->runJavaScript("activateRectangularSelection(false)");
    }
}

void ViaLactea::on_saveToDiskCheckBox_clicked(bool checked)
{
    ui->fileNameLineEdit->setEnabled(checked);
    ui->selectFsPushButton->setEnabled(checked);
}

void ViaLactea::on_selectFsPushButton_clicked()
{
    QString fn =
            QFileDialog::getSaveFileName(this, "Save as...", QString(), "Fits images (*.fits)");

    if (!fn.isEmpty() && !fn.endsWith(".fits", Qt::CaseInsensitive))
        fn += ".fits"; // default
    ui->fileNameLineEdit->setText(fn);
}

void ViaLactea::on_webViewRegionSelected(const QString &point, const QString &area)
{
    if (!point.isEmpty()) {
        QStringList pieces = point.split(",");
        ui->glonLineEdit->setText(QString::number(pieces[0].toDouble(), 'f', 4));
        ui->glatLineEdit->setText(QString::number(pieces[1].toDouble(), 'f', 4));
        if (ui->radiumLineEdit->text() == "")
            ui->radiumLineEdit->setText("0.1");
        ui->dlLineEdit->setText("");
        ui->dbLineEdit->setText("");

        ui->noneRadioButton->setChecked(true);
        on_noneRadioButton_clicked(true);
    }

    if (!area.isEmpty()) {
        QStringList pieces = area.split(",");
        QString dl = QString::number(pieces[0].toDouble(), 'f', 4);
        if (dl.toDouble() > 4.0)
            dl = QString::number(4.0, 'f', 4);
        QString db = QString::number(pieces[1].toDouble(), 'f', 4);
        if (db.toDouble() > 4.0)
            db = QString::number(4.0, 'f', 4);
        ui->dlLineEdit->setText(dl);
        ui->dbLineEdit->setText(db);
        ui->radiumLineEdit->setText("");
    }
}

void ViaLactea::on_glonLineEdit_textChanged(const QString &arg1)
{
    Q_UNUSED(arg1);
    queryButtonStatusOnOff();
}

void ViaLactea::on_glatLineEdit_textChanged(const QString &arg1)
{
    Q_UNUSED(arg1);
    queryButtonStatusOnOff();
}

void ViaLactea::on_radiumLineEdit_textChanged(const QString &arg1)
{
    Q_UNUSED(arg1);
    queryButtonStatusOnOff();
}

void ViaLactea::queryButtonStatusOnOff()
{
    if (ui->glatLineEdit->text() != "" && ui->glonLineEdit->text() != ""
        && (ui->radiumLineEdit->text() != ""
            || (ui->dlLineEdit->text() != "" && ui->dbLineEdit->text() != "")))
        ui->queryPushButton->setEnabled(true);
    else
        ui->queryPushButton->setEnabled(false);
}

void ViaLactea::on_openLocalImagePushButton_clicked()
{
    if (!server) {
        QMessageBox::warning(
                this, "Not connected",
                "Not connected to pvserver. Check the connection url in the settings.");
        return;
    }

    vtkSMReaderFactory *readerFactory = vtkSMProxyManager::GetProxyManager()->GetReaderFactory();
    QString filters = readerFactory->GetSupportedFileTypes(server->session());

    pqFileDialog dialog(server, this, QString(), QString(), filters);
    dialog.setFileMode(pqFileDialog::ExistingFile);
    if (dialog.exec() == pqFileDialog::Accepted) {
        QString file = dialog.getSelectedFiles().first();
        onImageDataLoaded(file);
    }

    /*QString fn = QFileDialog::getOpenFileName(this, "Open image file", QString(),
                                              "Fits images (*.fits)");

    if (!fn.isEmpty()) {
        if (masterWin == nullptr) {
            double coords[2], rectSize[2];
            AstroUtils().GetCenterCoords(fn.toStdString(), coords);
            AstroUtils().GetRectSize(fn.toStdString(), rectSize);

            VialacteaInitialQuery *vq = new VialacteaInitialQuery;
            connect(vq, &VialacteaInitialQuery::searchDone,
                    [vq, fn, this](QList<QMap<QString, QString>> results) {
                        auto fits = vtkSmartPointer<vtkFitsReader>::New();
                        fits->SetFileName(fn.toStdString());
                        auto win = new vtkwindow_new(this, fits);
                        win->setDbElements(results);
                        setMasterWin(win);
                        vq->deleteLater();
                    });

            vq->searchRequest(coords[0], coords[1], rectSize[0], rectSize[1]);
        } else if (canImportToMasterWin(fn.toStdString())) {
            auto fits = vtkSmartPointer<vtkFitsReader>::New();
            fits->SetFileName(fn.toStdString());
            masterWin->addLayerImage(fits);
        } else {
            QMessageBox::warning(this, QObject::tr("Import image"),
                                 QObject::tr("The regions do not overlap, the file cannot be "
                                             "imported in the current session."));
        }
    }*/
}

void ViaLactea::on_actionSettings_triggered()
{
    if (!settingForm) {
        settingForm = new SettingForm(this);
    }
    settingForm->show();
    settingForm->activateWindow();
    settingForm->raise();
}

void ViaLactea::reload()
{
    QSettings settings(m_sSettingsFile, QSettings::NativeFormat);

    // tilePath = settings.value("tilepath", "").toString();

    if (settings.value("online", false) == true) {
        tilePath = settings.value("onlinetilepath", "").toString();
        ui->webView->load(QUrl(tilePath));

    } else {
        tilePath = settings.value("tilepath", "").toString();
        ui->webView->load(QUrl::fromLocalFile(tilePath));
    }

    connectToPVServer();
    updateVLKBSetting();
}

bool ViaLactea::isMasterWin(vtkwindow_new *win)
{
    return win == masterWin;
}

void ViaLactea::resetMasterWin()
{
    masterWin = nullptr;
}

void ViaLactea::setMasterWin(vtkwindow_new *win)
{
    masterWin = win;
}

bool ViaLactea::canImportToMasterWin(std::string importFn)
{
    if (masterWin != nullptr) {
        std::string masterFile = masterWin->getFitsImage()->GetFileName();
        // By default, partial overlap are allowed
        return AstroUtils().CheckOverlap(masterFile, importFn, false);
    }

    return false;
}

void ViaLactea::sessionScan(const QString &currentDir, const QDir &rootDir, QStringList &results)
{
    QDir dir(currentDir);

    foreach (auto &&file, dir.entryInfoList({ "*.fit", "*.fits" }, QDir::Files)) {
        results << rootDir.relativeFilePath(file.absoluteFilePath());
    }

    foreach (auto &&subDir, dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        sessionScan(subDir.absoluteFilePath(), rootDir, results);
    }
}

void ViaLactea::on_localDCPushButton_clicked()
{
    if (!server) {
        QMessageBox::warning(
                this, "Not connected",
                "Not connected to pvserver. Check the connection url in the settings.");
        return;
    }

    vtkSMReaderFactory *readerFactory = vtkSMProxyManager::GetProxyManager()->GetReaderFactory();
    QString filters = readerFactory->GetSupportedFileTypes(server->session());

    pqFileDialog dialog(server, this, QString(), QString(), filters);
    dialog.setFileMode(pqFileDialog::ExistingFile);
    if (dialog.exec() == pqFileDialog::Accepted) {
        QString file = dialog.getSelectedFiles().first();
        onDataLoaded(file);
    }

    /*
    QString fn = QFileDialog::getOpenFileName(this, tr("Import a file"), "",
                                              tr("FITS images(*.fit *.fits)"));

    bool test = true;

    if (!fn.isEmpty()) {
        if (test) {

            auto fitsReader_moment = vtkSmartPointer<vtkFitsReader>::New();
            fitsReader_moment->SetFileName(fn.toStdString());
            fitsReader_moment->isMoment3D = true;
            fitsReader_moment->setMomentOrder(0);
            auto win = new vtkwindow_new(this, fitsReader_moment);
            setMasterWin(win);

            // Open a new window to visualize the datacube
            auto fitsReader_dc = vtkSmartPointer<vtkFitsReader>::New();
            fitsReader_dc->SetFileName(fn.toStdString());
            fitsReader_dc->is3D = true;
            new vtkwindow_new(masterWin, fitsReader_dc, 1, win);

            // qDebug()<<ui->localDCPushButton->actions()[0];
            pqLoadDataReaction *dataLoader = new pqLoadDataReaction(new QAction());
            auto newSources = dataLoader->loadData({ fn });
            onDataLoaded(newSources,
                         fn.toStdString().substr(fn.toStdString().find_last_of("/\\") + 1));

        } else {

            if (masterWin == nullptr) {
                double coords[2], rectSize[2];
                AstroUtils().GetCenterCoords(fn.toStdString(), coords);
                AstroUtils().GetRectSize(fn.toStdString(), rectSize);

                VialacteaInitialQuery *vq = new VialacteaInitialQuery;
                connect(vq, &VialacteaInitialQuery::searchDone,
                        [vq, fn, this](QList<QMap<QString, QString>> results) {
                            // Open a new window to visualize the momentmap
                            auto fitsReader_moment = vtkSmartPointer<vtkFitsReader>::New();
                            fitsReader_moment->SetFileName(fn.toStdString());
                            fitsReader_moment->isMoment3D = true;
                            fitsReader_moment->setMomentOrder(0);
                            auto win = new vtkwindow_new(this, fitsReader_moment);
                            win->setDbElements(results);
                            setMasterWin(win);

                            // Open a new window to visualize the datacube
                            auto fitsReader_dc = vtkSmartPointer<vtkFitsReader>::New();
                            fitsReader_dc->SetFileName(fn.toStdString());
                            fitsReader_dc->is3D = true;
                            new vtkwindow_new(masterWin, fitsReader_dc, 1, win);

                            vq->deleteLater();
                        });
                vq->searchRequest(coords[0], coords[1], rectSize[0], rectSize[1]);
            } else if (canImportToMasterWin(fn.toStdString())) {
                auto moment = vtkSmartPointer<vtkFitsReader>::New();
                moment->SetFileName(fn.toStdString());
                moment->isMoment3D = true;
                moment->setMomentOrder(0);
                masterWin->addLayerImage(moment);

                auto dc = vtkSmartPointer<vtkFitsReader>::New();
                dc->SetFileName(fn.toStdString());
                dc->is3D = true;
                new vtkwindow_new(masterWin, dc, 1, masterWin);
            }

            else {
                QMessageBox::warning(this, QObject::tr("Import DC"),
                                     QObject::tr("The regions do not overlap, the file cannot be "
                                                 "imported in the current session."));
            }
        }
    }
     */
}

void ViaLactea::on_actionExit_triggered()
{
    // QCoreApplication::exit(0);

    this->close();
}

void ViaLactea::closeEvent(QCloseEvent *event)
{
    if (masterWin != nullptr && !masterWin->isSessionSaved()) {
        // Prompt to save the session
        if (!masterWin->confirmSaveAndExit()) {
            // Cancel button was clicked, therefore do not close
            event->ignore();
            return;
        }
    }

    ui->webView->page()->deleteLater();
    QApplication::quit();
}

void ViaLactea::on_actionAbout_triggered()
{
    if (!aboutForm) {
        aboutForm = new AboutForm(this);
    }
    aboutForm->show();
    aboutForm->activateWindow();
    aboutForm->raise();
}

void ViaLactea::on_select3dPushButton_clicked()
{
    VLKBSimpleQueryComposer *skyregionquery = new VLKBSimpleQueryComposer(NULL);
    skyregionquery->setIs3dSelections();

    skyregionquery->setLongitude(0, 360);
    skyregionquery->setLatitude(-1, 1);
    skyregionquery->show();
}

void ViaLactea::on_actionLoad_SED_2_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Load SED fits"), QDir::homePath(),
                                                    tr("Archive (*.zip)"));
    if (fileName.isEmpty())
        return;

    QString sedZipPath = QDir::homePath() + "/VisIVODesktopTemp/tmp_download/SED.zip";
    QFile::copy(fileName, sedZipPath);
    QProcess process_unzip;
    process_unzip.start("unzip " + sedZipPath + " -d " + QDir::homePath()
                        + "/VisIVODesktopTemp/tmp_download");

    process_unzip.waitForFinished(); // sets current thread to sleep and waits for process end
    QString output_unzip(process_unzip.readAll());

    QDir tmp_download(QDir::homePath() + "/VisIVODesktopTemp/tmp_download");
    QStringList filters;
    filters << "SED*";
    tmp_download.setNameFilters(filters);
    QStringList dirList = tmp_download.entryList();

    QFile sedFile(QDir::homePath()
                          .append(QDir::separator())
                          .append("/VisIVODesktopTemp/tmp_download/SEDList.dat"));
    sedFile.open(QIODevice::ReadOnly);
    QDataStream in(&sedFile); // read the data serialized from the file
    QList<SED *> sed_list2;
    in >> sed_list2;
    /*
     foreach(SED* sed, sed_list2){
     qDebug()<<"SED Designation";
     qDebug()<<sed->getRootNode()->getDesignation();
     }
     */

    ViaLactea *vialactealWin = &Singleton<ViaLactea>::Instance();
    SEDVisualizerPlot *sedv = new SEDVisualizerPlot(sed_list2, 0, vialactealWin);
    sedv->show();
    sedv->loadSavedSED(dirList);
}

void ViaLactea::on_pointRadioButton_clicked(bool checked)
{
    if (checked) {
        ui->webView->page()->runJavaScript("activatePointSelection(true)");
    }
}

void ViaLactea::on_rectRadioButton_clicked(bool checked)
{
    if (checked) {
        ui->webView->page()->runJavaScript("activateRectangularSelection(true)");
    }
}

void ViaLactea::on_dlLineEdit_textChanged(const QString &arg1)
{
    Q_UNUSED(arg1);
    queryButtonStatusOnOff();
}

void ViaLactea::on_dbLineEdit_textChanged(const QString &arg1)
{
    Q_UNUSED(arg1);
    queryButtonStatusOnOff();
}

void ViaLactea::on_actionLoad_session_triggered()
{
    if (masterWin != nullptr) {
        QMessageBox::warning(this, tr("Load a session"),
                             tr("A session is already open, close it to load another session."));
        return;
    }

    QString rootPath = QFileDialog::getExistingDirectory(this, "Load a session", QDir::homePath());
    if (rootPath.isEmpty()) {
        return;
    }

    QString fn = QDir(rootPath).absoluteFilePath("session.json");
    if (!QFile::exists(fn)) {
        // Scan the folder for fits files and create the session file on-the-fly

        QStringList fits;
        QDir rootDir(rootPath);
        sessionScan(rootPath, rootDir, fits);

        QJsonArray layers;
        QJsonArray datacubes;
        foreach (auto &&file, fits) {
            std::string fn = rootDir.absoluteFilePath(file).toStdString();
            int status = 0;
            fitsfile *fptr;

            if (fits_open_data(&fptr, fn.c_str(), READONLY, &status)) {
                fits_report_error(stderr, status);
                continue;
            }

            int naxis;
            if (fits_get_img_dim(fptr, &naxis, &status)) {
                fits_report_error(stderr, status);
                continue;
            }

            if (naxis == 3) {
                QJsonObject dc;
                dc["fits"] = file;
                dc["enabled"] = false;
                datacubes.append(dc);
            } else {
                QJsonObject layer;
                layer["fits"] = file;
                layer["type"] = "Continuum";
                layer["enabled"] = false;
                layers.append(layer);
            }

            fits_close_file(fptr, &status);
        }

        QJsonObject image;
        image["layers"] = layers;

        QJsonObject root;
        root["datacubes"] = datacubes;
        root["image"] = image;

        QJsonDocument doc(root);
        QFile tmp(fn);
        if (!tmp.open(QFile::WriteOnly)) {
            QMessageBox::critical(this, tr("Error"), tmp.errorString());
            return;
        }
        tmp.write(doc.toJson());
        tmp.close();
    }

    auto sessionLoader = new SessionLoader(this, fn);
    sessionLoader->show();
    sessionLoader->activateWindow();
    sessionLoader->raise();
}

void ViaLactea::on_loadTableButton_clicked()
{
    QSettings settings(m_sSettingsFile, QSettings::NativeFormat);
    QString vlkbtype = settings.value("vlkbtype", "ia2").toString();

    if (vlkbtype == "neanias") {
        QMessageBox::information(this, "Info",
                                 "This feature is only available using the IA2 instance.");
        return;
    }

    QString fn = QFileDialog::getOpenFileName(this, "Load user table", QDir::homePath());
    if (fn.isEmpty())
        return;

    auto win = new UserTableWindow(fn, m_sSettingsFile, this);
    win->showMaximized();
    win->activateWindow();
    win->raise();
}

/**
 * @brief ViaLactea::on_actionInspectMemory_triggered
 * Create a memory inspector panel if one isn't created yet.
 * Show the memory inspector. Can be resized or dragged around.
 */
void ViaLactea::on_actionInspectMemory_triggered()
{
    if (!server) {
        QMessageBox::warning(this, "Not connected",
                             "Server not connected. Check the connection url in the settings.");
        return;
    }
    if (memInspectWin != nullptr) {
        memInspectWin->show();
        memInspectWin->raise();
    } else {
        memInspectWin = new pqMemInspector(this);
        memInspectWin->setAttribute(Qt::WA_Hover);
        memInspectWin->show();
        memInspectWin->activateWindow();
        memInspectWin->raise();
    }
}

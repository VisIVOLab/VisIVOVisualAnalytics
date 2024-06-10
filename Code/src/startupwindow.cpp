#include "startupwindow.h"
#include "ui_startupwindow.h"

#include "astroutils.h"
#include "fitsheadermodifierdialog.h"
#include "fitsheaderviewer.h"
#include "recentfilesmanager.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>

StartupWindow::StartupWindow(QWidget *parent)
: QWidget(parent),
ui(new Ui::StartupWindow),
settingsFile(QDir::homePath().append("/VisIVODesktopTemp/setting.ini")),
settings(settingsFile, QSettings::IniFormat)
{
    ui->setupUi(this);
    ui->tabWidget->setCurrentIndex(0);
    ui->headerViewer->hide();
    
    ui->localOpenPushButton->setIcon(QIcon(":/icons/folder_open.svg"));
    ui->vlkbPushButton->setIcon(QIcon(":/icons/settings.svg"));
    ui->settingsPushButton->setIcon(QIcon(":/icons/cloud.svg"));

    QPalette pal = QPalette();
    pal.setColor(QPalette::Window, QColor::fromRgb(234, 233, 229));
    ui->leftContainer->setAutoFillBackground(true);
    ui->leftContainer->setPalette(pal);
    
    QPalette pal2 = QPalette();
    pal2.setColor(QPalette::Window, Qt::white);
    
    ui->buttonArea->setAutoFillBackground(true);
    ui->buttonArea->setPalette(pal2);
    
    // Setup History Manager
    this->historyModel = new RecentFilesManager(this);
    ui->historyArea->setModel(this->historyModel);
    
        
}

StartupWindow::~StartupWindow()
{
    delete ui;
}

void StartupWindow::setupSessionManager()
{
    
}

void StartupWindow::on_localOpenPushButton_clicked(bool fromHistory = false)
{
   
}

void StartupWindow::on_clearPushButton_clicked()
{
    ui->headerViewer->hide();
    this->historyModel->clearHistory();
}
void StartupWindow::openLocalImage(const QString &fn)
{
    this->historyModel->addRecentFile(fn);
}

void StartupWindow::openLocalDC(const QString &fn)
{
    this->historyModel->addRecentFile(fn);
}

void StartupWindow::showFitsHeader(const QString &fn)
{
    if (!fn.isEmpty()) {
        ui->headerViewer->showHeader(fn);
        ui->headerViewer->show();
    }
}

void StartupWindow::on_settingsPushButton_clicked()
{
   
}

void StartupWindow::on_openPushButton_clicked()
{
}

void StartupWindow::on_vlkbPushButton_clicked()
{
    
}

void StartupWindow::on_historyArea_activated(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    
    this->showFitsHeader(index.data().toString());
}

void StartupWindow::on_historyArea_clicked(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    
    this->showFitsHeader(index.data().toString());
}


void StartupWindow::changeEvent(QEvent *e)
{
   
}

void StartupWindow::closeEvent(QCloseEvent *event)
{
    
    auto res = QMessageBox::question(this, "Confirm exit",
                                     "Do you want to exit?.\nClosing this "
                                     "window will terminate ongoing processes.",
                                     QMessageBox::Yes | QMessageBox::No);
    
    if (res == QMessageBox::No) {
        event->ignore();
        return;
    }
    
    QApplication::quit();
}

void StartupWindow::registerInSessionManager(const QString &name, QWidget *window, const QString &parentName)
{
    
}

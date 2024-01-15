#include "pqPVWindow.h"
#include "ui_pqPVWindow.h"

#include "pqFileDialog.h"
#include "vtkSMProxyManager.h"
#include "vtkSMSessionProxyManager.h"
#include "vtkSMWriterFactory.h"
#include "vtkSMWriterProxy.h"

#include <pqActiveObjects.h>
#include <pqApplicationCore.h>
#include <pqDataRepresentation.h>
#include <vtkPNGWriter.h>
#include <vtkRenderWindow.h>
#include <vtkSMCoreUtilities.h>
#include <vtkSMPropertyHelper.h>
#include <vtkSMPVRepresentationProxy.h>
#include <vtkSMRenderViewProxy.h>
#include <vtkSMTransferFunctionManager.h>
#include <vtkSMTransferFunctionPresets.h>
#include <vtkWindowToImageFilter.h>

#include <QCloseEvent>
#include <QFileDialog>
#include <QMessageBox>

pqPVWindow::pqPVWindow(QString serverUrl, QString cubeFileName, std::pair<int, int> &start, std::pair<int, int> &end, std::pair<int, int> &zSubExtent, QWidget *parent):
      QMainWindow(parent),
      ui(new Ui::pqPVWindow)
{
    ui->setupUi(this);


    this->builder = pqApplicationCore::instance()->getObjectBuilder();
    this->server = pqActiveObjects::instance().activeServer();;

    viewImage = qobject_cast<pqRenderView *>(builder->createView(pqRenderView::renderViewType(), server));
    viewImage->widget()->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->centralwidget->layout()->addWidget(viewImage->widget());

    auto presets = vtkSMTransferFunctionPresets::GetInstance();
    for (int i = 0; i < presets->GetNumberOfPresets(); ++i) {
        QString name = QString::fromStdString(presets->GetPresetName(i));
        ui->comboLut->addItem(name);
    }
    ui->comboLut->setCurrentIndex(ui->comboLut->findText("Grayscale"));

    this->thread = new QThread();
    this->dataObject = new PVDataObject(start, end, zSubExtent, cubeFileName, this->viewImage, serverUrl);
    connect(this->thread, &QThread::started, this->dataObject, &PVDataObject::genPVSlice);
    connect(this->thread, &QThread::finished, this->dataObject, &QObject::deleteLater);
    this->dataObject->moveToThread(this->thread);

    connect(ui->radioLinear, &QRadioButton::toggled, this, &pqPVWindow::changeLutScale);
    connect(ui->opacitySlider, &QSlider::valueChanged, this, &pqPVWindow::changeOpacity);
    connect(ui->comboLut, &QComboBox::currentTextChanged, this, &pqPVWindow::changeLut);

    connect(this, &pqPVWindow::dataObjChangeLut, this->dataObject, &PVDataObject::changeLut);
    connect(this, &pqPVWindow::dataObjChangeLutScale, this->dataObject, &PVDataObject::changeLutScale);
    connect(this, &pqPVWindow::dataObjChangeOpacity, this->dataObject, &PVDataObject::changeOpacity);
    connect(this, &pqPVWindow::genPV, this->dataObject, &PVDataObject::genPVSlice);
    connect(this->dataObject, &PVDataObject::pvGenComplete, this, &pqPVWindow::dataObjPVGenComplete);
    connect(this->dataObject, &PVDataObject::render, this, &pqPVWindow::render);
    connect(this->dataObject, &PVDataObject::renderReset, this, &pqPVWindow::renderReset);
}

pqPVWindow::~pqPVWindow()
{
    delete ui;
    delete this->dataObject;
    this->thread->terminate();
}

void pqPVWindow::closeEvent(QCloseEvent *event)
{
    emit closed();
    event->accept();
}

void pqPVWindow::genPVSlice()
{
//    connect(this->thread, SIGNAL(started), this->dataObject, SLOT(genPVSlice()));
//    emit genPV(this->viewImage);

    this->dataObject->genPVSlice();
//    this->thread->start();
//    if (this->thread->isRunning())
//        std::cerr << "Thread for PVDataObject is running" << std::endl;
//    else
//        std::cerr << "Thread for PVDataObject is not running!" << std::endl;

//    this->dataObject->genPVSlice(this->viewImage);

//    QtConcurrent::run(this->dataObject, &PVDataObject::genPVSlice, this->viewImage);
}

QString pqPVWindow::getColourMap() const
{
    return colourMap;
}

void pqPVWindow::changeLutScale()
{
    emit dataObjChangeLutScale();
}

void pqPVWindow::changeOpacity(int opacity)
{
    emit dataObjChangeOpacity(opacity);
}

void pqPVWindow::changeLut(const QString &lutName)
{
    emit dataObjChangeLut(lutName);
}

void pqPVWindow::on_actionSave_as_PNG_triggered()
{
    this->saveAsPNG();
}

void pqPVWindow::on_actionSave_as_FITS_triggered()
{
    this->saveAsFITS();
}

void pqPVWindow::dataObjPVGenComplete()
{
    emit pvGenComplete();
}

void pqPVWindow::render()
{
    this->viewImage->render();
}

void pqPVWindow::renderReset()
{
    this->viewImage->resetDisplay();
    this->viewImage->render();
}

int pqPVWindow::saveAsPNG()
{
    QString filepath =
            QFileDialog::getSaveFileName(this, "Save as PNG...", QString(), "PNG image (*.png)");
    if (filepath.isEmpty()) {
        return 0;
    }

    vtkNew<vtkWindowToImageFilter> filter;
    filter->SetInput(this->viewImage->getRenderViewProxy()->GetRenderWindow());
    filter->SetScale(2);
    filter->Update();

    vtkNew<vtkPNGWriter> writer;
    writer->SetFileName(filepath.toStdString().c_str());
    writer->SetInputConnection(filter->GetOutputPort());
    writer->Write();

    this->viewImage->render();

    QMessageBox::information(this, "Image saved", "Image saved: " + filepath);
    return 1;
}

int pqPVWindow::saveAsFITS()
{
    vtkSMWriterFactory *writerFactory = vtkSMProxyManager::GetProxyManager()->GetWriterFactory();
    auto srcProxy = this->dataObject->getSourceProxy();
    auto z = writerFactory->GetSupportedWriterProxies(srcProxy, 0);
    auto filters = writerFactory->GetSupportedFileTypes(srcProxy);
    auto filepath = pqFileDialog::getSaveFileName(server, this, QString(), QString(), filters);

    if (filepath.isEmpty()){
        return 0;
    }
    QFileInfo fInfo(filepath);
    if (fInfo.suffix() != "fits" && fInfo.suffix() != "fts" && fInfo.suffix() != "fit" && fInfo.suffix() != ".fits" && fInfo.suffix() != ".fts" && fInfo.suffix() != ".fit")
        filepath.append(".fits");

    vtkSMWriterProxy* writer = vtkSMWriterProxy::SafeDownCast(server->proxyManager()->GetProxy("writers", "FitsWriter"));

    // Set input data
    writer->SetSelectionInput(0, srcProxy, 0);

    // Configure writer properties
    vtkSMPropertyHelper(writer, "FileName").Set(filepath.toStdString().c_str());

    // Update and execute the writer
    writer->UpdateVTKObjects();
    writer->UpdatePipeline();

    // Clean up
    writer->Delete();
    return 1;
}

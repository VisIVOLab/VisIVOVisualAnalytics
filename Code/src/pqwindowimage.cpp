#include "pqwindowimage.h"
#include "ui_pqwindowimage.h"

#include "vialacteainitialquery.h"
#include "interactors/vtkinteractorstyleimagecustom.h"
#include "errorMessage.h"

#include <pqActiveObjects.h>
#include <pqAlwaysConnectedBehavior.h>
#include <pqApplicationCore.h>
#include <pqFileDialog.h>
#include <pqLoadDataReaction.h>
#include <pqObjectBuilder.h>
#include <pqPipelineSource.h>
#include <pqRenderView.h>
#include <pqScalarBarRepresentation.h>
#include <pqScalarsToColors.h>
#include <pqSMAdaptor.h>

#include <vtkClientServerStream.h>
#include <vtkPVArrayInformation.h>
#include <vtkPVDataInformation.h>
#include <vtkPVDataMover.h>
#include <vtkPVDataSetAttributesInformation.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkScalarBarWidget.h>
#include <vtkSMCoreUtilities.h>
#include <vtkSMProperty.h>
#include <vtkSMPropertyHelper.h>
#include <vtkSMProxyManager.h>
#include <vtkSMPVRepresentationProxy.h>
#include <vtkSMReaderFactory.h>
#include <vtkSMSession.h>
#include <vtkSMSessionProxyManager.h>
#include <vtkSMStringVectorProperty.h>
#include <vtkSMTransferFunctionManager.h>
#include <vtkSMTransferFunctionPresets.h>
#include <vtkSMTransferFunctionProxy.h>
#include <vtkSMViewProxy.h>

#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsEffect>
#include <QSignalMapper>

pqWindowImage::pqWindowImage(const QString &filepath, const CubeSubset &cubeSubset) : ui(new Ui::pqWindowImage)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    // Create the LUT combo box options
    clmInit = true;
    imgCounter = 0;
    auto presets = vtkSMTransferFunctionPresets::GetInstance();
    for (int i = 0; i < presets->GetNumberOfPresets(); ++i) {
        QString name = QString::fromStdString(presets->GetPresetName(i));
        ui->cmbxLUTSelect->addItem(name);
    }
    ui->cmbxLUTSelect->setCurrentIndex(ui->cmbxLUTSelect->findText("Grayscale"));
    ui->linearRadioButton->setChecked(true);

    //Initialise image stack
    this->images = std::vector<vlvaStackImage*>();

    ui->tblCompactSourcesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tblCompactSourcesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->lstImageList->setDragDropMode(QAbstractItemView::InternalMove);
    connect(ui->lstImageList->model(), SIGNAL(rowsMoved(QModelIndex,int,int,QModelIndex,int)),
            this, SLOT(movedLayersRow(QModelIndex,int,int,QModelIndex,int)));

    // Set default opacity
    ui->opacitySlider->setSliderPosition(ui->opacitySlider->maximum());

    clmInit = false;

    // ParaView Init
    builder = pqApplicationCore::instance()->getObjectBuilder();
    server = pqActiveObjects::instance().activeServer();
    serverProxyManager = server->proxyManager();
    new pqAlwaysConnectedBehavior(this);

    // Enable annotations such as Remote Rendering, FPS, etc...
    auto renderingSettings = serverProxyManager->GetProxy("settings", "RenderViewSettings");
    vtkSMPropertyHelper(renderingSettings, "ShowAnnotation").Set(1);

    vtkSMPropertyHelper(renderingSettings, "LODThreshold").Set(512);
    vtkSMPropertyHelper(renderingSettings, "LODResolution").Set(0);
    vtkSMPropertyHelper(renderingSettings, "UseOutlineForLODRendering").Set(1);
    vtkSMPropertyHelper(renderingSettings, "RemoteRenderThreshold").Set(512);
    vtkSMPropertyHelper(renderingSettings, "CompressorConfig").Set("vtkLZ4Compressor 0 5");

    //Create the viewport
    createView();

    //Load the initial image
    int initImg = addImageToStack(filepath, cubeSubset);
    if (initImg){
        //Query VLKB and populate the table widget
        checkVLKB(this->images.at(0));
        //Create proxy to ParaviewVLKBProxy
        vlkbManagerProxy = serverProxyManager->NewProxy("sources", "VLKBProxy");
        // vlkbManagerProxy = builder->createProxy("sources", "VLKBProxy", server, "");
    }

    //Update the UI with initial values
    updateUI();

    viewImage->resetDisplay();
    viewImage->render();
}

pqWindowImage::~pqWindowImage()
{
    builder->destroySources(server);
    delete ui;
}

/**
 * @brief pqWindowImage::changeColorMap
 * Changes the colour map used to visualize the FITS image.
 * @param name The name of the colour map (provided by paraview presets)
 */
void pqWindowImage::changeColorMap(const QString &name)
{
    this->images.at(activeIndex)->changeColorMap(name);
    this->ui->lstImageList->item(activeIndex)->setText(name);
    viewImage->render();
}

/**
 * @brief pqWindowImage::showStatusBarMessage
 * Displays a message on the status bar of the window
 * @param msg The message to display
 */
void pqWindowImage::showStatusBarMessage(const std::string &msg)
{
    ui->statusBar->showMessage(QString::fromStdString(msg));
}

/**
 * @brief pqWindowImage::updateUI
 * Function that updates the interface with the newest values.
 * Called whenever something changes, such as user selecting another image.
 */
void pqWindowImage::updateUI()
{
    this->ui->lstImageList->clear();
    //Disable buttons if all images are removed
    if (images.size() == 0){
        this->ui->btnRemoveImageFromStack->setEnabled(false);
        this->ui->cmbxLUTSelect->setEnabled(false);
        this->ui->opacitySlider->setEnabled(false);
        this->ui->linearRadioButton->setCheckable(false);
        this->ui->logRadioButton->setCheckable(false);
        viewImage->render();
        return;
    }
    else{
        this->ui->btnRemoveImageFromStack->setEnabled(true);
        this->ui->cmbxLUTSelect->setEnabled(true);
        this->ui->opacitySlider->setEnabled(true);
        this->ui->linearRadioButton->setCheckable(true);
        this->ui->logRadioButton->setCheckable(true);
    }

    clmInit = true;
    setWindowTitle(this->images.at(activeIndex)->getFitsFileName());

    // Interactor to show pixel coordinates in the status bar
    vtkNew<vtkInteractorStyleImageCustom> interactorStyle;
    interactorStyle->SetCoordsCallback(
            [this](const std::string &str) { showStatusBarMessage(str); });
    interactorStyle->SetLayerFitsReaderFunc(this->images.at(activeIndex)->getFitsHeaderPath());
    viewImage->getViewProxy()->GetRenderWindow()->GetInteractor()->SetInteractorStyle(
            interactorStyle);
    std::sort(images.begin(), images.end(), [](vlvaStackImage* a, vlvaStackImage* b){ return a->getIndex() < b->getIndex();});

    //Create list of images at bottom right
    for (int i = 0; i < images.size(); ++i)
    {
        auto item = images.at(i);
        item->setZPosition();
        auto lstItem = new QListWidgetItem(this->ui->lstImageList);
        Qt::CheckState checked = (item->isEnabled()) ? Qt::Checked : Qt::Unchecked;
        lstItem->setText(item->getFitsFileName());
        lstItem->setFlags(lstItem->flags() | Qt::ItemIsUserCheckable);
        lstItem->setCheckState(checked);
        this->ui->lstImageList->addItem(lstItem);
    }

    //Update UI for current image values
    this->ui->lstImageList->setCurrentItem(this->ui->lstImageList->item(this->activeIndex));
    int newCMIndex = this->ui->cmbxLUTSelect->findText(this->images.at(activeIndex)->getColourMap());
    this->ui->cmbxLUTSelect->setCurrentIndex(newCMIndex);

    this->ui->opacitySlider->setValue(this->images.at(activeIndex)->getOpacity() * 100);
    this->ui->logRadioButton->setChecked(this->images.at(activeIndex)->getLogScale());
    this->ui->linearRadioButton->setChecked(!this->images.at(activeIndex)->getLogScale());

    viewImage->render();
    clmInit = false;
}

/**
 * @brief pqWindowImage::setImageListCheckbox
 * Function that sets images active or not if the user checks the checkbox.
 * @param row The index of the image the user selected.
 * @param checked The state of the checkbox (checked, not checked).
 */
void pqWindowImage::setImageListCheckbox(int row, Qt::CheckState checked)
{
    auto im = images.at(row);
    if (checked == Qt::Checked){
        im->setActive(true);
    }
    else{
        im->setActive(false);
    }
}

/**
 * @brief pqWindowImage::setLogScale
 * Sets the image visualization to use log10 scaling on the colour map.
 * @param logScale
 * A boolean whether or not to use log10 scaling.
 */
void pqWindowImage::setLogScale(bool logScale)
{
    // If in the process of initialising or updating the UI, ignore this command.
    if (clmInit)
        return;

    this->images.at(activeIndex)->setLogScale(logScale);
    viewImage->render();
}

/**
 * @brief pqWindowImage::setOpacity
 * Function that sets the image opacity according to the given value.
 * @param value
 * A value between 0 and 1, with 0 being fully transparent and 1 being fully opaque.
 */
void pqWindowImage::setOpacity(float value)
{
    this->images.at(activeIndex)->setOpacity(value);
    viewImage->render();
}

/**
 * @brief pqWindowImage::addImageToStack
 * Function that adds a new image to the image stack.
 * @param file The file name (on the server) of the new image.
 * @param subset The subset variables selected (used to limit resolution for big images)
 * @return True if successful, false if unsuccessful
 */
int pqWindowImage::addImageToStack(QString file, const CubeSubset &subset)
{
    // If in the process of initialising the UI, ignore this command.
    if (clmInit)
        return 1;
    std::cerr << "Adding image " << file.toStdString() << " to stack via proxy call." << std::endl;
    int index = images.size();
    auto im = new vlvaStackImage(file, imgCounter, this->logScaleDefault, builder, viewImage, serverProxyManager);
    im->setIndex(index);
    images.push_back(im);
    this->activeIndex = images.size() - 1;

    // Initialise image on server side
    removeErrorCode initErr = (removeErrorCode) images.at(activeIndex)->init(file, subset);
    //Check error code
    if (initErr == removeErrorCode::NO_ERROR)
    {
        if (images.size() == 1){
            this->positionImage(images.at(activeIndex), true);
            images.at(activeIndex)->setOpacity(1);
        }
        else{
            if (!this->positionImage(images.at(activeIndex), false))
            {
                std::cerr << file.toStdString() << " not added to stack." << std::endl;
                removeImageFromStack(activeIndex);
                return 0;
            }

            images.at(activeIndex)->setOpacity(defaultMultiOpacity);
        }
        this->updateUI();
        viewImage->render();
        imgCounter++;
        return 1;
    } else
    {
        std::cerr << "Failed to initialise image for file " << file.toStdString() << "." << std::endl;
        removeImageFromStack(activeIndex, initErr);
        return 0;
    }
}

/**
 * @brief pqWindowImage::removeImageFromStack
 * Function that removes an image from the stack
 * @param index The index of the image to be removed
 * @param remErrCode The error code used for removing the image. Used when default removal would cause issues for program.
 * @return True if successful, false if unsuccessful
 */
int pqWindowImage::removeImageFromStack(const int index, const removeErrorCode remErrCode)
{
    // If in the process of initialising the UI, ignore this command.
    if (clmInit)
        return 1;

    auto rep = this->images.at(activeIndex)->getImageRep();
    //If error code returns that the image is a cube, don't remove colour bar since it doesn't exist
    if (remErrCode != removeErrorCode::IS_CUBE_ERROR)
    {
        auto colProxy = rep->getLookupTable();
        auto scalBarRep = colProxy->getScalarBar(this->viewImage);
        builder->destroy(scalBarRep);
    }

    //Free resources on the server
    builder->destroy(rep);

    images.erase(images.begin() + index);
    this->activeIndex = activeIndex > 0 ? index - 1 : 0;
    updateUI();
    return 1;
}


/**
 * @brief pqWindowImage::setVLKBElementsList
 * This function takes a list as provided by the VLKB REST API and populates the table in the top right of the UI.
 * @param elementsOnDb The list of potential cutouts available on the VLKB.
 */
void pqWindowImage::setVLKBElementsList(QList<QMap<QString, QString> > elementsOnDb)
{
    auto classElementsOnDb = elementsOnDb;
    int i = 0;
    ui->elementListWidget->clear();
    while (!elementsOnDb.isEmpty()) {
        QMap<QString, QString> datacube = elementsOnDb.takeFirst();
        ui->elementListWidget->insertRow(i);
        QTableWidgetItem *item_0 = new QTableWidgetItem();
        item_0->setFlags(item_0->flags() ^ Qt::ItemIsEditable);
        item_0->setText(datacube["Survey"] + "\n" + datacube["Species"]);
        ui->elementListWidget->setItem(i, 0, item_0);

        QTableWidgetItem *item_1 = new QTableWidgetItem();
        item_1->setFlags(item_1->flags() ^ Qt::ItemIsEditable);
        QString codeString = "";
        switch (datacube["code"].toInt()) {
        case 2:
            codeString = "The Region is completely inside the input";
            break;
        case 3:
            codeString = "Full Overlap";
            break;
        case 4:
            codeString = "Partial Overlap";
            break;
        case 5:
            codeString = "The Regions are identical ";
            break;
        default:
            codeString = "Merge";
            break;
        }

        item_1->setText(datacube["Transition"] + "\n" + codeString);
        ui->elementListWidget->setItem(i, 1, item_1);
        if (datacube["code"].toDouble() == 3) {
            ui->elementListWidget->item(i, 0)->setBackground(Qt::green);
            ui->elementListWidget->item(i, 1)->setBackground(Qt::green);
        }
        item_0->setToolTip(datacube["Description"]);
        item_1->setToolTip(datacube["Description"]);
        i++;
    }

    ui->elementListWidget->setWordWrap(true);
    ui->elementListWidget->setTextElideMode(Qt::ElideMiddle);
    ui->elementListWidget->resizeColumnsToContents();
    ui->elementListWidget->resizeRowsToContents();
}

/**
 * @brief pqWindowImage::checkVLKB
 * This function makes a request to the VLKB for potential cutouts that overlap at least partially with the first image loaded.
 * @param stackImage
 * The first image loaded, which provides the area of the sky for which cutouts should be sought.
 */
void pqWindowImage::checkVLKB(vlvaStackImage *stackImage)
{
    bool doSearch = true;//settings.value("vlkb.search", false).toBool();
    if (doSearch) {

        //Create loading warning/notification
        this->ui->elementListWidget->insertRow(0);
        QTableWidgetItem *loadingItem = new QTableWidgetItem();
        loadingItem->setText("Loading...");
        QFont font = QFont();
        font.setFamily(font.defaultFamily());
        font.setBold(true);
        font.setPointSize(12);
        loadingItem->setFont(font);
        loadingItem->setBackground(Qt::darkGray);
        ui->elementListWidget->setItem(0, 0, loadingItem);

        double coords[2], rectSize[2];
        auto filePath = stackImage->getFitsHeaderPath();
        AstroUtils::GetCenterCoords(filePath, coords);
        AstroUtils::GetRectSize(filePath, rectSize);
        VialacteaInitialQuery *vq = new VialacteaInitialQuery;
        connect(vq, &VialacteaInitialQuery::searchDone,
                [vq, this](QList<QMap<QString, QString>> results) {
                    this->setVLKBElementsList(results);
                    //Add some sort of notification that results have been added to widget
                    vq->deleteLater();
                    QGraphicsColorizeEffect *eff = new QGraphicsColorizeEffect(this);
                    this->ui->elementListWidget->setGraphicsEffect(eff);
                    QPropertyAnimation *a = new QPropertyAnimation(eff,"strength");
                    a->setDuration(5000);
                    a->setStartValue(1);
                    a->setEndValue(0);
                    a->setEasingCurve(QEasingCurve::OutBack);
                    a->start(QPropertyAnimation::DeleteWhenStopped);
                    connect(a,&QPropertyAnimation::finished,this,[this](){
                        QGraphicsColorizeEffect *eff = new QGraphicsColorizeEffect(this);
                        this->ui->elementListWidget->setGraphicsEffect(eff);
                        QPropertyAnimation *a = new QPropertyAnimation(eff,"strength");
                        a->setDuration(100);
                        a->setStartValue(0);
                        a->setEndValue(0);
                        a->start(QPropertyAnimation::DeleteWhenStopped);});//Undo the recolour
                });

        vq->searchRequest(coords[0], coords[1], rectSize[0], rectSize[1]);
    }
}

/**
 * @brief pqWindowImage::downloadFromVLKB
 * This function interacts with the proxy on the server to download a file from the VLKB.
 * @param URL
 * The URL of the image to be downloaded, provided from the element list.
 */
void pqWindowImage::downloadFromVLKB(std::string URL)
{
    if (auto prop = vlkbManagerProxy->GetProperty("DownloadFile"))
    {
        vtkSMPropertyHelper(prop).Set(URL.c_str());
        vlkbManagerProxy->UpdateVTKObjects();
        throwMessage("Result from DownloadFile for file:", URL.c_str());
    }
    else
        throwError("vlkbManagerProxy not instantiated!", "Investigate allocation of the proxy!");
}

void pqWindowImage::checkDownloads(std::string URL)
{
    if (vlkbManagerProxy)
    {
        vtkClientServerStream stream;
        stream << vtkClientServerStream::Invoke
               << VTKOBJECT(vlkbManagerProxy)
               << "GetFileStatus" << URL.c_str()
               << vtkClientServerStream::End;

        vtkSMSession* session = vlkbManagerProxy->GetSession();
        session->ExecuteStream(
                /*location*/ vlkbManagerProxy->GetLocation(),
                /*stream*/ stream,
                /* ignore_errores*/ false);

        vtkClientServerStream result = session->GetLastResult(vlkbManagerProxy->GetLocation());
        if (result.GetNumberOfMessages() == 1 && result.GetNumberOfArguments(0) == 1)
        {
            std::string aresult;
            result.GetArgument(0, 0, &aresult);
            throwMessage("Result from ExecuteStream:", aresult.c_str());
        }
    }
    else
        throwError("vlkbManagerProxy not instantiated!", "Investigate allocation of the proxy!");
}

/**
 * @brief pqWindowImage::positionImage
 * Function that positions and scales an image in the view according to its WCS coordinates, as determined by the first image to be uploaded.
 * @param stackImage The image that is to be positioned
 * @param setBasePos If true, this is the first image in the stack and all others should use this as reference position
 * @return True if everything is successful, false if any unsuccessful
 */
int pqWindowImage::positionImage(vlvaStackImage *stackImage, bool setBasePos)
{
    auto skyCoords = new double[2];
    auto *coord = new double[3];
    double angle = 0, x1, y1;
    auto fitsPath = stackImage->getFitsHeaderPath();
    if (setBasePos)
        refImageFits = fitsPath;
    try {
        AstroUtils().xy2sky(fitsPath, 0, 0, skyCoords, 3);
        AstroUtils().sky2xy(refImageFits, skyCoords[0],
                            skyCoords[1], coord);

        x1 = coord[0];
        y1 = coord[1];
        if (setBasePos){
            refImageBottomLeftCornerCoords.first = x1;
            refImageBottomLeftCornerCoords.second = y1;
        }

        AstroUtils().xy2sky(fitsPath, 0, 100, skyCoords, 3);
        AstroUtils().sky2xy(refImageFits, skyCoords[0],
                            skyCoords[1], coord);

        if (x1 != coord[0]) {
            double m = std::fabs((coord[1] - y1) / (coord[0] - x1));
            angle = 90 - std::atan(m) * 180 / M_PI;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Error when trying to extract WCS position from FITS header file!" << std::endl;
        std::stringstream eString, eInfo;
        eString << "Error when trying to extract WCS position from FITS header file " << stackImage->getFitsFileName().toStdString() << "!";
        eInfo << "Please file a bug report and include the specific files you were trying to load.";
        throwError(eString.str().c_str(), eInfo.str().c_str());
        return 0;
    }

    try
    {
        double scaledPixel;
        if (setBasePos){
            refImagePixScaling = AstroUtils().arcsecPixel(fitsPath);
        }
        scaledPixel = AstroUtils().arcsecPixel(fitsPath) / refImagePixScaling;

        int sum = 0;
        sum += stackImage->setOrientation(0, 0, angle);
        sum += stackImage->setScale(scaledPixel, scaledPixel);
        sum += stackImage->setXYPosition(x1, y1);
        if (sum != 3){
            std::cerr << "Error when placing/rotating/scaling image " << stackImage->getFitsFileName().toStdString() << "!" << std::endl;
            std::stringstream eString, eInfo;
            eString << "Error when placing/rotating/scaling image " << stackImage->getFitsFileName().toStdString() << "!";
            eInfo << "Please file a bug report and include the specific files you were trying to load.";
            throwError(eString.str().c_str(), eInfo.str().c_str());
            return 0;
        }
        if (!setBasePos && !overlaps(this->images, stackImage)){
            std::cerr << "File " << stackImage->getFitsFileName().toStdString() << " does not overlap any currently displayed image!" << std::endl;
            std::stringstream eString, eInfo;
            eString << "File " << stackImage->getFitsFileName().toStdString() << " does not overlap any currently displayed image!";
            eInfo << "Images must overlap at least partially to be shown in a stack. Remove all current images and try again if you want to load this image.";
            throwError(eString.str().c_str(), eInfo.str().c_str());

            return 0;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Error when trying to position image via proxies!" << std::endl;
        std::stringstream eString, eInfo;
        eString << "Error when trying to position image via proxies!";
        eInfo << "Please file a bug report and include the specific files you were trying to load.";
        throwError(eString.str().c_str(), eInfo.str().c_str());
        return 0;
    }
    return 1;
}

/**
 * @brief pqWindowImage::createView
 * Function that creates the window in which the data is visualized.
 */
void pqWindowImage::createView()
{
    viewImage = qobject_cast<pqRenderView *>(
            builder->createView(pqRenderView::renderViewType(), server));
    viewImage->widget()->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->PVLayout->addWidget(viewImage->widget());
}

/**
 * @brief pqWindowImage::tableCheckboxClicked
 * @param cb The index of the checkbox in the table
 * @param status
 */
void pqWindowImage::tableCheckboxClicked(int cb, bool status)
{
    auto im = images.at(cb);
    if (im->isEnabled()){
        im->setActive(false);
    }
    else{
        im->setActive(true);
    }
    updateUI();
}

/**
 * @brief pqWindowImage::movedLayersRow
 * This slot catches the signal that is sent out when the user rearranges items in the list view.
 * @param sourceParent The parent object for the source (always the same as destination in our case).
 * @param sourceStart The index where the item started
 * @param sourceEnd
 * @param destinationParent The parent object for the destination (always the same as source in our case).
 * @param destinationRow The index where the item ended.
 */
void pqWindowImage::movedLayersRow(const QModelIndex &sourceParent, int sourceStart, int sourceEnd, const QModelIndex &destinationParent, int destinationRow)
{
    if (sourceStart > destinationRow) { // down

        for (int i = sourceStart - 1; i >= destinationRow; i--) {
            images.at(i)->setIndex(i + 1);
        }

        images.at(sourceStart)->setIndex(destinationRow);
        this->activeIndex = destinationRow;
    } else { // up

        for (int i = sourceStart + 1; i < destinationRow; i++) {
            images.at(i)->setIndex(i - 1);
        }
        images.at(sourceStart)->setIndex(destinationRow - 1);
        this->activeIndex = destinationRow - 1;
    }
    updateUI();
}

/**
 * @brief pqWindowImage::on_cmbxLUTSelect_currentIndexChanged
 * Changes the colour map used to visualize the image if the
 * user selects from the combo box.
 * @param index The index of the selected item in the combobox,
 * used to retrieve the name of the colour map.
 */
void pqWindowImage::on_cmbxLUTSelect_currentIndexChanged(int index)
{
    // If the UI is being initialised, ignore this command.
    if (index >= 0 && !clmInit) {
        changeColorMap(ui->cmbxLUTSelect->itemText(index));
        setLogScale(this->images.at(activeIndex)->getLogScale());
    }
}

/**
 * @brief pqWindowImage::on_linearRadioButton_toggled
 * Function that changes the scaling used for visualizing the image
 * to be appropriate to which radio button is selected.
 * @param checked Bool that says if the linear radio button is selected.
 */
void pqWindowImage::on_linearRadioButton_toggled(bool checked)
{
    // If in the process of initialising the UI, ignore this command.
    if (clmInit)
        return;
    setLogScale(!checked);
}

/**
 * @brief pqWindowImage::on_opacitySlider_actionTriggered
 * Function that checks what action was triggered by the slider and
 * sets a boolean that is used to determine if the changed value should
 * be sent to the server.
 * @param action The enum value for the action triggered (see documentation
 * for QAbstractSlider::actionTriggered(int action) for details).
 */
void pqWindowImage::on_opacitySlider_actionTriggered(int action)
{
    // Set the slider to only update the image when released or changed by keyboard/mouse
    switch (action) {
    case QSlider::SliderNoAction:
        loadOpacityChange = false;
        break;
    case QSlider::SliderMove:
        loadOpacityChange = false;
        break;
    default:
        loadOpacityChange = true;
        break;
    }
}

/**
 * @brief pqWindowImage::on_opacitySlider_valueChanged
 * Function that updates the opacity value if the slider's value is changed
 * by keyboard or mouse click (not drag).
 * @param value The new value on the opacity slider.
 */
void pqWindowImage::on_opacitySlider_valueChanged(int value)
{
    // If in the process of initialising the UI, ignore this command.
    if (clmInit)
        return;
    // Check what caused the value to change, and don't update while sliding.
    if (!loadOpacityChange)
        return;

    // Opacity is from 0 to 1, slider is from 0 to 100. So divide by 100 to get value to be sent to
    // server.
    float opacityValue = ui->opacitySlider->value() / 100.0;

    setOpacity(opacityValue);
    loadOpacityChange = false;
}

/**
 * @brief pqWindowImage::on_opacitySlider_sliderReleased
 * Function that updates the opacity value when the slider is released
 * after being dragged.
 */
void pqWindowImage::on_opacitySlider_sliderReleased()
{
    // Opacity is from 0 to 1, slider is from 0 to 100. So divide by 100 to get value to be sent to
    // server.
    float opacityValue = ui->opacitySlider->value() / 100.0;

    setOpacity(opacityValue);
    loadOpacityChange = false;
}

/**
 * @brief pqWindowImage::on_btnAddImageToStack_clicked
 * Function that creates a file dialog to select a file on the server,
 * and a subset selector dialog if a file is selected. The subset selector
 * dialog calls the addImageToStack function on close.
 */
void pqWindowImage::on_btnAddImageToStack_clicked()
{
    vtkSMReaderFactory *readerFactory = vtkSMProxyManager::GetProxyManager()->GetReaderFactory();
    QString filters = readerFactory->GetSupportedFileTypes(server->session());
    pqFileDialog dialog(server, this, QString(), QString(), filters);
    dialog.setFileMode(pqFileDialog::ExistingFile);
    if (dialog.exec() == pqFileDialog::Accepted) {
        QString file = dialog.getSelectedFiles().first();
        auto subsetSelector = new SubsetSelectorDialog(this);
        subsetSelector->setAttribute(Qt::WA_DeleteOnClose);
        connect(subsetSelector, &SubsetSelectorDialog::subsetSelected, this, [=](const CubeSubset &subset){this->addImageToStack(file, subset);});
        subsetSelector->show();
        subsetSelector->activateWindow();
        subsetSelector->raise();
    }
}

/**
 * @brief pqWindowImage::on_btnRemoveImageFromStack_clicked
 * UI function that fires when user selecte
 */
void pqWindowImage::on_btnRemoveImageFromStack_clicked()
{
    removeImageFromStack(this->activeIndex);
}

/**
 * @brief pqWindowImage::on_lstImageList_itemClicked
 * Function that fires if the user clicks on an item in the item list.
 * @param item
 */
void pqWindowImage::on_lstImageList_itemClicked(QListWidgetItem *item)
{
    // If in the process of initialising or updating the UI, ignore this command.
    if (clmInit)
        return;
    if (!item)
        return;
    int index = this->ui->lstImageList->row(item);
    // If clicking on an already selected image, do nothing.
    if (this->activeIndex == index) return;

    // Switch to image at index and update the UI.
    this->activeIndex = index;
    updateUI();
}

void pqWindowImage::on_lstImageList_itemChanged(QListWidgetItem *item)
{
    // If in the process of initialising or updating the UI, ignore this command.
    if (clmInit)
        return;

    setImageListCheckbox(item->listWidget()->row(item), item->checkState());
    updateUI();
}


void pqWindowImage::on_btnVLKBDown_clicked()
{
    downloadFromVLKB("TestingTesting");
}


void pqWindowImage::on_btnCheckVLKB_clicked()
{
    checkDownloads("TestingTesting");
}


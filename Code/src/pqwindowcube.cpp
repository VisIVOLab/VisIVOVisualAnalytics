#include "pqwindowcube.h"
#include "qmessagebox.h"
#include "ui_pqwindowcube.h"

#include "interactors/vtkinteractorstyleimagecustom.h"

#include "errorMessage.h"

#include "vtkNamedColors.h"
#include "vtkPolyDataMapper.h"
#include "vtkProperty.h"

#include "vtklegendscaleactor.h"

#include <pqActiveObjects.h>
#include <pqAlwaysConnectedBehavior.h>
#include <pqApplicationCore.h>
#include <pqLoadDataReaction.h>
#include <pqObjectBuilder.h>
#include <pqPipelineSource.h>
#include <pqRenderView.h>

#include <vtkCamera.h>
#include <vtkPVArrayInformation.h>
#include <vtkPVDataInformation.h>
#include <vtkPVDataMover.h>
#include <vtkPVDataSetAttributesInformation.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSMCoreUtilities.h>
#include <vtkSMPropertyHelper.h>
#include <vtkSMPVRepresentationProxy.h>
#include <vtkSMRenderViewProxy.h>
#include <vtkSMSessionProxyManager.h>
#include <vtkSMTransferFunctionManager.h>
#include <vtkSMTransferFunctionPresets.h>
#include <vtkSMTransferFunctionProxy.h>
#include <vtkSMUncheckedPropertyHelper.h>
#include <vtkSMViewProxy.h>
#include <vtkTable.h>

#include <QDebug>
#include <QDir>
#include <QMouseEvent>

#include <cmath>
#include <cstring>
#include <utility>

pqWindowCube::pqWindowCube(const QString &filepath, const CubeSubset &cubeSubset)
    : ui(new Ui::pqWindowCube),
      FitsFileName(QFileInfo(filepath).fileName()),
      cubeFilePath(filepath),
      cubeSubset(cubeSubset),
      currentSlice(-1),
      contourFilter(nullptr),
      contourFilter2D(nullptr)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(FitsFileName);
    connect(ui->actionVolGenerate, &QAction::triggered, this,
            &pqWindowCube::generateVolumeRendering);

    // Opacity menu actions
    auto opacityGroup = new QActionGroup(this);
    opacityGroup->addAction(ui->action0);
    opacityGroup->addAction(ui->action25);
    opacityGroup->addAction(ui->action50);
    opacityGroup->addAction(ui->action75);
    opacityGroup->addAction(ui->action100);

    // View menu show action group
    auto viewGroup = new QActionGroup(this);
    viewGroup->addAction(ui->actionView_Slice);
    viewGroup->addAction(ui->actionView_Moment_Map);

    logScaleActive = false;

    auto scalingGroup = new QActionGroup(this);
    auto scalingLinear = new QAction("Linear", scalingGroup);
    scalingLinear->setCheckable(true);
    scalingLinear->setChecked(true);
    scalingGroup->addAction(scalingLinear);
    auto scalingLog = new QAction("Logarithm", scalingGroup);
    scalingLog->setCheckable(true);
    connect(scalingLinear, &QAction::triggered, this, [=]() { setLogScale(false); });
    connect(scalingLog, &QAction::triggered, this, [=]() { setLogScale(true); });
    ui->menuScaling->addActions(scalingGroup->actions());

    // Create LUTs menu actions
    auto presets = vtkSMTransferFunctionPresets::GetInstance();
    auto lutGroup = new QActionGroup(this);
    for (int i = 0; i < presets->GetNumberOfPresets(); ++i) {
        QString name = QString::fromStdString(presets->GetPresetName(i));
        auto lut = new QAction(name, lutGroup);
        lut->setCheckable(true);
        if (name == "Grayscale")
            lut->setChecked(true);

        lutGroup->addAction(lut);
        connect(lut, &QAction::triggered, this, [=]() { changeColorMap(name); });
    }
    ui->menuPreset->addActions(lutGroup->actions());

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

    // Load Reactions
    CubeSource = pqLoadDataReaction::loadData({ filepath });
    SliceSource = pqLoadDataReaction::loadData({ filepath });

    MomentMapSource = pqLoadDataReaction::loadData({ filepath });

    fullSrc = NULL;

    // Handle Subset selection
    setSubsetProperties(cubeSubset);

    createViews();
    showOutline();

    // Fetch information from source and header and update UI
    readInfoFromSource();
    readHeaderFromSource();
    rms = getRMSFromHeader(fitsHeader);
    lowerBound = 3 * rms;
    upperBound = dataMax;
    ui->lowerBoundText->setText(QString::number(lowerBound, 'f', 4));
    ui->upperBoundText->setText(QString::number(upperBound, 'f', 4));
    ui->minCubeText->setText(QString::number(dataMin, 'f', 4));
    ui->maxCubeText->setText(QString::number(dataMax, 'f', 4));
    ui->rmsCubeText->setText(QString::number(rms, 'f', 4));

    // Show Legend Scale Actors in both renderers
    fitsHeaderPath = createFitsHeaderFile(fitsHeader);
    showLegendScaleActors();

    // Show slice and set default LUT
    showSlice();
    loadMomentMap();
    setSliceDatacube(1);
    changeColorMap("Grayscale");
    setLogScale(false);
    vtkSMPVRepresentationProxy::SetScalarBarVisibility(sliceProxy, viewSlice->getProxy(), true);
    vtkSMPVRepresentationProxy::SetScalarBarVisibility(momentProxy, viewMomentMap->getProxy(),
                                                       true);

    // Set up interactor to show pixel coordinates in the status bar
    pixCoordInteractorStyle->SetCoordsCallback(
            [this](const std::string &str) { showStatusBarMessage(str); });

    vtkNew<vtkInteractorStyleImageCustom> interactorStyle;
    interactorStyle->SetCoordsCallback(
            [this](const std::string &str) { showStatusBarMessage(str); });
    interactorStyle->SetLayerFitsReaderFunc(fitsHeaderPath.toStdString());
    interactorStyle->SetPixelZCompFunc([this]() { return currentSlice; });
    viewSlice->getViewProxy()->GetRenderWindow()->GetInteractor()->SetInteractorStyle(
            interactorStyle);
    viewMomentMap->getViewProxy()->GetRenderWindow()->GetInteractor()->SetInteractorStyle(
            interactorStyle);

    pixCoordInteractorStyle->SetLayerFitsReaderFunc(fitsHeaderPath.toStdString());
    pixCoordInteractorStyle->SetPixelZCompFunc([this]() { return currentSlice; });

    // Set up interactor for drawing PV slice line
    drawPVLineInteractorStyle->setLineValsCallback([this](float x1, float y1, float x2, float y2) {
        sendLineEndPoints(std::make_pair(x1, y1), std::make_pair(x2, y2));
    });
    drawPVLineInteractorStyle->setLineAbortCallback([this]() { endDrawLine(); });

    // Set initial interactor
    viewSlice->getViewProxy()->GetRenderWindow()->GetInteractor()->SetInteractorStyle(
            pixCoordInteractorStyle);

    viewSlice->resetDisplay();
    viewCube->resetDisplay();
    viewMomentMap->resetDisplay();
    viewSlice->render();
    viewCube->render();
    viewMomentMap->render();

    setMomentMapVisible(false);
}

pqWindowCube::~pqWindowCube()
{
    builder->destroy(CubeSource);
    builder->destroy(SliceSource);
    builder->destroy(MomentMapSource);

    if (this->fullSrc != NULL) {
        builder->destroy(fullSrc);
        this->fullSrc = NULL;
    }
    builder->destroySources(server);
    this->CubeSource = NULL;
    this->SliceSource = NULL;
    this->MomentMapSource = NULL;
    delete ui;
}

void pqWindowCube::readInfoFromSource()
{
    // Bounds
    auto fitsInfo = this->CubeSource->getOutputPort(0)->getDataInformation();
    fitsInfo->GetBounds(bounds);

    // Data range
    auto fitsImageInfo = fitsInfo->GetPointDataInformation()->GetArrayInformation("FITSImage");
    double dataRange[2];
    fitsImageInfo->GetComponentRange(0, dataRange);
    dataMin = dataRange[0];
    dataMax = dataRange[1];
}

void pqWindowCube::readHeaderFromSource()
{
    auto dataMoverProxy = vtk::TakeSmartPointer(serverProxyManager->NewProxy("misc", "DataMover"));
    vtkSMPropertyHelper(dataMoverProxy, "Producer").Set(this->CubeSource->getProxy());
    vtkSMPropertyHelper(dataMoverProxy, "PortNumber").Set(1);
    vtkSMPropertyHelper(dataMoverProxy, "SkipEmptyDataSets").Set(1);
    dataMoverProxy->UpdateVTKObjects();
    dataMoverProxy->InvokeCommand("Execute");

    auto dataMover = vtkPVDataMover::SafeDownCast(dataMoverProxy->GetClientSideObject());
    for (int table = 0; table < dataMover->GetNumberOfDataSets(); ++table) {
        vtkTable *headerTable = vtkTable::SafeDownCast(dataMover->GetDataSetAtIndex(table));
        for (vtkIdType i = 0; i < headerTable->GetNumberOfRows(); ++i) {
            fitsHeader.insert(QString::fromStdString(headerTable->GetValue(i, 0).ToString()),
                              QString::fromStdString(headerTable->GetValue(i, 1).ToString()));
        }
    }
}

QString pqWindowCube::createFitsHeaderFile(const FitsHeaderMap &fitsHeader)
{
    fitsfile *fptr;
    int status = 0;

    QString headerFile = QDir::homePath()
                                 .append(QDir::separator())
                                 .append("VisIVODesktopTemp")
                                 .append(QDir::separator())
                                 .append(this->FitsFileName);

    fits_create_file(&fptr, ("!" + headerFile).toStdString().c_str(), &status);
    fits_update_key_log(fptr, "SIMPLE", TRUE, "", &status);

    int datatype;
    foreach (const auto &key, fitsHeader.keys()) {
        const auto &value = fitsHeader[key];
        if (key.compare("SIMPLE") == 0) {
            continue;
        }

        if (strchr(value.toStdString().c_str(), '\'')) {
            datatype = TSTRING;
        } else if (strchr(value.toStdString().c_str(), '\"'))
            datatype = TSTRING;
        else if (strchr(value.toStdString().c_str(), '.'))
            datatype = TDOUBLE;
        else if (isdigit(value.toStdString().c_str()[0]))
            datatype = TLONG;
        else if (strcasecmp(value.toStdString().c_str(), "TRUE") == 0
                 || strcasecmp(value.toStdString().c_str(), "FALSE") == 0)
            datatype = TLOGICAL;
        else
            datatype = TLONG;

        switch (datatype) {
        case TSTRING: {
            // remove quotes
            char *cstr = new char[value.toStdString().length() + 1];
            std::strcpy(cstr, value.toStdString().c_str());
            std::string str(cstr);
            std::string result = str.substr(1, str.size() - 2);

            fits_update_key_str(fptr, key.toStdString().c_str(), result.c_str(), "", &status);
            break;
        }
        case TFLOAT:
            fits_update_key_flt(fptr, key.toStdString().c_str(), atof(value.toStdString().c_str()),
                                -7, "", &status);
            break;
        case TDOUBLE:
            fits_update_key_dbl(fptr, key.toStdString().c_str(), atof(value.toStdString().c_str()),
                                -15, "", &status);
            break;
        case TLONG: {
            fits_update_key_lng(fptr, key.toStdString().c_str(), atol(value.toStdString().c_str()),
                                "", &status);
            break;
        }
        }
    }
    fits_close_file(fptr, &status);

    return headerFile;
}

double pqWindowCube::getRMSFromHeader(const FitsHeaderMap &fitsHeader)
{
    if (fitsHeader.contains("RMS")) {
        return fitsHeader.value("RMS").toDouble();
    }

    int n = fitsHeader.value("MSn").toInt();
    if (n <= 0) {
        qDebug() << Q_FUNC_INFO
                 << "Missing required header keywords to calculate RMS from partial values";
        return 0.0;
    }

    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        QString keyword("MS");
        keyword.append(QString::number(i));
        sum += fitsHeader.value(keyword).toDouble();
    }

    return std::sqrt(sum / n);
}

void pqWindowCube::createViews()
{
    viewCube = qobject_cast<pqRenderView *>(
            builder->createView(pqRenderView::renderViewType(), server));
    viewCube->widget()->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    viewSlice = qobject_cast<pqRenderView *>(
            builder->createView(pqRenderView::renderViewType(), server));
    viewSlice->widget()->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    viewMomentMap = qobject_cast<pqRenderView *>(
            builder->createView(pqRenderView::renderViewType(), server));
    viewMomentMap->widget()->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    ui->PVLayout->addWidget(viewCube->widget());
    ui->PVLayout->addWidget(viewSlice->widget());
    ui->PVLayout->addWidget(viewMomentMap->widget());
    viewSlice->widget()->setVisible(true);
    viewMomentMap->widget()->setVisible(false);
}

void pqWindowCube::showOutline()
{
    auto outline =
            builder->createDataRepresentation(CubeSource->getOutputPort(0), viewCube)->getProxy();
    vtkSMPropertyHelper(outline, "Representation").Set("Outline");
    double red[3] = { 1.0, 0.0, 0.0 };
    vtkSMPropertyHelper(outline, "AmbientColor").Set(red, 3);
    outline->UpdateVTKObjects();
}

void pqWindowCube::showLegendScaleActors()
{
    // Show Legend Scale Actor in both renderers
    auto legendActorCube = vtkSmartPointer<vtkLegendScaleActor>::New();
    legendActorCube->LegendVisibilityOff();
    legendActorCube->setFitsHeader(fitsHeaderPath.toStdString());
    auto rwCube = viewCube->getViewProxy()->GetRenderWindow()->GetRenderers();
    vtkRenderer::SafeDownCast(rwCube->GetItemAsObject(1))->AddActor(legendActorCube);

    auto legendActorSlice = vtkSmartPointer<vtkLegendScaleActor>::New();
    legendActorSlice->LegendVisibilityOff();
    legendActorSlice->setFitsHeader(fitsHeaderPath.toStdString());
    auto rwSlice = viewSlice->getViewProxy()->GetRenderWindow()->GetRenderers();
    rwSlice->GetFirstRenderer()->AddActor(legendActorSlice);
    vtkRenderer::SafeDownCast(rwSlice->GetItemAsObject(1))->AddActor(legendActorSlice);

    auto legendActorMoment = vtkSmartPointer<vtkLegendScaleActor>::New();
    legendActorMoment->LegendVisibilityOff();
    legendActorMoment->setFitsHeader(fitsHeaderPath.toStdString());
    auto rwMoment = viewMomentMap->getViewProxy()->GetRenderWindow()->GetRenderers();
    rwMoment->GetFirstRenderer()->AddActor(legendActorMoment);
    vtkRenderer::SafeDownCast(rwMoment->GetItemAsObject(1))->AddActor(legendActorMoment);
}

void pqWindowCube::showSlice()
{
    // Slice in Cube Renderer

    cubeSliceProxy = builder->createDataRepresentation(this->CubeSource->getOutputPort(0), viewCube)
                             ->getProxy();
    vtkSMPropertyHelper(cubeSliceProxy, "Representation").Set("Slice");
    vtkSMPVRepresentationProxy::SetScalarColoring(cubeSliceProxy, "FITSImage",
                                                  vtkDataObject::POINT);
    cubeSliceProxy->UpdateVTKObjects();

    // vtkNew<vtkSMTransferFunctionManager> mgr;
    // lutProxy = vtkSMTransferFunctionProxy::SafeDownCast(
    //                                                     mgr->GetColorTransferFunction("FITSImage",
    // cubeSliceProxy->GetSessionProxyManager()));

    // Filter to extract the slice (used for slice data range and contour 2D)
    // extractGrid = builder->createFilter("filters", "ExtractGrid", this->CubeSource);

    // Setup reader to get a slice using SubExtent Property
    int extent[6] = { -1, -1, -1, -1, 0, 0 };
    auto slicePropertyProxy = this->SliceSource->getProxy();
    vtkSMPropertyHelper(slicePropertyProxy, "ReadSubExtent").Set(true);
    vtkSMPropertyHelper(slicePropertyProxy, "SubExtent").Set(extent, 6);
    slicePropertyProxy->UpdateVTKObjects();
    SliceSource->updatePipeline();

    // Slice Render (right view)
    sliceProxy = builder->createDataRepresentation(this->SliceSource->getOutputPort(0), viewSlice)
                         ->getProxy();
    vtkSMPropertyHelper(sliceProxy, "Representation").Set("Slice");
    vtkSMPVRepresentationProxy::SetScalarColoring(sliceProxy, "FITSImage", vtkDataObject::POINT);

    vtkNew<vtkSMTransferFunctionManager> mgr;
    lutProxy = vtkSMTransferFunctionProxy::SafeDownCast(
            mgr->GetColorTransferFunction("FITSImage", sliceProxy->GetSessionProxyManager()));

    ui->sliceSlider->setRange(1, bounds[5] + 1);
    ui->sliceSpinBox->setRange(1, bounds[5] + 1);
    this->zDepth = bounds[5] + 2;
}

/**
 * @brief pqWindowCube::loadMomentMap
 * This function calls the reader on the server to load the given file
 * with the ReadAsType set to Moment Map.
 */
void pqWindowCube::loadMomentMap()
{

    // Set the appropriate properties
    auto momentPropProxy = this->MomentMapSource->getProxy();
    vtkSMPropertyHelper(momentPropProxy, "ReadAsType").Set(1);
    vtkSMPropertyHelper(momentPropProxy, "MomentOrder").Set(0);
    momentPropProxy->UpdateVTKObjects();
    MomentMapSource->updatePipeline();

    // Link the data to the correct view and create a colour mapp
    momentProxy = builder->createDataRepresentation(this->MomentMapSource->getOutputPort(0),
                                                    viewMomentMap)
                          ->getProxy();
    vtkSMPropertyHelper(momentProxy, "Representation").Set("Slice");
    std::string name = "FITSImage" + std::to_string(momentOrder);
    vtkSMPVRepresentationProxy::SetScalarColoring(momentProxy, name.c_str(), vtkDataObject::POINT);

    vtkNew<vtkSMTransferFunctionManager> mgr;
    momentLUTProxy = vtkSMTransferFunctionProxy::SafeDownCast(
            mgr->GetColorTransferFunction(name.c_str(), momentProxy->GetSessionProxyManager()));

    viewMomentMap->resetDisplay();
    viewMomentMap->render();
}

/**
 * @brief pqWindowCube::showMomentMap
 * This function is used to switch between the different moment map orders.
 * @param order The order to switch to.
 */
void pqWindowCube::showMomentMap(int order)
{
    // Change the properties on the server to the new values.
    momentOrder = order;
    auto momentPropProxy = this->MomentMapSource->getProxy();
    vtkSMPropertyHelper(momentPropProxy, "ReadAsType").Set(1);
    vtkSMPropertyHelper(momentPropProxy, "MomentOrder").Set(order);
    momentPropProxy->UpdateVTKObjects();
    MomentMapSource->updatePipeline();
    this->momentProxy->UpdateVTKObjects();

    // Create a new colour map to match the values of the new moment map order.
    vtkNew<vtkSMTransferFunctionManager> mgr;
    std::string name = "FITSImage" + std::to_string(momentOrder);
    momentLUTProxy = vtkSMTransferFunctionProxy::SafeDownCast(
            mgr->GetColorTransferFunction(name.c_str(), momentProxy->GetSessionProxyManager()));

    vtkSMPVRepresentationProxy::SetScalarColoring(momentProxy, name.c_str(), vtkDataObject::POINT);
    changeColorMap(currentColorMap);

    // Set the moment map to be visible if it wasn't before.
    setMomentMapVisible(true);
    viewMomentMap->resetDisplay();
    viewMomentMap->render();
}

void pqWindowCube::showStatusBarMessage(const std::string &msg)
{
    ui->statusBar->showMessage(QString::fromStdString(msg));
}

void pqWindowCube::setSubsetProperties(const CubeSubset &subset)
{
    auto sourceProxy = CubeSource->getProxy();
    vtkSMPropertyHelper(sourceProxy, "ReadSubExtent").Set(subset.ReadSubExtent);
    vtkSMPropertyHelper(sourceProxy, "SubExtent").Set(subset.SubExtent, 6);
    vtkSMPropertyHelper(sourceProxy, "AutoScale").Set(subset.AutoScale);
    vtkSMPropertyHelper(sourceProxy, "CubeMaxSize").Set(subset.CubeMaxSize);
    if (!subset.AutoScale)
        vtkSMPropertyHelper(sourceProxy, "ScaleFactor").Set(subset.ScaleFactor);
    sourceProxy->UpdateVTKObjects();
}

void pqWindowCube::updateVelocityText()
{
    double crval3 = fitsHeader.value("CRVAL3").toDouble();
    double cdelt3 = fitsHeader.value("CDELT3").toDouble();
    double crpix3 = fitsHeader.value("CRPIX3").toDouble();

    double initSlice = crval3 - (cdelt3 * (crpix3 - 1));
    double velocity = initSlice + cdelt3 * currentSlice;

    if (fitsHeader.value("CUNIT3").startsWith("m")) {
        // Return value in km/s
        velocity /= 1000;
    }
    ui->velocityText->setText(QString::number(velocity).append(" Km/s"));
}

/**
 * @brief pqWindowCube::updateMinMax
 * This function updates the min/max values on the UI (above the 2D view).
 * @param moment The moment order currently active (ignored if a moment map is not displayed).
 */
void pqWindowCube::updateMinMax(bool moment)
{
    vtkPVDataInformation *dataInformation;
    vtkPVArrayInformation *fitsImageInfo;
    std::string name = "FITSImage" + std::to_string(momentOrder);
    // Check if a moment map is displayed
    if (moment) {
        dataInformation = this->MomentMapSource->getOutputPort(0)->getDataInformation();
        fitsImageInfo =
                dataInformation->GetPointDataInformation()->GetArrayInformation(name.c_str());
    }
    // Otherwise, use the information from the slice view.
    else {
        dataInformation = this->SliceSource->getOutputPort(0)->getDataInformation();
        fitsImageInfo =
                dataInformation->GetPointDataInformation()->GetArrayInformation("FITSImage");
    }

    if (!fitsImageInfo) {
        std::cerr << "Error! Could not acquire fitsImageInfo from array name \"" << name << "\"!"
                  << std::endl;
        std::stringstream eString, eInfo;
        eString << "Error when trying to extract information from array name \"" << name << "\"!";
        eInfo << "Please file a bug report on the repository with details of what you were "
                 "attempting to do.";
        throwError(eString.str().c_str(), eInfo.str().c_str());
        return;
    }

    double dataRange[2];
    fitsImageInfo->GetComponentRange(0, dataRange);
    ui->minSliceText->setText(QString::number(dataRange[0], 'f', 4));
    ui->maxSliceText->setText(QString::number(dataRange[1], 'f', 4));
    if (moment) {
        momentLUTProxy->RescaleTransferFunction(dataRange, true);
        momentLUTProxy->UpdateVTKObjects();
    }
}

void pqWindowCube::setThreshold(double threshold)
{
    auto filterProxy = contourFilter->getProxy();
    vtkSMPropertyHelper(filterProxy, "ContourValues").Set(threshold);
    filterProxy->UpdateVTKObjects();
    contourFilter->updatePipeline();
    viewCube->render();
}

void pqWindowCube::showContours()
{
    removeContours();

    contourFilter2D = builder->createFilter("filters", "Contour", this->SliceSource);
    auto contourProxy = contourFilter2D->getProxy();

    int level = ui->levelText->text().toInt();
    double min = ui->lowerBoundText->text().toDouble();
    double max = ui->upperBoundText->text().toDouble();

    if (level == 1) {
        vtkSMPropertyHelper(contourFilter2D->getProxy(), "ContourValues").Set(min);
    } else {
        for (int i = 0; i < level; ++i) {
            double val = min + i * (max - min) / (level - 1);
            vtkSMPropertyHelper(contourFilter2D->getProxy(), "ContourValues").Set(i, val);
        }
    }

    contourProxy->UpdateVTKObjects();
    contourFilter2D->updatePipeline();

    auto contourSurface =
            builder->createDataRepresentation(contourFilter2D->getOutputPort(0), viewSlice)
                    ->getProxy();
    vtkSMPropertyHelper(contourSurface, "Representation").Set("Surface");
    auto separateProperty = vtkSMPVRepresentationProxy::SafeDownCast(contourSurface)
                                    ->GetProperty("UseSeparateColorMap");
    vtkSMPropertyHelper(separateProperty).Set(1);
    vtkSMPVRepresentationProxy::SetScalarColoring(contourSurface, "FITSImage",
                                                  vtkDataObject::POINT);

    contourSurface->UpdateVTKObjects();
    viewSlice->render();
}

void pqWindowCube::removeContours()
{
    if (contourFilter2D) {
        builder->destroy(contourFilter2D);
        contourFilter2D = nullptr;
        viewSlice->render();
    }
}

/**
 * @brief pqWindowCube::setMomentMapVisible
 * This function switches the 3D display between the view of the slice and the moment map.
 * @param val True if a moment map should be displayed, false if the slice view should be displayed.
 */
void pqWindowCube::setMomentMapVisible(bool val)
{
    viewMomentMap->widget()->setVisible(val);
    viewSlice->widget()->setVisible(!val);
    ui->sliceGroupBox->setTitle(val ? "Moment" : "Slice");
    updateMinMax(val);
    viewSlice->resetDisplay();
    viewSlice->render();
    viewMomentMap->resetDisplay();
    viewMomentMap->render();
}

void pqWindowCube::sendLineEndPoints(std::pair<float, float> start, std::pair<float, float> end)
{
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setText("Do you want to load a Position-Velocity slice?");
    msgBox.setInformativeText("Generating the PV slice will take time if the cube is large.");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Yes);
    msgBox.setEscapeButton(QMessageBox::Cancel);
    int ret = msgBox.exec();
    switch (ret) {
    case QMessageBox::Yes:
        break;
    case QMessageBox::Cancel:
        endDrawLine();
        return;
        break;
    default:
        break;
    }

    bool invalid = start.first < bounds[0] || start.first > bounds[1] || start.second < bounds[2]
            || start.second > bounds[3];
    invalid = invalid || end.first < bounds[0] || end.first > bounds[1] || end.second < bounds[2]
            || end.second > bounds[3];
    if (invalid) {
        std::stringstream eString, eInfo;
        eString << "Error: trying to draw a PV slice beyond the edges of the available data!";
        eInfo << "Draw a line only on the slice.";
        //        throwError(eString.str().c_str(), eInfo.str().c_str());
        return;
    }
    int x1, y1, x2, y2;
    x1 = std::round(start.first);
    y1 = std::round(start.second);
    x2 = std::round(end.first);
    y2 = std::round(end.second);
    this->showPVSlice(std::make_pair(x1, y1), std::make_pair(x2, y2));
}

void pqWindowCube::endDrawLine()
{
    viewSlice->getViewProxy()->GetRenderWindow()->GetInteractor()->SetInteractorStyle(
            pixCoordInteractorStyle);
    this->ui->actionDraw_PV_line->setChecked(false);
    drawing = false;
    auto viewProxy = viewSlice->getProxy();
    vtkSMPropertyHelper(viewProxy, "ShowAnnotation").Set(1);
    viewProxy->UpdateVTKObjects();
    endPVSlice();
}

void pqWindowCube::showPVSlice()
{
    int start1 = 50, start2 = 50;
    int end1 = 100, end2 = 100;
    showPVSlice(std::make_pair(start1, start2), std::make_pair(end1, end2));
}

void pqWindowCube::showPVSlice(std::pair<int, int> start, std::pair<int, int> end)
{
    CubeSource->getProxy()->UpdatePropertyInformation();
    int sF;
    vtkSMPropertyHelper(CubeSource->getProxy(), "ScaleFactorInfo").Get(&sF);
    if (sF != 1) {
        if (fullSrc == NULL)
            fullSrc = pqLoadDataReaction::loadData({ cubeFilePath });
        //        CubeSubset sub;
        //        sub.AutoScale = false;
        //        sub.ScaleFactor = 1;
        //        int x1, y1, z1, z2;
        //        if (cubeSubset.ReadSubExtent){
        //            x1 = cubeSubset.SubExtent[0];
        //            y1 = cubeSubset.SubExtent[2];
        //            z1 = cubeSubset.SubExtent[4];
        //            z2 = cubeSubset.SubExtent[5];
        //        }
        //        else{
        //            x1 = bounds[0];
        //            y1 = bounds[2];
        //            z1 = bounds[4];
        //            z2 = bounds[5];
        //        }

        //        std::pair<int, int> botLeft = std::make_pair(std::min(start.first, end.first),
        //        std::min(start.second, end.second)); std::pair<int, int> topRight =
        //        std::make_pair(std::max(start.first, end.first), std::max(start.second,
        //        end.second)); sub.SubExtent[0] = x1 + botLeft.first; sub.SubExtent[1] = x1 +
        //        topRight.first; sub.SubExtent[2] = y1 + botLeft.second; sub.SubExtent[3] = y1 +
        //        topRight.second; sub.SubExtent[4] = z1; sub.SubExtent[5] = z2;

        //        auto sourceProxy = fullSrc->getProxy();
        //        vtkSMPropertyHelper(sourceProxy, "ReadSubExtent").Set(true);
        //        vtkSMPropertyHelper(sourceProxy, "SubExtent").Set(sub.SubExtent, 6);
        //        vtkSMPropertyHelper(sourceProxy, "AutoScale").Set(sub.AutoScale);
        //        vtkSMPropertyHelper(sourceProxy, "CubeMaxSize").Set(sub.CubeMaxSize);
        //        sourceProxy->UpdateVTKObjects();
        //        start = std::make_pair(start.first - botLeft.first, start.second -
        //        botLeft.second); end = std::make_pair(end.first - botLeft.first, end.second -
        //        botLeft.second);
        pvSlice = new pqPVWindow(this->server, fullSrc, start, end, this);
    } else
        pvSlice = new pqPVWindow(this->server, this->CubeSource, start, end, this);
    connect(pvSlice, &pqPVWindow::closed, this, &pqWindowCube::endPVSlice);
    pvSlice->setAttribute(Qt::WA_Hover);
    pvSlice->show();
    pvSlice->activateWindow();
    pvSlice->raise();
}

void pqWindowCube::endPVSlice()
{
    this->drawPVLineInteractorStyle->removeArrow();
    viewSlice->render();
    viewCube->render();
}

void pqWindowCube::on_sliceSlider_valueChanged()
{
    // Check what caused the value to change, and don't update while sliding.
    if (!loadChange)
        return;

    int value = ui->sliceSlider->value();

    // Match slider and spinbox values
    if (ui->sliceSpinBox->value() != value) {
        ui->sliceSpinBox->setValue(value);
    }

    setSliceDatacube(value);
    loadChange = false;
}

void pqWindowCube::on_sliceSlider_actionTriggered(int action)
{
    // Set the slider to only update the image when released or changed by keyboard/mouse
    switch (action) {
    case QSlider::SliderNoAction:
        loadChange = false;
        break;
    case QSlider::SliderMove:
        loadChange = false;
        break;
    default:
        loadChange = true;
        break;
    }
}

void pqWindowCube::on_sliceSlider_sliderReleased()
{
    int value = ui->sliceSlider->value();

    // Match slider and spinbox values
    if (ui->sliceSpinBox->value() != value) {
        ui->sliceSpinBox->setValue(value);
    }

    setSliceDatacube(value);
    loadChange = false;
}

void pqWindowCube::on_sliceSpinBox_valueChanged(int arg1)
{
    // Match slider and spinbox values
    if (ui->sliceSlider->value() != arg1) {
        ui->sliceSlider->setValue(arg1);
        ui->sliceSlider->update();
    }

    setSliceDatacube(arg1);
}

void pqWindowCube::on_sliceSpinBox_editingFinished()
{
    int value = ui->sliceSpinBox->value();

    // Match slider and spinbox values
    if (ui->sliceSlider->value() != value) {
        ui->sliceSlider->setValue(value);
        ui->sliceSlider->update();
    }

    setSliceDatacube(value);
}

void pqWindowCube::setSliceDatacube(int value)
{
    if (currentSlice == (value - 1)) {
        // No need to update the slice
        return;
    }

    currentSlice = value - 1;
    updateVelocityText();

    // vtkSMPropertyHelper(sliceProxy, "Slice").Set(currentSlice);
    // vtkSMPropertyHelper(sliceProxy, "SliceMode").Set(VTK_XY_PLANE);
    //  Setup reader to get a slice using SubExtent Property
    int extent[6] = { -1, -1, -1, -1, currentSlice, currentSlice };
    auto slicePropertyProxy = this->SliceSource->getProxy();
    vtkSMPropertyHelper(slicePropertyProxy, "ReadSubExtent").Set(true);
    vtkSMPropertyHelper(slicePropertyProxy, "SubExtent").Set(extent, 6);
    slicePropertyProxy->UpdateVTKObjects();
    sliceProxy->UpdateVTKObjects();
    SliceSource->updatePipeline();

    CubeSource->getProxy()->UpdatePropertyInformation();
    int sF;
    vtkSMPropertyHelper(CubeSource->getProxy(), "ScaleFactorInfo").Get(&sF);
    int slicePos = std::round(((float)currentSlice) / sF);
    vtkSMPropertyHelper(cubeSliceProxy, "Slice").Set(slicePos);
    vtkSMPropertyHelper(cubeSliceProxy, "SliceMode").Set(VTK_XY_PLANE);
    cubeSliceProxy->UpdateVTKObjects();

    // Get slice data range and update UI
    /*
    auto extractGridProxy = extractGrid->getProxy();
    int selectedSlice[] = { (int)bounds[0], (int)bounds[1], (int)bounds[2],
                            (int)bounds[3], (currentSlice), (currentSlice) };
    vtkSMPropertyHelper(extractGridProxy, "VOI").Set(selectedSlice, 6);
    extractGridProxy->UpdateVTKObjects();
    extractGrid->updatePipeline();
    */

    auto dataInformation = this->SliceSource->getOutputPort(0)->getDataInformation();
    auto fitsImageInfo =
            dataInformation->GetPointDataInformation()->GetArrayInformation("FITSImage");
    double dataRange[2];
    fitsImageInfo->GetComponentRange(0, dataRange);
    ui->minSliceText->setText(QString::number(dataRange[0], 'f', 4));
    ui->maxSliceText->setText(QString::number(dataRange[1], 'f', 4));
    lutProxy->RescaleTransferFunction(dataRange, true);

    viewSlice->resetDisplay();
    viewSlice->render();
    viewCube->render();

    if (ui->contourCheckBox->isChecked()) {
        showContours();
    }
}

void pqWindowCube::generateVolumeRendering()
{
    if (contourFilter != nullptr) {
        return;
    }

    // Default contour value is lowerBound (RMS*3)
    contourFilter = builder->createFilter("filters", "Contour", this->CubeSource);
    ui->thresholdText->setText(QString::number(lowerBound, 'f', 4));
    setThreshold(lowerBound);

    contourProxy = builder->createDataRepresentation(contourFilter->getOutputPort(0), viewCube)
                           ->getProxy();
    vtkSMPVRepresentationProxy::SetScalarColoring(contourProxy, nullptr, 0);
    vtkSMPropertyHelper(contourProxy, "Representation").Set("Surface");
    vtkSMPropertyHelper(contourProxy, "Ambient").Set(0.5);
    vtkSMPropertyHelper(contourProxy, "Diffuse").Set(0.5);
    vtkSMPropertyHelper(contourProxy, "Opacity").Set(1);
    double red[3] = { 1.0, 0.0, 0.0 };
    vtkSMPropertyHelper(contourProxy, "AmbientColor").Set(red, 3);
    contourProxy->UpdateVTKObjects();
    viewCube->render();
}

void pqWindowCube::setLogScale(bool logScale)
{
    if (logScale) {
        double range[2];
        vtkSMTransferFunctionProxy::GetRange(lutProxy, range);
        vtkSMCoreUtilities::AdjustRangeForLog(range);

        logScaleActive = true;
        vtkSMTransferFunctionProxy::RescaleTransferFunction(lutProxy, range);
        vtkSMPropertyHelper(lutProxy, "UseLogScale").Set(1);

        vtkSMTransferFunctionProxy::GetRange(momentLUTProxy, range);
        vtkSMCoreUtilities::AdjustRangeForLog(range);
        vtkSMTransferFunctionProxy::RescaleTransferFunction(momentLUTProxy, range);
        vtkSMPropertyHelper(momentLUTProxy, "UseLogScale").Set(1);

        changeColorMap(currentColorMap);
    } else {
        logScaleActive = false;
        vtkSMTransferFunctionProxy::RescaleTransferFunctionToDataRange(lutProxy);
        vtkSMPropertyHelper(lutProxy, "UseLogScale").Set(0);
        vtkSMTransferFunctionProxy::RescaleTransferFunctionToDataRange(momentLUTProxy);
        vtkSMPropertyHelper(momentLUTProxy, "UseLogScale").Set(0);
        changeColorMap(currentColorMap);
    }
    lutProxy->UpdateVTKObjects();
    momentLUTProxy->UpdateVTKObjects();
    sliceProxy->UpdateVTKObjects();
    cubeSliceProxy->UpdateVTKObjects();
    momentProxy->UpdateVTKObjects();

    viewSlice->resetDisplay();

    viewMomentMap->resetDisplay();
    viewSlice->render();
    viewMomentMap->render();
    viewCube->render();
}

void pqWindowCube::changeColorMap(const QString &name)
{
    vtkSMProperty *lutProperty = sliceProxy->GetProperty("LookupTable");
    vtkSMProperty *momentLUTProperty = momentProxy->GetProperty("LookupTable");
    if (lutProperty && momentLUTProperty) {
        currentColorMap = name;
        auto presets = vtkSMTransferFunctionPresets::GetInstance();
        lutProxy->ApplyPreset(presets->GetFirstPresetWithName(name.toStdString().c_str()));
        vtkSMPropertyHelper(lutProperty).Set(lutProxy);
        lutProxy->UpdateVTKObjects();
        vtkSMPVRepresentationProxy::RescaleTransferFunctionToDataRange(sliceProxy, false, false);

        momentLUTProxy->ApplyPreset(presets->GetFirstPresetWithName(name.toStdString().c_str()));
        vtkSMPropertyHelper(momentLUTProperty).Set(momentLUTProxy);
        momentLUTProxy->UpdateVTKObjects();
        vtkSMPVRepresentationProxy::RescaleTransferFunctionToDataRange(sliceProxy, false, false);

        sliceProxy->UpdateVTKObjects();
        momentProxy->UpdateVTKObjects();

        viewSlice->resetDisplay();
        viewSlice->render();
        viewMomentMap->resetDisplay();
        viewMomentMap->render();
        viewCube->render();
    } else
        std::cerr << "Error with setting colour map!" << std::endl;
}

void pqWindowCube::on_actionFront_triggered()
{
    // Negative Z
    viewCube->resetViewDirection(0, 0, -1, 0, 1, 0);
    viewCube->render();
}

void pqWindowCube::on_actionBack_triggered()
{
    // Positive Z
    viewCube->resetViewDirection(0, 0, 1, 0, 1, 0);
    viewCube->render();
}

void pqWindowCube::on_actionTop_triggered()
{
    // Negative Y
    viewCube->resetViewDirection(0, -1, 0, 0, 0, 1);
    viewCube->render();
}

void pqWindowCube::on_actionRight_triggered()
{
    // Negative X, rotation 90
    viewCube->resetViewDirection(-1, 0, 0, 0, 0, 1);
    viewCube->getRenderViewProxy()->GetActiveCamera()->Roll(90);
    viewCube->render();
}

void pqWindowCube::on_actionBottom_triggered()
{
    // Positive Y
    viewCube->resetViewDirection(0, 1, 0, 0, 0, 1);
    viewCube->render();
}

void pqWindowCube::on_actionLeft_triggered()
{
    // Positive X, rotation -90
    viewCube->resetViewDirection(1, 0, 0, 0, 0, 1);
    viewCube->getRenderViewProxy()->GetActiveCamera()->Roll(-90);
    viewCube->render();
}

void pqWindowCube::on_thresholdText_editingFinished()
{
    if (contourFilter == nullptr) {
        return;
    }

    double threshold = ui->thresholdText->text().toDouble();
    // Clamp threshold
    threshold = fmin(fmax(threshold, lowerBound), upperBound);
    ui->thresholdText->setText(QString::number(threshold, 'f', 4));

    int tickPosition = 100 * (threshold - lowerBound) / (upperBound - lowerBound);
    ui->thresholdSlider->setValue(tickPosition);

    setThreshold(threshold);
}

void pqWindowCube::on_thresholdSlider_sliderReleased()
{
    if (contourFilter == nullptr) {
        return;
    }

    double threshold =
            (ui->thresholdSlider->value() * (upperBound - lowerBound) / 100) + lowerBound;
    ui->thresholdText->setText(QString::number(threshold, 'f', 4));
    setThreshold(threshold);
}

void pqWindowCube::on_contourCheckBox_toggled(bool checked)
{
    if (checked) {
        showContours();
    } else {
        removeContours();
    }
}

void pqWindowCube::on_levelText_editingFinished()
{
    if (ui->contourCheckBox->isChecked()) {
        showContours();
    }
}

void pqWindowCube::on_lowerBoundText_editingFinished()
{
    if (ui->contourCheckBox->isChecked()) {
        showContours();
    }
}

void pqWindowCube::on_upperBoundText_editingFinished()
{
    if (ui->contourCheckBox->isChecked()) {
        showContours();
    }
}

void pqWindowCube::on_action0_triggered()
{
    setVolumeRenderingOpacity(0);
}

void pqWindowCube::on_action25_triggered()
{
    setVolumeRenderingOpacity(0.25);
}

void pqWindowCube::on_action50_triggered()
{
    setVolumeRenderingOpacity(0.5);
}

void pqWindowCube::on_action75_triggered()
{
    setVolumeRenderingOpacity(0.75);
}

void pqWindowCube::on_action100_triggered()
{
    setVolumeRenderingOpacity(1);
}

void pqWindowCube::setVolumeRenderingOpacity(double opacity)
{
    vtkSMPropertyHelper(contourProxy, "Opacity").Set(opacity);
    contourProxy->UpdateVTKObjects();
    viewCube->render();
}

void pqWindowCube::on_actionView_Slice_triggered()
{
    setMomentMapVisible(false);
}

void pqWindowCube::on_actionView_Moment_Map_triggered()
{
    setMomentMapVisible(true);
}

void pqWindowCube::on_actionMomentOrder0_triggered()
{
    showMomentMap(0);
}

void pqWindowCube::on_actionMomentOrder1_triggered()
{
    showMomentMap(1);
}

void pqWindowCube::on_actionMomentOrder2_triggered()
{
    showMomentMap(2);
}

void pqWindowCube::on_actionMomentOrder6_triggered()
{
    showMomentMap(6);
}

void pqWindowCube::on_actionMomentOrder8_triggered()
{
    showMomentMap(8);
}

void pqWindowCube::on_actionMomentOrder10_triggered()
{
    showMomentMap(10);
}

void pqWindowCube::on_actionDraw_PV_line_triggered()
{
    if (!drawing) {
        viewSlice->getViewProxy()->GetRenderWindow()->GetInteractor()->SetInteractorStyle(
                drawPVLineInteractorStyle);
        drawing = true;
        showStatusBarMessage("Press ENTER to confirm your selection, press ESC to abort.");
        auto viewProxy = viewSlice->getProxy();
        vtkSMPropertyHelper(viewProxy, "ShowAnnotation").Set(0);
        viewProxy->UpdateVTKObjects();
    } else
        endDrawLine();
}

void pqWindowCube::on_actionGen_test_PV_slice_triggered()
{
    showPVSlice();
}

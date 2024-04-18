#include "pvdataobject.h"

#include <pqActiveObjects.h>
#include <pqAlwaysConnectedBehavior.h>
#include <pqApplicationCore.h>
#include <pqLoadDataReaction.h>
#include <vtkSMCoreUtilities.h>
#include <vtkSMPropertyHelper.h>
#include <vtkSMPVRepresentationProxy.h>
#include "vtkSMTransferFunctionManager.h"
#include <vtkSMTransferFunctionPresets.h>

PVDataObject::PVDataObject(std::pair<int, int> &startP, std::pair<int, int> &endP, std::pair<int, int> ZSubExtent, QString fileName, pqRenderView* vwImg, QString serv, QObject *parent)
    : QObject{parent}, start(startP), end(endP), ZBound(ZSubExtent), cubeFileName(fileName), viewImage(vwImg), serverUrl(serv)
{

}

PVDataObject::~PVDataObject()
{
    builder->destroy(this->PVSliceImage);
    this->PVSliceImage = NULL;
}

QString PVDataObject::getColourMap() const
{
    return colourMap;
}

vtkSMSourceProxy *PVDataObject::getSourceProxy() const
{
    return this->PVSliceImage->getSourceProxy();
}

int PVDataObject::changeLutScale()
{
    logScale = !logScale;
    if (logScale) {
        if (auto logProperty = lutProxy->GetProperty("UseLogScale"))
        {
            double range[2];
            vtkSMTransferFunctionProxy::GetRange(lutProxy, range);
            vtkSMCoreUtilities::AdjustRangeForLog(range);

            vtkSMTransferFunctionProxy::RescaleTransferFunction(lutProxy, range);
            vtkSMPropertyHelper(logProperty).Set(1);

            lutProxy->UpdateVTKObjects();
            imageProxy->UpdateVTKObjects();
            changeLut(this->getColourMap());
            emit this->render();
            return 1;
        }
        else
        {
            std::cerr << "Error with logscale proxy: not found correctly." << std::endl;
            return 0;
        }
    } else {
        if (auto logProperty = lutProxy->GetProperty("UseLogScale"))
        {
            this->logScale = false;
            vtkSMTransferFunctionProxy::RescaleTransferFunctionToDataRange(lutProxy);
            vtkSMPropertyHelper(logProperty).Set(0);

            lutProxy->UpdateVTKObjects();
            imageProxy->UpdateVTKObjects();
            changeLut(this->getColourMap());
            emit this->render();
            return 1;
        }
        else
        {
            std::cerr << "Error with logscale proxy: not found correctly." << std::endl;
            return 0;
        }
    }
}

int PVDataObject::changeOpacity(int opacity)
{
    if (vtkSMProperty *opacityProperty = imageProxy->GetProperty("Opacity")) {

        vtkSMPropertyHelper(opacityProperty).Set(opacity * 0.01);
        imageProxy->UpdateVTKObjects();

        emit this->render();
        return 1;
    }
    return 0;
}

int PVDataObject::changeLut(const QString &lutName)
{
    if (vtkSMProperty *lutProperty = imageProxy->GetProperty("LookupTable")) {
        auto presets = vtkSMTransferFunctionPresets::GetInstance();
        lutProxy->ApplyPreset(presets->GetFirstPresetWithName(lutName.toStdString().c_str()));
        vtkSMPropertyHelper(lutProperty).Set(lutProxy);
        lutProxy->UpdateVTKObjects();
        vtkSMPVRepresentationProxy::RescaleTransferFunctionToDataRange(imageProxy, false, false);

        vtkSMPVRepresentationProxy::SetScalarBarVisibility(imageProxy, viewImage->getProxy(), true);

        imageProxy->UpdateVTKObjects();
        this->colourMap = lutName;
        emit this->render();
        return 1;
    }
    return 0;
}

void PVDataObject::genPVSlice()
{
//    QThread::sleep(10);
    this->builder = pqApplicationCore::instance()->getObjectBuilder();
    this->server = pqApplicationCore::instance()->getObjectBuilder()->createServer(pqServerResource(serverUrl), 3);
    this->serverProxyManager = server->proxyManager();
    new pqAlwaysConnectedBehavior(this);
    this->PVSliceImage = pqLoadDataReaction::loadData({ this->cubeFileName });
    auto cubeProxy = this->PVSliceImage->getProxy();
    if (auto PVProp = cubeProxy->GetProperty("ReadAsPV")){
        bool pvSlice = true;
        vtkSMPropertyHelper(PVProp).Set(pvSlice);
        cubeProxy->UpdateVTKObjects();
    }
    else{
        std::cerr << "Error when setting ReadAsPV slice to true!" << std::endl;
    }

    if (auto startProp = cubeProxy->GetProperty("StartPoint")){
        int* startVals = new int[2];
        startVals[0] = start.first;
        startVals[1] = start.second;
        vtkSMPropertyHelper(startProp).Set(startVals, 2);
        cubeProxy->UpdateVTKObjects();
        delete[] startVals;
    }
    else{
        std::cerr << "Error when setting filter properties!" << std::endl;
    }

    if (auto endProp = cubeProxy->GetProperty("EndPoint")){
        int* endVals = new int[2];
        endVals[0] = end.first;
        endVals[1] = end.second;
        vtkSMPropertyHelper(endProp).Set(endVals, 2);
        cubeProxy->UpdateVTKObjects();
        delete[] endVals;
    }
    else{
        std::cerr << "Error when setting filter properties!" << std::endl;
    }

    if (auto ZProp = cubeProxy->GetProperty("ZBounds")){
        int* ZVals = new int[2];
        ZVals[0] = ZBound.first;
        ZVals[1] = ZBound.second;
        vtkSMPropertyHelper(ZProp).Set(ZVals, 2);
        cubeProxy->UpdateVTKObjects();
        delete[] ZVals;
    }
    else{
        std::cerr << "Error when setting filter properties!" << std::endl;
    }

    auto botLeft = std::make_pair(std::min(start.first, this->end.first), std::min(start.second, this->end.second));
    auto topRight = std::make_pair(std::max(start.first, this->end.first), std::max(start.second, this->end.second));
    int subset[6] = {botLeft.first, topRight.first, botLeft.second, topRight.second, ZBound.first, ZBound.second};
    vtkSMPropertyHelper(cubeProxy, "ReadSubExtent").Set(1);
    vtkSMPropertyHelper(cubeProxy, "SubExtent").Set(subset, 6);

    cubeProxy->UpdateVTKObjects();

    this->imageProxy = this->builder->createDataRepresentation(this->PVSliceImage->getOutputPort(0), viewImage)->getProxy();
    vtkSMPropertyHelper(cubeProxy, "Representation").Set("Slice");
    vtkSMPVRepresentationProxy::SetScalarColoring(cubeProxy, "FITSImage", vtkDataObject::POINT);
    this->imageProxy->UpdateVTKObjects();

    vtkNew<vtkSMTransferFunctionManager> mgr;
    lutProxy = vtkSMTransferFunctionProxy::SafeDownCast(mgr->GetColorTransferFunction("PVSliceTransferFunction", this->imageProxy->GetSessionProxyManager()));
    changeLut(this->getColourMap());
    this->logScale = false;

    emit pvGenComplete();
    emit this->renderReset();
}

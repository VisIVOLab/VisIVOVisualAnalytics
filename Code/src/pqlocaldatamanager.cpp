#include "pqlocaldatamanager.h"

#include <pqActiveObjects.h>
#include <pqAlwaysConnectedBehavior.h>
#include <pqApplicationCore.h>
#include <pqLoadDataReaction.h>
#include "vtkSMPropertyHelper.h"
#include "vtkSMProxy.h"

#include <iostream>

pqLocalDataManager::pqLocalDataManager(const QString file, const CubeSubset &fileSubset)
{
    this->builder= pqApplicationCore::instance()->getObjectBuilder();
    this->server = pqActiveObjects::instance().activeServer();
    new pqAlwaysConnectedBehavior(this);

    auto imageSource = pqLoadDataReaction::loadData({ file });
    setSubsetProperties(imageSource, fileSubset);
}

pqLocalDataManager::~pqLocalDataManager()
{
    builder->destroySources(server);
}

int pqLocalDataManager::setSubsetProperties(pqPipelineSource* imageSource, const CubeSubset& subset)
{
    try
    {
        auto sourceProxy = imageSource->getProxy();
        vtkSMPropertyHelper(sourceProxy, "ReadSubExtent").Set(subset.ReadSubExtent);
        vtkSMPropertyHelper(sourceProxy, "SubExtent").Set(subset.SubExtent, 6);
        vtkSMPropertyHelper(sourceProxy, "AutoScale").Set(subset.AutoScale);
        vtkSMPropertyHelper(sourceProxy, "CubeMaxSize").Set(subset.CubeMaxSize);
        if (!subset.AutoScale)
            vtkSMPropertyHelper(sourceProxy, "ScaleFactor").Set(subset.ScaleFactor);
        sourceProxy->UpdateVTKObjects();
        return 1;
    }
    catch (std::exception& e)
    {
        std::cerr << "Error when setting subset properties of stack image! Error: " << e.what() << std::endl;
        return 0;
    }
    std::cerr << "StackImage not initialised, returning default value." << std::endl;
    return 0;
}

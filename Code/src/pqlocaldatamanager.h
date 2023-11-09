#ifndef PQLOCALDATAMANAGER_H
#define PQLOCALDATAMANAGER_H

#include "src/subsetselectordialog.h"

#include "pqObjectBuilder.h"
#include "pqServer.h"

#include <QString>

class pqLocalDataManager : QObject
{
    Q_OBJECT
public:
    pqLocalDataManager(const QString file, const CubeSubset &fileSubset);
    ~pqLocalDataManager();

private:
    int setSubsetProperties(pqPipelineSource *imageSource, const CubeSubset &subset);

    pqServer *server;
    pqObjectBuilder *builder;
    vtkSMProxy *imageProxy;
};

#endif // PQLOCALDATAMANAGER_H

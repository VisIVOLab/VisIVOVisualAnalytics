#ifndef PVDATAOBJECT_H
#define PVDATAOBJECT_H

#include "pqObjectBuilder.h"
#include "pqRenderView.h"
#include "pqServer.h"
#include "vtkSMTransferFunctionProxy.h"
#include <QObject>

class PVDataObject : public QObject
{
    Q_OBJECT
public:
    explicit PVDataObject(std::pair<int, int> &startP, std::pair<int, int> &endP, std::pair<int, int> ZSubExtent, QString fileName, pqRenderView *vwImg, QString serv, QObject *parent = nullptr);
    ~PVDataObject();

    QString getColourMap() const;
    vtkSMSourceProxy* getSourceProxy() const;

public slots:
    int changeLutScale();
    int changeOpacity(int opacity);
    int changeLut(const QString &lutName);

    void genPVSlice();

signals:
    void pvGenComplete();
    void render();
    void renderReset();

private:
    QString serverUrl;
    pqServer *server;
    vtkSMSessionProxyManager *serverProxyManager;
    pqObjectBuilder *builder;
    QString cubeFileName;
    pqPipelineSource* PVSliceImage;
    vtkSMProxy* imageProxy;
    vtkSMTransferFunctionProxy* lutProxy;
    pqRenderView* viewImage;

    std::pair<int, int> start, end, ZBound;

    QString colourMap = "Grayscale";
    bool logScale = false;
};

#endif // PVDATAOBJECT_H

#ifndef PQWINDOWCUBE_H
#define PQWINDOWCUBE_H

#include "subsetselectordialog.h"

#include <QMainWindow>
#include <QMap>
#include <QPointer>

class pqPipelineSource;
class pqServer;
class vtkSMSessionProxyManager;
class pqObjectBuilder;
class pqRenderView;
class vtkSMProxy;
class vtkSMTransferFunctionProxy;

namespace Ui {
class pqWindowCube;
}

class pqWindowCube : public QMainWindow
{
    Q_OBJECT

    using FitsHeaderMap = QMap<QString, QString>;

public:
    explicit pqWindowCube(const QString &filepath, const CubeSubset &cubeSubset = CubeSubset());
    ~pqWindowCube() override;

private slots:
    void on_sliceSlider_actionTriggered(int action);
    void on_sliceSlider_sliderReleased();
    void on_sliceSlider_valueChanged();
    void on_sliceSpinBox_editingFinished();
    void on_sliceSpinBox_valueChanged(int arg1);
    void on_actionFront_triggered();
    void on_actionBack_triggered();
    void on_actionTop_triggered();
    void on_actionRight_triggered();
    void on_actionBottom_triggered();
    void on_actionLeft_triggered();

    void on_thresholdText_editingFinished();
    void on_thresholdSlider_sliderReleased();

    void on_contourCheckBox_toggled(bool checked);
    void on_levelText_editingFinished();
    void on_lowerBoundText_editingFinished();
    void on_upperBoundText_editingFinished();

    void on_action0_triggered();
    void on_action25_triggered();
    void on_action50_triggered();
    void on_action75_triggered();
    void on_action100_triggered();

    void generateVolumeRendering();

    void on_actionView_Slice_triggered();
    void on_actionView_Moment_Map_triggered();

    void on_actionMomentOrder0_triggered();
    void on_actionMomentOrder1_triggered();
    void on_actionMomentOrder2_triggered();
    void on_actionMomentOrder6_triggered();
    void on_actionMomentOrder8_triggered();
    void on_actionMomentOrder10_triggered();

private:
    Ui::pqWindowCube *ui;

    QString FitsFileName;
    pqPipelineSource *CubeSource;
    pqPipelineSource *SliceSource;
    pqPipelineSource *MomentMapSource;

    CubeSubset cubeSubset;

    pqServer *server;
    vtkSMSessionProxyManager *serverProxyManager;
    pqObjectBuilder *builder;
    pqRenderView *viewCube;
    pqRenderView *viewSlice;
    pqRenderView *viewMomentMap;
    vtkSMProxy *cubeSliceProxy;
    vtkSMProxy *sliceProxy;
    vtkSMProxy *momentProxy;
    vtkSMTransferFunctionProxy *lutProxy;
    vtkSMTransferFunctionProxy *momentLUTProxy;
    pqPipelineSource *extractGrid;
    QPointer<pqPipelineSource> contourFilter2D;
    pqPipelineSource *contourFilter;
    vtkSMProxy *contourProxy;

    QString fitsHeaderPath;
    FitsHeaderMap fitsHeader;
    double bounds[6];
    double dataMin;
    double dataMax;
    double rms;
    double lowerBound;
    double upperBound;
    int currentSlice;
    bool logScaleActive;
    QString currentColorMap;

    void showStatusBarMessage(const std::string &msg);

    void setSubsetProperties(const CubeSubset &subset);

    void readInfoFromSource();
    void readHeaderFromSource();
    QString createFitsHeaderFile(const FitsHeaderMap &fitsHeader);
    double getRMSFromHeader(const FitsHeaderMap &fitsHeader);

    void createViews();
    void showOutline();
    void showLegendScaleActors();
    void showSlice();
    void loadMomentMap();
    void showMomentMap(int order = 0);
    void changeColorMap(const QString &name);
    void setLogScale(bool logScale);

    void setSliceDatacube(int value);
    void updateVelocityText();
    void updateMinMax(bool moment);

    void setThreshold(double threshold);
    void setVolumeRenderingOpacity(double opacity);

    void showContours();
    void removeContours();

    void setMomentMapVisible(bool val = false);

    bool loadChange = false;
    int momentOrder = 0;
};

#endif // PQWINDOWCUBE_H

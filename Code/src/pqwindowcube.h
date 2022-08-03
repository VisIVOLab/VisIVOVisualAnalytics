#ifndef PQWINDOWCUBE_H
#define PQWINDOWCUBE_H

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
    explicit pqWindowCube(pqPipelineSource *fitsSource, const std::string &fn);
    ~pqWindowCube() override;

private slots:
    void on_sliceSlider_sliderReleased();
    void on_sliceSpinBox_editingFinished();

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

private:
    Ui::pqWindowCube *ui;

    pqPipelineSource *FitsSource;
    std::string FitsFileName;

    pqServer *server;
    vtkSMSessionProxyManager *serverProxyManager;
    pqObjectBuilder *builder;
    pqRenderView *viewCube;
    pqRenderView *viewSlice;
    vtkSMProxy *cubeSliceProxy;
    vtkSMProxy *sliceProxy;
    vtkSMTransferFunctionProxy *lutProxy;
    pqPipelineSource *extractGrid;
    QPointer<pqPipelineSource> contourFilter2D;
    pqPipelineSource *contourFilter;
    vtkSMProxy *contourProxy;

    QString fitsHeaderPath;
    FitsHeaderMap fitsHeader;
    double bounds[6] {};
    double dataMin {};
    double dataMax {};
    double rms;
    double lowerBound;
    double upperBound;
    int currentSlice;

    void readInfoFromSource();
    void readHeaderFromSource();
    QString createFitsHeaderFile(const FitsHeaderMap &fitsHeader);
    double getRMSFromHeader(const FitsHeaderMap &fitsHeader);

    void createViews();
    void showOutline();
    void showLegendScaleActors();
    void showSlice();
    void changeColorMap(const QString &name);

    QString velocityUnit;

    void showStatusBarMessage(const std::string &msg);

    void setSliceDatacube(int value);
    void updateVelocityText();

    void setThreshold(double threshold);
    void setVolumeRenderingOpacity(double opacity);

    void showContours();
    void removeContours();

    void resetCamera();
    void setCameraAzimuth(double az);
    void setCameraElevation(double el);
};

#endif // PQWINDOWCUBE_H

#ifndef PQWINDOWIMAGE_H
#define PQWINDOWIMAGE_H

#include "downloadmanager.h"
#include "subsetselectordialog.h"
#include "vlvastackimage.h"
#include "xmlparser.h"

#include <QTableWidgetItem>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMap>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QPropertyAnimation>

class pqPipelineSource;
class pqServer;
class vtkSMSessionProxyManager;
class pqObjectBuilder;
class pqRenderView;
class vtkSMProxy;
class vtkSMTransferFunctionProxy;

namespace Ui {
class pqWindowImage;
}

class pqWindowImage : public QMainWindow
{
    Q_OBJECT

    using FitsHeaderMap = QMap<QString, QString>;

public:
    explicit pqWindowImage(const QString &filepath, const CubeSubset &cubeSubset = CubeSubset());
    ~pqWindowImage();

public slots:
    void setVLKBElementsList(QList<QMap<QString, QString>> elementsOnDb);

private slots:
    int addImageToStack(QString file, const CubeSubset& subset);

    void tableCheckboxClicked(int cb, bool status = false);

    void movedLayersRow(const QModelIndex &sourceParent, int sourceStart, int sourceEnd,
                        const QModelIndex &destinationParent, int destinationRow);

    void on_cmbxLUTSelect_currentIndexChanged(int index);

    void on_linearRadioButton_toggled(bool checked);

    void on_opacitySlider_actionTriggered(int action);

    void on_opacitySlider_valueChanged(int value);

    void on_opacitySlider_sliderReleased();

    void on_btnAddImageToStack_clicked();

    void on_btnRemoveImageFromStack_clicked();

    void on_lstImageList_itemClicked(QListWidgetItem *item);

    void on_lstImageList_itemChanged(QListWidgetItem *item);

private:
    /**
     * @brief The removeErrorCode enum
     * Used for removing images from stack for reasons to make sure that program doesn't
     * try to destroy unitialised or nonexisting proxies and sources.
     */
    enum removeErrorCode{INIT_ERROR, NO_ERROR, IS_CUBE_ERROR};

    Ui::pqWindowImage *ui;

    std::vector<vlvaStackImage*> images;

    pqServer *server;
    vtkSMSessionProxyManager *serverProxyManager;
    pqObjectBuilder *builder;
    pqRenderView *viewImage;
    vtkSMProxy *imageProxy;
    vtkSMTransferFunctionProxy *lutProxy;

    xmlparser* parser;

    std::pair<double, double> refImageBottomLeftCornerCoords;
    double refImagePixScaling;
    std::string refImageFits;
    int imgCounter;

    bool clmInit;

    bool loadOpacityChange;

    bool logScaleActive;

    int activeIndex;

    void changeColorMap(const QString &name);
    void showStatusBarMessage(const std::string &msg);

    void updateUI();

    void setImageListCheckbox(int row, Qt::CheckState checked);

    QString createFitsHeaderFile(const FitsHeaderMap &fitsHeader);
    void createView();
    void readInfoFromSource();
    void readHeaderFromSource();
    void setLogScale(bool logScale);
    void setOpacity(float value);
    int removeImageFromStack(const int index, const removeErrorCode remErrCode = removeErrorCode::NO_ERROR);
    void checkVLKB(vlvaStackImage *stackImage);

    int positionImage(vlvaStackImage* stackImage, bool setBasePos = false);

    static constexpr float defaultMultiOpacity = 0.5;
    static constexpr bool logScaleDefault = false;
};

#endif // PQWINDOWIMAGE_H

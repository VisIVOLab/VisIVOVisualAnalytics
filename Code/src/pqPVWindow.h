#ifndef PQPVWINDOW_H
#define PQPVWINDOW_H

#include "pqObjectBuilder.h"
#include "pqRenderView.h"
#include "pqServer.h"
#include "qthread.h"
#include "pvdataobject.h"

#include <QMainWindow>

namespace Ui {
class pqPVWindow;
}

class pqPVWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit pqPVWindow(const QString serverUrl, QString cubeFileName, std::pair<int, int>& start, std::pair<int, int>& end, std::pair<int, int>& zSubExtent, QWidget *parent = nullptr);
    ~pqPVWindow();

    QString getColourMap() const;

    void closeEvent(QCloseEvent *event) override;
    void genPVSlice();

signals:
    void closed();
    void pvGenComplete();

    void dataObjChangeLutScale();
    void dataObjChangeOpacity(int opacity);
    void dataObjChangeLut(const QString &lutName);
    void genPV(pqRenderView* viewImage);

private slots:
    void changeLutScale();
    void changeOpacity(int opacity);
    void changeLut(const QString &lutName);

    void on_actionSave_as_PNG_triggered();

    void on_actionSave_as_FITS_triggered();

    void dataObjPVGenComplete();

    void render();
    void renderReset();

private:
    Ui::pqPVWindow *ui;

    QThread* thread;
    PVDataObject* dataObject;

    pqServer* server;
    pqObjectBuilder* builder;
    pqRenderView* viewImage;

    QString colourMap = "Grayscale";
    bool logScale = false;

    void applyFilter();
    int saveAsPNG();
    int saveAsFITS();
};

#endif // PQPVWINDOW_H

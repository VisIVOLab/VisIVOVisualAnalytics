#ifndef LUTCustomizerDialog_h
#define LUTCustomizerDialog_h

#include <vtkSmartPointer.h>

#include <QDialog>

#include "LutScale.h"

class QAbstractButton;
class QCPAbstractItem;
class QCPItemStraightLine;
class QDoubleSpinBox;
class QLabel;
class vtkImageData;
class vtkLookupTable;

QT_BEGIN_NAMESPACE
namespace Ui {
class LUTCustomizerDialog;
}
QT_END_NAMESPACE

class LUTCustomizerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LUTCustomizerDialog(QWidget *parent = nullptr);
    ~LUTCustomizerDialog() override;

    void init(vtkImageData *dataset, vtkLookupTable *lut);
    // Variant that seeds the colour-scale mode + gamma from the owning layer so
    // the dialog reopens showing the stretch currently applied.
    void init(vtkImageData *dataset, vtkLookupTable *lut, LutScale::Mode mode, double gamma);

    LutScale::Mode scaleMode() const { return m_scaleMode; }
    double scaleGamma() const { return m_gamma; }

signals:
    void lutUpdated();

private slots:
    void resetMin();
    void resetMax();

    void selectReferenceLine(QCPAbstractItem *item);
    void moveReferenceLine(QMouseEvent *event);
    void deselectReferenceLine();
    void updateReferenceLines();

    void updateLut();
    void buttonClicked(QAbstractButton *btn);
    void changeAxisScaling(bool logarithmic);
    void changeScaleMode(int index);

private:
    Ui::LUTCustomizerDialog *ui;
    QCPItemStraightLine *refLineMin;
    QCPItemStraightLine *refLineMax;
    QCPItemStraightLine *selectedLine;
    QDoubleSpinBox *gammaSpin = nullptr;
    QLabel *gammaLabel = nullptr;

    vtkSmartPointer<vtkImageData> dataset;
    vtkSmartPointer<vtkLookupTable> lut;
    double datasetRange[2];
    LutScale::Mode m_scaleMode = LutScale::Mode::Linear;
    double m_gamma = 2.0;

    void setupPlot();
    void plotHistogram();
};

#endif

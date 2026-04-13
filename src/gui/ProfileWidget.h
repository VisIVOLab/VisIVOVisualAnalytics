#ifndef ProfileWidget_h
#define ProfileWidget_h

#include "AstroUtils.h"

#include <vtkSmartPointer.h>

#include <QWidget>
#include <QVector>

#include <memory>

class QCPItemLine;
class QPushButton;
class vtkImageData;
class vtkInteractorStyleProfile;

QT_BEGIN_NAMESPACE
namespace Ui {
class ProfileWidget;
}
QT_END_NAMESPACE

class ProfileWidget : public QWidget
{
    Q_OBJECT

public:
    enum class UsageMode
    {
        ProbeLive,
        RegionStatic,
    };

    explicit ProfileWidget(vtkInteractorStyleProfile *style, vtkImageData *dataset,
                           const std::string &filepath, QWidget *parent = nullptr);
    explicit ProfileWidget(QWidget *parent = nullptr);
    ~ProfileWidget();

    void setUsageMode(UsageMode mode, const QString &windowTitle = QString());
    void setupImagePlots();
    void setupImagePlots(const QString &xLabel, const QString &yLabel);
    void setupSpectrumPlot();
    void setupSpectrumPlot(const QString &xLabel, const QString &yLabel);
    void updateSpectrumPlot(const QVector<double> &key, const QVector<double> &values,
                            const QString &title, bool live);
    void updateSpectrumPlotSeries(const QVector<double> &key,
                                  const QVector<double> &primaryValues,
                                  const QString &primaryLabel,
                                  const QVector<double> &secondaryValues,
                                  const QString &secondaryLabel,
                                  const QString &title, bool live);
    void updateImageProfiles(const QVector<double> &keyX, const QVector<double> &valuesX,
                             const QVector<double> &keyY, const QVector<double> &valuesY,
                             double probeX, double probeY, bool live);

    /// Apply dark (true) or light (false) plot theme to all active plots.
    void applyPlotTheme(bool dark);

private slots:
    void setLiveMode(bool live);
    void savePlot1Png();
    void savePlot2Png();

private:
    Ui::ProfileWidget *ui;
    vtkSmartPointer<vtkImageData> dataset;
    std::unique_ptr<AstroUtils> astro;

    float *ScalarPointer;
    int Dimensions[3];
    vtkIdType Increments[3];

    vtkSmartPointer<vtkInteractorStyleProfile> interactor;
    QCPItemLine *refLineX;
    QCPItemLine *refLineY;
    UsageMode usageMode{ UsageMode::ProbeLive };

    QPushButton *m_btnSavePlot1 { nullptr };
    QPushButton *m_btnSavePlot2 { nullptr };
    bool m_darkTheme{ true };

    void plotProfile(double x, double y, bool live);
    void plotSpectrum(double x, double y, bool live);
    void applyUsageModeUi(const QString &windowTitle = QString());
};

#endif

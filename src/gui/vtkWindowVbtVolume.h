#ifndef vtkWindowVbtVolume_h
#define vtkWindowVbtVolume_h

#include "app/BackendClient.h"

#include <vtkNew.h>
#include <vtkColorTransferFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPiecewiseFunction.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <QMainWindow>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>

class QAction;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QToolButton;
class QVTKOpenGLNativeWidget;
class vtkImageData;

class vtkWindowVbtVolume : public QMainWindow
{
    Q_OBJECT

public:
    explicit vtkWindowVbtVolume(BackendVbtOpenResult openResult,
                                BackendVbtSubsetResult initialSubset,
                                const QString &backendUrl,
                                const QString &backendToken,
                                const QString &sessionId,
                                QWidget *parent = nullptr);
    ~vtkWindowVbtVolume() override = default;

private:
    void setupUi();
    void setupRenderer();
    void rebuildVolumeData();
    void applySubsetResult(const BackendVbtSubsetResult &subset);
    void updateTransferFunctions();
    void updateSummary();
    void updateBoundsContext();
    void updateScalarBar();
    void resetView();
    void applyKnownGoodVisibleFallback(double minValue, double maxValue, const double bounds[6]);
    static double percentileFromSortedSample(const std::vector<double> &values, double fraction);
    void setColorRange(double minValue, double maxValue);
    double positiveFloor() const;

    BackendVbtOpenResult openResult;
    const QString backendUrl;
    const QString backendToken;
    const QString sessionId;

    QPointer<QVTKOpenGLNativeWidget> vtkWidget;
    QPointer<QComboBox> comboScalarField;
    QPointer<QComboBox> comboBlendMode;  // Composite / MIP / MinIP
    QPointer<QComboBox> comboScaleMode;
    QPointer<QSlider> sliderOpacity;
    QPointer<QDoubleSpinBox> spinRangeMin;
    QPointer<QDoubleSpinBox> spinRangeMax;
    QPointer<QPushButton> buttonAutoscale;
    QPointer<QLabel> summaryLabel;
    QPointer<QListWidget> fieldList;
    QPointer<QLabel> metadataLabel;
    bool updatingRangeControls{ false };

    // ── Sidebar ──────────────────────────────────────────────────────────────
    bool sidebarExpanded{ true };
    int pendingPageIndex{ -1 };
    QPointer<QParallelAnimationGroup> sidebarAnimGroup;
    QPointer<QAction> actionShowControlsSidebar;
    QPointer<QWidget> sidebarContainer;
    QPointer<QWidget> railWidget;
    QPointer<QScrollArea> sidebarScrollArea;
    QPointer<QStackedWidget> sidebarStack;
    QPointer<QToolButton> btnToggleSidebar;
    QPointer<QToolButton> railBtnVolume;
    QPointer<QToolButton> railBtnMetadata;
    void setupSidebarUi(QWidget *centralWidget);
    void setupSidebarRail();
    void setupRailIcons();
    void setupSidebarPages();
    void setSidebarExpanded(bool expanded, bool animated = true);
    void toggleSidebar();
    void switchToPage(int index);
    void setActiveRailButton(QToolButton *btn);
    double dataScalarMin{ 0.0 };
    double dataScalarMax{ 1.0 };
    double activeColorMin{ 0.0 };
    double activeColorMax{ 1.0 };
    double cachedOpacityLow{ 0.0 };
    double cachedOpacityMidLow{ 0.0 };
    double cachedOpacityMidHigh{ 0.0 };
    double cachedOpacityHigh{ 1.0 };
    double cachedPositiveFloor{ 1e-12 };
    std::vector<double> cachedSampledValues;
    std::vector<double> cachedPositiveSampledValues;

    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkSmartVolumeMapper> volumeMapper;
    vtkNew<vtkVolumeProperty> volumeProperty;
    vtkNew<vtkVolume> volumeActor;
    vtkNew<vtkColorTransferFunction> colorTransfer;
    vtkNew<vtkPiecewiseFunction> opacityTransfer;
    vtkNew<vtkLookupTable> scalarBarLut;
    vtkNew<vtkCubeAxesActor> boxActor;
    vtkNew<vtkScalarBarActor> scalarBar;
    vtkNew<vtkOrientationMarkerWidget> axesWidget;
    vtkSmartPointer<vtkImageData> currentImageData;
};

#endif

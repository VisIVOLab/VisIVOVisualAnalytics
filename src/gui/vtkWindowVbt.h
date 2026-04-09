#ifndef vtkWindowVbt_h
#define vtkWindowVbt_h

#include "app/BackendClient.h"
#include "vtk/ColorMaps.h"

#include <vtkNew.h>
#include <vtkActor.h>
#include <vtkCubeAxesActor.h>
#include <vtkDoubleArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkLookupTable.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPointGaussianMapper.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>

#include <QFutureWatcher>
#include <QMainWindow>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>

class QAction;
class QCheckBox;
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

struct VbtFilterTaskResult
{
    bool valid{ false };
    QString error;
    BackendVbtSubsetResult subsetResult;
    QString statusMsg;
};

class vtkWindowVbt : public QMainWindow
{
    Q_OBJECT

public:
    explicit vtkWindowVbt(BackendVbtOpenResult openResult, BackendVbtSubsetResult subsetResult,
                          const QString &backendUrl, const QString &backendToken,
                          const QString &sessionId, QWidget *parent = nullptr);
    ~vtkWindowVbt() override = default;

private:
    void setupUi();
    void setupRenderer();
    void buildPointCloud();
    void applyFilter();
    void updateColorMapping();
    void updateScalarBar();
    void updateSummary();
    void updateBoundsContext();
    void applyBackground();
    void resetView();
    void updateRenderMode();
    QString activeColorFieldName() const;
    QString activeColormapName() const;
    void setScalarRange(double minValue, double maxValue);

    BackendVbtOpenResult openResult;
    BackendVbtSubsetResult subsetResult;
    QString xFieldName;
    QString yFieldName;
    QString zFieldName;
    const QString backendUrl;
    const QString backendToken;
    const QString sessionId;

    QPointer<QVTKOpenGLNativeWidget> vtkWidget;
    QPointer<QComboBox> comboRenderMode;
    QPointer<QComboBox> comboColorField;
    QPointer<QComboBox> comboColormap;
    QPointer<QComboBox> comboBackground;
    QPointer<QSlider> sliderPointSize;
    QPointer<QSlider> sliderRayIntensity;
    QPointer<QCheckBox> checkShowBox;
    QPointer<QCheckBox> checkShowLut;
    QPointer<QCheckBox> checkShowOrientation;
    QPointer<QDoubleSpinBox> spinRangeMin;
    QPointer<QDoubleSpinBox> spinRangeMax;
    QPointer<QPushButton> buttonAutoscale;
    QPointer<QPushButton> buttonResetView;
    QPointer<QLabel> summaryLabel;
    // Multi-filter UI
    QPointer<QListWidget> filterList;
    QPointer<QPushButton> btnFilterAdd;
    QPointer<QPushButton> btnFilterRemove;
    QPointer<QPushButton> btnFilterClearAll;
    QPointer<QPushButton> btnFilterApply;
    QPointer<QLabel> filterStatusLabel;
    std::vector<BackendTabularFilter> activeFilters;
    bool filterBusy{ false };
    QFutureWatcher<VbtFilterTaskResult> filterWatcher;
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
    QPointer<QToolButton> railBtnDisplay;
    QPointer<QToolButton> railBtnFilters;
    QPointer<QToolButton> railBtnMetadata;
    void setupSidebarUi(QWidget *centralWidget);
    void setupSidebarRail();
    void setupRailIcons();
    void setupSidebarPages();
    void setSidebarExpanded(bool expanded, bool animated = true);
    void toggleSidebar();
    void switchToPage(int index);
    void setActiveRailButton(QToolButton *btn);
    double activeScalarMin{ 0.0 };
    double activeScalarMax{ 1.0 };
    double dataScalarMin{ 0.0 };
    double dataScalarMax{ 1.0 };

    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkPoints> points;
    vtkNew<vtkPolyData> polyData;
    vtkNew<vtkDoubleArray> pointScalars;
    vtkNew<vtkLookupTable> scalarLut;
    vtkNew<vtkPolyDataMapper> mapper;
    vtkNew<vtkPointGaussianMapper> gaussianMapper;
    vtkNew<vtkPiecewiseFunction> gaussianScaleFunction;
    vtkNew<vtkPiecewiseFunction> gaussianOpacityFunction;
    vtkNew<vtkActor> actor;
    vtkNew<vtkCubeAxesActor> boxActor;
    vtkNew<vtkScalarBarActor> scalarBar;
    vtkNew<vtkOrientationMarkerWidget> axesWidget;
};

#endif

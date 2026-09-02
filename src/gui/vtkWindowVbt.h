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
#include <vtkTextActor.h>

#include <QFutureWatcher>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QPointer>

class AssistantPanel;
class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QListWidget;
class QPushButton;
class SidebarPanel;
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
    int totalRows{ 0 }; // backend-reported total (before limit/offset)
};

class vtkWindowVbt : public QMainWindow
{
    Q_OBJECT

public:
    explicit vtkWindowVbt(BackendVbtOpenResult openResult, BackendVbtSubsetResult subsetResult,
                          const QString &backendUrl, const QString &backendToken,
                          const QString &sessionId, const QString &headerPath = {},
                          QWidget *parent = nullptr);
    ~vtkWindowVbt() override = default;

signals:
    /// Emitted when a session-expiry error is detected and the user requests
    /// that the dataset be reopened.  Connect to MainWindow::doOpenVbtPath.
    void reopenRequested(const QString &headerPath);

private:
    void setupUi();
    void setupRenderer();
    void buildPointCloud();
    void rebuildCoordinates();
    void applyFilter();
    void updateColorMapping();
    void updateScalarBar();
    void updateSummary();
    void updateBoundsContext();
    void applyBackground();
    void applyBoxActorColors(bool darkBackground);
    void resetView();
    void updateRenderMode();

    /// Apply scientific-copilot visualisation directives to this VBT view.
    /// A point/volume table supports colormap and camera control: set_colormap
    /// (recolour by the named map) and reset_camera. Other directives (threshold,
    /// render/blend mode — cube-only) are ignored. State is snapshotted first so
    /// undoCopilotActions() can revert it.
    void applyCopilotActions(const QList<QJsonObject> &actions);
    void undoCopilotActions();

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
    QString sessionId;
    const QString m_headerPath;   ///< original .head file path; used for session-expiry reopen

    QPointer<QVTKOpenGLNativeWidget> vtkWidget;
    QPointer<QComboBox> comboRenderMode;
    QPointer<QComboBox> comboXField;
    QPointer<QComboBox> comboYField;
    QPointer<QComboBox> comboZField;
    QPointer<QComboBox> comboColorField;
    QPointer<QComboBox> comboColormap;
    QPointer<QComboBox> comboBackground;
    QPointer<QSlider> sliderPointSize;
    QPointer<QSlider> sliderRayIntensity;
    QPointer<QLabel> m_labelRayIntensity;
    QPointer<QCheckBox> checkShowBox;
    QPointer<QCheckBox> checkShowLut;
    QPointer<QCheckBox> checkShowOrientation;
    QPointer<QDoubleSpinBox> spinRangeMin;
    QPointer<QDoubleSpinBox> spinRangeMax;
    QPointer<QPushButton> buttonAutoscale;
    QPointer<QPushButton> buttonResetView;
    QPointer<QLabel> summaryLabel;
    // Info page dynamic labels (updated by updateSummary())
    QPointer<QLabel> m_infoModeVal;
    QPointer<QLabel> m_infoColorVal;
    QPointer<QLabel> m_infoXVal;
    QPointer<QLabel> m_infoYVal;
    QPointer<QLabel> m_infoZVal;
    QPointer<QLabel> m_infoScalarVal;
    QPointer<QLabel> m_infoDataMinVal;
    QPointer<QLabel> m_infoDataMaxVal;
    QPointer<QLabel> m_infoRangeMinVal;
    QPointer<QLabel> m_infoRangeMaxVal;
    QPointer<QLabel> m_infoMeanVal;
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
    // ── Pagination ────────────────────────────────────────────────────────────
    int m_currentOffset{ 0 };
    int m_pageSize{ 50000 };
    int m_totalCount{ -1 };
    bool m_loadingMore{ false };
    QPointer<QPushButton> btnLoadMore;
    QFutureWatcher<VbtFilterTaskResult> m_loadMoreWatcher;
    void loadMoreEntries();
    // ── Session-expiry auto-recovery ──────────────────────────────────────────
    QFutureWatcher<BackendVbtOpenResult> m_reopenWatcher;
    void autoReopenThenLoadMore();
    QPointer<QListWidget> fieldList;
    QPointer<QLabel> metadataLabel;
    bool updatingRangeControls{ false };
    QString m_lastColorField;

    // ── Sidebar ──────────────────────────────────────────────────────────────
    SidebarPanel *m_sidebar{ nullptr };
    QPointer<QAction> actionShowControlsSidebar;
    void setupSidebarUi();

    // ── Scientific copilot ────────────────────────────────────────────────────
    AssistantPanel *m_copilot{ nullptr };
    /// State captured before the copilot's last change, so Undo can restore it.
    struct CopilotUndo
    {
        bool cameraCaptured{ false };
        double position[3]{ 0, 0, 0 };
        double focalPoint[3]{ 0, 0, 0 };
        double viewUp[3]{ 0, 0, 1 };
        double parallelScale{ 1.0 };
        bool colormapCaptured{ false };
        QString colormapName;
        bool colorFieldCaptured{ false };
        int colorFieldIndex{ -1 };   // restore by index (a field may be named like the sentinel)
    } m_copilotUndo;
    double activeScalarMin{ 0.0 };
    double activeScalarMax{ 1.0 };
    double dataScalarMin{ 0.0 };
    double dataScalarMax{ 1.0 };
    double dataScalarMean{ 0.0 };

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
    vtkNew<vtkTextActor> m_loadingActor;
};

#endif

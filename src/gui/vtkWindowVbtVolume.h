#ifndef vtkWindowVbtVolume_h
#define vtkWindowVbtVolume_h

#include "app/BackendClient.h"
#include "LutScale.h"

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
#include <vtkTextActor.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QFormLayout>
#include <QPointer>

class AssistantPanel;
class QAction;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QVTKOpenGLNativeWidget;
class vtkImageData;

class WorkspaceChrome;

class vtkWindowVbtVolume : public QMainWindow
{
    Q_OBJECT

public:
    explicit vtkWindowVbtVolume(BackendVbtOpenResult openResult,
                                BackendVbtSubsetResult initialSubset,
                                const QString &backendUrl,
                                const QString &backendToken,
                                const QString &sessionId,
                                const QString &headerPath = {},
                                QWidget *parent = nullptr);
    ~vtkWindowVbtVolume() override = default;

public:
    /// Apply scientific-copilot visualisation directives to this volume view:
    /// colour scale (linear/log/…), blend mode (composite/MIP/MinIP), colour
    /// field, and camera reset. State is snapshotted per batch so Undo reverts
    /// exactly the last change.
    void applyCopilotActions(const QList<QJsonObject> &actions);
    void undoCopilotActions();

private:
    void setupUi();
    void setupRenderer();
    /// The command bar and status rail the image and cube viewers have. Built
    /// here too: a VBT volume is a dataset on a backend like any other, and this
    /// window said nothing about which file it was showing or whether that
    /// backend was still answering.
    /// The colour range "auto" should mean: the 2nd–99.9th percentile of the
    /// sampled voxels, which is what the viewer picks when the field is loaded.
    void applyAutoColorRange();
    void setupViewerChrome();
    /// Session Data on the left, Inspector on the right — as in every viewer.
    void setupWorkspaceDocks();
    /// What the kind tag opens — the VBT's own header: fields, type, geometry.
    void showVbtHeaderDialog();
    void rebuildVolumeData();
    void applySubsetResult(const BackendVbtSubsetResult &subset);
    void updateTransferFunctions();
    void updateSummary();
    void updateBoundsContext();
    void updateScalarBar();
    void resetView();
    void applyBackground();
    void applyBoxActorColors(bool darkBackground);
    void applyKnownGoodVisibleFallback(double minValue, double maxValue, const double bounds[6]);
    static double percentileFromSortedSample(const std::vector<double> &values, double fraction);
    void setColorRange(double minValue, double maxValue);
    double positiveFloor() const;

    BackendVbtOpenResult openResult;
    const QString m_headerPath;   ///< the .head file this volume came from
    WorkspaceChrome *m_workspaceChrome{ nullptr };
    const QString backendUrl;
    const QString backendToken;
    const QString sessionId;

    QPointer<QVTKOpenGLNativeWidget> vtkWidget;
    QPointer<QComboBox> comboScalarField;
    QPointer<QComboBox> comboBlendMode;  // Composite / MIP / MinIP
    QPointer<QComboBox> comboScaleMode;  // Linear / Log / Sqrt / Square / Power
    QPointer<QDoubleSpinBox> spinScaleGamma;  // exponent for the Power mode
    QPointer<QComboBox> comboBackground;
    QPointer<QSlider> sliderOpacity;
    QPointer<QDoubleSpinBox> spinRangeMin;
    QPointer<QDoubleSpinBox> spinRangeMax;
    QPointer<QPushButton> buttonAutoscale;
    QPointer<QLabel> m_propActiveScalar;   ///< Inspector row; follows the Scalar selector
    QPointer<QListWidget> fieldList;
    QPointer<QFormLayout> metadataGrid;
    bool updatingRangeControls{ false };

    // ── Sidebar ──────────────────────────────────────────────────────────────
    /// The two pages, built once and mounted in the docks (see
    /// setupWorkspaceDocks): the volume controls in the LEFT column under
    /// Session Data, the dataset info in the RIGHT Inspector.
    QPointer<QWidget> m_pageVolume;
    QPointer<QWidget> m_pageInfo;
    QPointer<QAction> m_actionSessionPanel;
    QPointer<QAction> actionShowControlsSidebar;
    void setupSidebarUi();

    // ── Scientific copilot ────────────────────────────────────────────────────
    AssistantPanel *m_copilot{ nullptr };
    /// State captured before the copilot's last batch, so Undo reverts just it.
    struct CopilotUndo
    {
        bool cameraCaptured{ false };
        double position[3]{ 0, 0, 0 };
        double focalPoint[3]{ 0, 0, 0 };
        double viewUp[3]{ 0, 0, 1 };
        double parallelScale{ 1.0 };
        bool scaleCaptured{ false };
        QString scaleText;
        bool blendCaptured{ false };
        int blendIndex{ -1 };
        bool colorFieldCaptured{ false };
        int colorFieldIndex{ -1 };
    } m_copilotUndo;
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
    vtkNew<vtkTextActor> m_loadingActor;
};

#endif

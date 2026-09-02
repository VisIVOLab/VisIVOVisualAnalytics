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
class SidebarPanel;
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
    QPointer<QLabel> summaryLabel;
    QPointer<QListWidget> fieldList;
    QPointer<QLabel> metadataLabel;
    bool updatingRangeControls{ false };

    // ── Sidebar ──────────────────────────────────────────────────────────────
    SidebarPanel *m_sidebar{ nullptr };
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

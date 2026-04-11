#ifndef vtkWindowCube_H
#define vtkWindowCube_H

#include "CatalogueOverlayUtils.h"
#include "CubeOpenPreviewTask.h"
#include "CubeUiAssembler.h"

// Controllers are forward-declared here; their full headers are included in
// vtkWindowCube.cpp where they are constructed and their methods are called.
class MomentMapController;
class NoiseController;
class PvController;

#include <vtkSmartPointer.h>
#include <vtkNew.h>

#include <QFutureWatcher>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <array>
#include <limits>
#include <memory>
#include <vector>

class QAction;
class QMenu;
class SidebarPanel;
class CatalogueTableModel;
class QDockWidget;
class QLabel;
class LUTCustomizerDialog;
class ProfileWidget;
class PvDiagramWidget;
class QTableView;
class vtkAxisActor2D;
class vtkActor;
class vtkGPUVolumeRayCastMapper;
class vtkColorTransferFunction;
class vtkContourTriangulator;
class vtkCoordinate;
class vtkFlyingEdges2D;
class vtkFlyingEdges3D;
class vtkGenericOpenGLRenderWindow;
class vtkImageData;
class vtkImageMapToColors;
class vtkLegendScaleActorWCS;
class vtkLineSource;
class vtkLookupTable;
class vtkOrientationMarkerWidget;
class vtkPlaneSource;
class vtkPiecewiseFunction;
class vtkPoints;
class vtkCellArray;
class vtkPolyData;
class vtkRegularPolygonSource;
class vtkRenderer;
class vtkTextActor;
class vtkTrivialProducer;
class vtkVolume;

QT_BEGIN_NAMESPACE
namespace Ui {
class vtkWindowCube;
}
QT_END_NAMESPACE

struct AsyncIsosurfaceResult
{
    vtkSmartPointer<vtkPolyData> mesh;
    QString errorMessage;
    int requestId{ 0 };
    bool meshInDisplayCoordinates{ false };
};

struct RemoteCubePreviewResult
{
    bool valid{ false };
    QString errorMessage;
    vtkSmartPointer<vtkImageData> cubeImageData;
    std::array<double, 2> cubeRange{ 0., 0. };
    std::array<int, 6> dataExtent{ 0, -1, 0, -1, 0, -1 };
};

struct RemoteCubeSliceResult
{
    bool valid{ false };
    QString errorMessage;
    vtkSmartPointer<vtkImageData> imageData;
    std::array<double, 2> imageRange{ 0., 0. };
    int index{ 0 };
};

struct RemoteCubeSubvolumeResult
{
    bool valid{ false };
    QString errorMessage;
    vtkSmartPointer<vtkImageData> cubeImageData;
    std::array<double, 2> cubeRange{ 0., 0. };
    std::array<int, 6> dataExtent{ 0, -1, 0, -1, 0, -1 };
    double cubeMean{ 0. };
    double cubeRms{ 0. };
};

class vtkWindowCube : public QMainWindow
{
    Q_OBJECT

public:
    // A dataset must always be opened via the backend (datasetId is required).
    // Use MainWindow::openData() → BackendClient::openDataset() to obtain the
    // datasetId before constructing this window.
    vtkWindowCube(const QString &filepath, const QString &backendUrl, const QString &datasetId,
                  int remoteWidth, int remoteHeight, int remoteDepth,
                  const std::array<double, 3> &remoteSpacing,
                  const std::array<double, 3> &remoteOrigin,
                  const std::array<QString, 3> &remoteCtype,
                  const std::array<QString, 3> &remoteCunit,
                  const std::array<double, 3> &remoteCrval,
                  const std::array<double, 3> &remoteCrpix,
                  const std::array<double, 3> &remoteCdelt,
                  const QString &remoteDegenerateAxesSummary,
                  const QString &remoteSpectralAxisType = QString(),
                  const QString &remoteSpectralAxisUnit = QString(),
                  const QString &remoteWcsStatus = QStringLiteral("ok"),
                  const QString &remoteWcsWarningMessage = QString(),
                  const QString &remoteSessionId = QString(),
                  const QString &remoteBackendToken = QString(), QWidget *parent = nullptr);
    ~vtkWindowCube() override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void resetCameraFront();
    void resetCameraBack();
    void resetCameraTop();
    void resetCameraRight();
    void resetCameraBottom();
    void resetCameraLeft();

    void renderImage();
    void changeImageRenderer();

    void changeCubeRender();
    void changeCubeColor();

    void thresholdSliderChanged(int action);
    void thresholdLineChanged();

    void sliceSliderChanged(int action);
    void sliceSpinChanged(int value);

    void changeLegendWCS();

    void showLUTCustomizer();
    void updateLUTCustomizer();

    void setInteractorStyleImage();
    void setInteractorStyleProfile();
    void setInteractorStyleRegion();
    void toggleProbeFreeze();
    void finishRegionInteraction();
    void finalizePvInteraction();
    void extractSpectrumAtCurrentProbe();
    void setPvModeActive(bool active);
    void setProbeModeActive(bool active);
    void syncCatalogueTableSelection(int index);

signals:
    void catalogueSourceSelectionChanged(int index);

public:
    enum class RemoteCubeDisplayState
    {
        Preview,
        LoadingFullResolution,
        FullResolution,
    };

    enum class SpectralAxisKind
    {
        Channel,
        Frequency,
        RadioVelocity,
        OpticalVelocity,
        GenericVelocity,
        GenericSpectral,
    };

    struct SpectralAxisDescriptor
    {
        SpectralAxisKind kind{ SpectralAxisKind::Channel };
        QString unit;
        QString label;
        QString sourceLabel;
        bool trusted{ false };
        bool inferred{ false };
        bool physical{ false };
    };

public:
    enum class RegionMode
    {
        None,
        Box,
        Circle,
        Polygon,
        Annulus,
    };

private:
    struct MomentMapApplyResult
    {
        vtkSmartPointer<vtkImageData> imageData;
        std::array<double, 2> imageRange;
    };

    struct MomentProvenanceState
    {
        bool valid{ false };
        QString summary;
        QString details;
    };

    Ui::vtkWindowCube *ui;
    const QString filepath;
    const QString remoteBackendUrl;
    const QString remoteDatasetId;
    const QString remoteSessionId;
    const QString remoteBackendToken;
    const int remoteDatasetWidth;
    const int remoteDatasetHeight;
    const int remoteDatasetDepth;
    const std::array<double, 3> remoteDatasetSpacing;
    const std::array<double, 3> remoteDatasetOrigin;
    const std::array<QString, 3> remoteDatasetCtype;
    const std::array<QString, 3> remoteDatasetCunit;
    const std::array<double, 3> remoteDatasetCrval;
    const std::array<double, 3> remoteDatasetCrpix;
    const std::array<double, 3> remoteDatasetCdelt;
    const QString remoteDegenerateAxesSummary;
    const QString remoteSpectralAxisType;
    const QString remoteSpectralAxisUnit;
    const QString remoteWcsStatus;
    const QString remoteWcsWarningMessage;
    bool wcsDiagnosticPublished{ false };

    QPointer<LUTCustomizerDialog> lutCustomizer;
    QPointer<ProfileWidget> profileWidget;
    QPointer<ProfileWidget> probePlotWidget;
    QPointer<PvDiagramWidget> pvDiagramWidget;
    QPointer<QLabel> cubeOpenStateLabel;
    QPointer<QLabel> hoverReadoutLabel;
    QPointer<QLabel> dataStateLabel;
    QPointer<QLabel> sanityLabel;
    QPointer<QLabel> wcsStatusLabel;
    QPointer<QLabel> momentProvenanceLabel;
    QPointer<QLabel> catalogueInfoLabel;
    QPointer<QDockWidget> catalogueDock;
    QPointer<QTableView> catalogueTableView;
    QPointer<CatalogueTableModel> catalogueTableModel;
    bool syncingCatalogueSelection{ false };
    // ── Sidebar ───────────────────────────────────────────────────────────────
    SidebarPanel *m_sidebar{ nullptr };
    QPointer<QAction> actionShowControlsSidebar;
    // Owns all sidebar widget pointers and the four page-build methods.
    std::unique_ptr<CubeUiAssembler> m_uiAssembler;
    void setupSidebar();

    // ── 3D View Settings – VTK-side state (UI widgets are in m_uiAssembler) ──
    QString             m_activeColorMapName;
    // Menu actions for volume blend mode (created programmatically).
    QPointer<QAction>   m_actionComposite;
    QPointer<QAction>   m_actionMIP;
    QPointer<QAction>   m_actionMinIP;
    QPointer<QMenu>     m_menuVolumeRendering;
    void applyColorMapByName(const QString &name);
    void applyVolumeBlendMode(int idx);  // 0=Composite, 1=MIP, 2=MinIP
    void updateVolumeModeVisibility(bool volumeActive);

    // ── Moment card state machine ─────────────────────────────────────────────
    using MomentCardState = CubeUiAssembler::MomentCardData::State;
    MomentCardState m_momentCardState{ MomentCardState::NoMoment };
    QString         m_momentCardDetail;  // context string for Computing/Error states
    void setMomentCardState(MomentCardState state, const QString &detail = {});

    // ── 2D View Settings – VTK-side state (UI widgets are in m_uiAssembler) ──
    QString             m_activeSliceLutName;
    QString             m_activeMomentLutName;
    void applySliceLutByName(const QString &name);
    void applyMomentLutByName(const QString &name);
    void update2dModeVisibility(bool sliceActive);
    void requestMomentMode();

    // ── Tools page ────────────────────────────────────────────────────────────
    QPointer<QAction> actionCameraRoi;   // View menu + Tools sidebar

    // ── Info / Stats page ────────────────────────────────────────────────────
    void updateInfoStatsPage();

    // ── Workspace docks ───────────────────────────────────────────────────────
    QPointer<QDockWidget> dockCubeView;
    QPointer<QDockWidget> dockSliceView;
    QPointer<QAction> actionResetLayout;
    void setupWorkspaceDocks();
    void resetWorkspaceLayout();

    QPointer<QAction> actionShowWcsAxes;
    QPointer<QAction> actionWcsSexagesimal;
    QPointer<QAction> actionWcsDecimal;
    QPointer<QAction> actionBoxRegion;
    QPointer<QAction> actionCircleRegion;
    QPointer<QAction> actionPolygonRegion;
    QPointer<QAction> actionAnnulusRegion;
    QPointer<QAction> actionExtractPvDiagram;
    QPointer<QAction> actionEstimateNoise;
    QPointer<QAction> actionLoadCatalogueOverlay;
    QPointer<QAction> actionShowCatalogueOverlay;
    QPointer<QAction> actionShowCatalogueLabels;
    QPointer<QAction> actionClearCatalogueOverlay;
    QFutureWatcher<CubeOpenStageResult> cubeOpenWatcher;
    QFutureWatcher<RemoteCubePreviewResult> remotePreviewWatcher;
    QFutureWatcher<RemoteCubeSubvolumeResult> remoteHighResCubeWatcher;
    QFutureWatcher<AsyncIsosurfaceResult> isosurfaceWatcher;
    // Moment computation is delegated to MomentMapController.
    MomentMapController *momentController{ nullptr };
    // Noise estimation is delegated to NoiseController.
    NoiseController *noiseController{ nullptr };
    // PV diagram backend fetch is delegated to PvController.
    PvController *pvController{ nullptr };
    int currentRemotePreviewRequestId{ 0 };
    int currentRemoteHighResRequestId{ 0 };
    int currentRemoteSliceRequestId{ 0 };
    std::array<int, 6> currentRemoteRoi{ 0, 0, 0, 0, 0, 0 };
    QString currentRemoteRefinementModeLabel{ QStringLiteral("Full") };
    bool currentRemoteRoiThicknessExpanded{ false };
    int currentRequestedRemoteSliceIndex{ 0 };
    int pendingRemoteSliceIndex{ 0 };
    int activeRemoteSliceRequests{ 0 };
    int activeRemoteIsosurfaceRequests{ 0 };
    QTimer isosurfaceDebounceTimer;
    QTimer remoteSliceDebounceTimer;
    QTimer remoteFullResolutionStateTimer;
    int currentIsosurfaceRequestId{ 0 };
    int currentFullCubeGeneration{ 0 };
    QTimer statusMessageClearTimer;
    QElapsedTimer statusMessageElapsed;
    int statusMessageMinDurationMs{ 0 };
    bool persistentStatusActive{ false };
    bool usingHighResCube{ false };
    bool useCameraRoiRefinement{ false };
    bool pendingRemoteRefinementReload{ false };
    bool probeModeActive{ false };
    RegionMode regionMode{ RegionMode::None };
    bool pvModeActive{ false };
    bool pvDragging{ false };
    bool pvValid{ false };
    bool pvCursorValid{ false };
    int pvWidthPixels{ 1 };
    bool useSexagesimalWcsFormat{ false };
    bool wcsFormatExplicitlyChosen{ false };
    bool sliceWcsOverlayInitialized{ false };
    bool momentWcsOverlayInitialized{ false };
    bool probeFrozen{ false };
    bool probeValid{ false };
    bool regionDragging{ false };
    bool regionValid{ false };
    bool catalogueOverlayLoaded{ false };
    bool ignoreNextPolygonRelease{ false };
    MomentProvenanceState momentProvenanceState;
    std::array<int, 3> probeVoxel{ -1, -1, -1 };
    std::array<int, 2> regionAnchorVoxel{ -1, -1 };
    std::array<int, 2> regionCurrentVoxel{ -1, -1 };
    std::vector<std::array<int, 2>> regionPolygonVertices;
    std::vector<CatalogueOverlayEntry> catalogueOverlayEntries;
    std::vector<std::array<double, 2>> catalogueOverlayPixels;
    std::vector<std::vector<std::array<double, 2>>> catalogueOverlayPolylines;
    std::vector<int> catalogueOverlaySourceFirstPolyline;
    std::vector<int> catalogueOverlaySourcePolylineCount;
    std::vector<int> catalogueOverlayLabelIndices;
    QStringList catalogueOverlayLabels;
    QString catalogueOverlaySummary;
    int hoveredCatalogueSourceIndex{ -1 };
    int selectedCatalogueSourceIndex{ -1 };
    double regionAnnulusInnerRadius{ 0.0 };
    std::array<int, 2> pvCurrentVoxel{ -1, -1 };
    std::vector<std::array<int, 2>> pvPolylineVertices;
    RemoteCubeDisplayState remoteCubeDisplayState{ RemoteCubeDisplayState::Preview };
    static constexpr int remoteLoadingStateDelayMs = 250;
    static constexpr int remoteSliceDebounceDelayMs = 100;
    bool remoteIsosurfaceRequestInFlight{ false };
    double inFlightRemoteIsosurfaceThreshold{ std::numeric_limits<double>::quiet_NaN() };

    std::array<double, 2> currentCubeVisibleRange{ 0., 0. };
    double currentCubeInvisibleSentinel{ -1.0 };
    float lowerBound;
    float upperBound;

    // Render Windows
    vtkNew<vtkGenericOpenGLRenderWindow> sliceWin;
    vtkNew<vtkGenericOpenGLRenderWindow> momentWin;
    vtkNew<vtkOrientationMarkerWidget> axesWidget;
    vtkNew<vtkCoordinate> coordinate;
    double initialCameraPosition[3];
    double initialCameraFocalPoint[3];
    void setupCubeRenderer();
    void setupSliceRenderer();
    void setupMomentRenderer();
    void resetCubeCamera();
    void setCameraAzimuth(double az);
    void setCameraElevation(double el);
    void mouseCallback();
    bool updateProbeFromDisplayPosition(int displayX, int displayY);
    bool updateRegionFromDisplayPosition(int displayX, int displayY);
    bool updatePvFromDisplayPosition(int displayX, int displayY);
    void refreshProbeOverlay();
    void refreshRegionOverlay();
    void refreshPvOverlay();
    void clearProbe();
    void clearRegion();
    void clearPv();
    void analyzeCurrentRegion();
    bool finalizePolygonRegion();
    void extractCurrentPvDiagram();
    void loadCatalogueOverlay();
    void clearCatalogueOverlay();
    void setCatalogueOverlayVisible(bool visible);
    void rebuildCatalogueOverlay();
    void updateCatalogueOverlayLabels();
    void refreshCatalogueOverlayInteraction(int displayX, int displayY, bool fromClick);
    int catalogueSourceIndexNearDisplayPosition(int displayX, int displayY,
                                                vtkRenderer *renderer, double *distancePx = nullptr) const;
    void rebuildCatalogueHighlightOverlay(vtkPoints *points, vtkCellArray *cells, vtkPolyData *data,
                                          int sourceIndex);
    void updateCatalogueInfoPanel();
    QString catalogueSourceSummary(int sourceIndex) const;
    void ensureCatalogueDock();
    void refreshCatalogueTable();
    void setSelectedCatalogueSourceIndex(int index);
    void centerViewOnCatalogueSource(int index, double zoomFactor = 2.5);
    bool catalogueWorldToPixel(double raDeg, double decDeg, std::array<double, 2> &pixel) const;
    void updateProbeReadout(vtkImageData *imageData);
    void updateProbePlot();
    QString formatSpatialPointSummary(const std::array<int, 2> &voxel) const;
    std::vector<std::array<int, 2>> pvSampledPath(
            const std::vector<std::array<int, 2>> &vertices) const;
    std::array<double, 2> pvLocalNormalForSample(
            const std::vector<std::array<int, 2>> &sampledPoints, std::size_t index) const;
    QString selectedFrameAxisTitle(int axis) const;
    bool viewingIsosurface() const;
    bool viewingSlice() const;

    // Cube
    vtkNew<vtkTrivialProducer> cubeDisplaySource;
    vtkNew<vtkFlyingEdges3D> isosurfaceFilter;
    vtkNew<vtkActor> isosurface;
    vtkSmartPointer<vtkActor> currentIsosurfaceActor;
    vtkNew<vtkVolume> volume;
    vtkNew<vtkColorTransferFunction> volumeColorTransferFunction;
    vtkNew<vtkPiecewiseFunction> volumeOpacity;
    vtkSmartPointer<vtkGPUVolumeRayCastMapper> volumeMapper;  // stored for blend-mode changes
    vtkNew<vtkPlaneSource> remoteCuttingPlaneSource;
    vtkNew<vtkActor> remoteCuttingPlaneActor;
    bool remoteIsosurfaceReady{ false };
    void applyCubeOpenResult(const CubeOpenStageResult &result);
    void applyIsosurfaceResult(const AsyncIsosurfaceResult &result);
    void startAsyncIsosurface(double isoValue);
    void scheduleIsosurfaceRecompute();
    void setCubeRenderModeLocally(bool isosurfaceMode);
    void updateCube();
    std::array<int, 6> computeVisibleROI();
    bool requestHighResCube();
    void updateRemoteCuttingPlane(int sliceIndex);
    int remoteSliceCount() const;
    int clampRemoteSliceIndex(int sliceIndex) const;
    double remoteSliceCoordinate(int sliceIndex) const;
    SpectralAxisDescriptor spectralAxisDescriptor() const;
    double spectralAxisValue(double datasetVoxelIndex, bool *ok = nullptr) const;
    QString formatSpectralAxisValue(double datasetVoxelIndex) const;
    QString spectralAxisTitle() const;
    QString spectralAxisTooltip() const;
    void refreshSpectralAxisUi();
    bool remoteHasWcsAxis(int axis) const;
    double remoteVoxelToWcs(int axis, double voxelIndex, bool *ok = nullptr) const;
    QString remoteFormatAxisCoordinate(int axis, double voxelIndex) const;
    QString remoteAxisTitle(int axis) const;
    void updateWcsStatusIndicator();
    int selectedWcsFrame() const;
    int remoteNativeCelestialFrame() const;
    bool remoteHasCelestialAxes() const;
    bool convertRemoteCelestialCoordinates(double nativeX, double nativeY, double &frameX,
                                           double &frameY) const;
    QString formatRemoteOverlayCoordinate(int axis, double value) const;
    QString remoteOverlayAxisTitle(int axis) const;
    QString formatDegreeCoordinate(double value) const;
    void updateDataStatePanel();
    QString currentWcsFrameLabel() const;
    void setRegionMode(RegionMode mode, bool active);
    void updateSliceWcsOverlay();
    void updateMomentWcsOverlay();
    void set2dWcsOverlayVisible(bool visible);
    void ensureOverlayTickActors(vtkRenderer *renderer,
                                 std::vector<vtkSmartPointer<vtkTextActor>> &xActors,
                                 std::vector<vtkSmartPointer<vtkTextActor>> &yActors);
    void invalidateWcsOverlayCache();
    void applyDefaultWcsFormatForSelectedFrame();
    void requestWcsOverlayRender();
    void updateMomentProvenancePanel();
    void updateSanityPanel();

    // Slice
    vtkNew<vtkImageMapToColors> sliceColors;
    vtkNew<vtkLookupTable> lutSlice;
    vtkNew<vtkTrivialProducer> remoteSliceDisplaySource;
    vtkNew<vtkLegendScaleActorWCS> legendSlice;
    vtkNew<vtkAxisActor2D> sliceOverlayXAxis;
    vtkNew<vtkAxisActor2D> sliceOverlayYAxis;
    vtkNew<vtkTextActor> sliceOverlayXTitleActor;
    vtkNew<vtkTextActor> sliceOverlayYTitleActor;
    vtkNew<vtkLineSource> sliceProbeHorizontalLine;
    vtkNew<vtkLineSource> sliceProbeVerticalLine;
    vtkNew<vtkActor> sliceProbeHorizontalActor;
    vtkNew<vtkActor> sliceProbeVerticalActor;
    vtkNew<vtkActor> sliceCatalogueOverlayActor;
    vtkNew<vtkActor> sliceCatalogueHoverOverlayActor;
    vtkNew<vtkActor> sliceCatalogueSelectionOverlayActor;
    vtkNew<vtkLineSource> sliceRegionTopLine;
    vtkNew<vtkLineSource> sliceRegionBottomLine;
    vtkNew<vtkLineSource> sliceRegionLeftLine;
    vtkNew<vtkLineSource> sliceRegionRightLine;
    vtkNew<vtkActor> sliceRegionTopActor;
    vtkNew<vtkActor> sliceRegionBottomActor;
    vtkNew<vtkActor> sliceRegionLeftActor;
    vtkNew<vtkActor> sliceRegionRightActor;
    vtkNew<vtkRegularPolygonSource> sliceRegionCircleSource;
    vtkNew<vtkActor> sliceRegionCircleActor;
    vtkNew<vtkRegularPolygonSource> sliceRegionAnnulusOuterSource;
    vtkNew<vtkRegularPolygonSource> sliceRegionAnnulusInnerSource;
    vtkNew<vtkActor> sliceRegionAnnulusOuterActor;
    vtkNew<vtkActor> sliceRegionAnnulusInnerActor;
    vtkNew<vtkPoints> sliceRegionAnnulusFillPoints;
    vtkNew<vtkCellArray> sliceRegionAnnulusFillCells;
    vtkNew<vtkPolyData> sliceRegionAnnulusFillData;
    vtkNew<vtkActor> sliceRegionAnnulusFillActor;
    vtkNew<vtkPoints> sliceRegionPolygonPoints;
    vtkNew<vtkCellArray> sliceRegionPolygonCells;
    vtkNew<vtkPolyData> sliceRegionPolygonData;
    vtkNew<vtkActor> sliceRegionPolygonActor;
    vtkNew<vtkPolyData> sliceRegionPolygonFillData;
    vtkNew<vtkContourTriangulator> sliceRegionPolygonTriangulator;
    vtkNew<vtkActor> sliceRegionPolygonFillActor;
    vtkNew<vtkPoints> slicePvPoints;
    vtkNew<vtkCellArray> slicePvCells;
    vtkNew<vtkPolyData> slicePvData;
    vtkNew<vtkActor> slicePvActor;
    vtkNew<vtkPoints> slicePvUpperPoints;
    vtkNew<vtkCellArray> slicePvUpperCells;
    vtkNew<vtkPolyData> slicePvUpperData;
    vtkNew<vtkActor> slicePvUpperActor;
    vtkNew<vtkPoints> slicePvLowerPoints;
    vtkNew<vtkCellArray> slicePvLowerCells;
    vtkNew<vtkPolyData> slicePvLowerData;
    vtkNew<vtkActor> slicePvLowerActor;
    vtkNew<vtkPoints> catalogueHoverOverlayPoints;
    vtkNew<vtkCellArray> catalogueHoverOverlayCells;
    vtkNew<vtkPolyData> catalogueHoverOverlayData;
    vtkNew<vtkPoints> catalogueSelectionOverlayPoints;
    vtkNew<vtkCellArray> catalogueSelectionOverlayCells;
    vtkNew<vtkPolyData> catalogueSelectionOverlayData;
    std::vector<vtkSmartPointer<vtkTextActor>> sliceCatalogueOverlayLabelActors;
    std::vector<vtkSmartPointer<vtkTextActor>> sliceOverlayXTickActors;
    std::vector<vtkSmartPointer<vtkTextActor>> sliceOverlayYTickActors;
    std::array<double, 4> lastSliceOverlayVisibleBounds{ std::numeric_limits<double>::quiet_NaN(),
                                                         std::numeric_limits<double>::quiet_NaN(),
                                                         std::numeric_limits<double>::quiet_NaN(),
                                                         std::numeric_limits<double>::quiet_NaN() };
    std::array<int, 2> lastSliceOverlayViewportSize{ -1, -1 };
    QHash<QString, RemoteCubeSliceResult> remoteSliceCache;
    QList<QString> remoteSliceCacheLru;
    QSet<QString> remoteSliceFetchesInFlight;
    static constexpr int remoteSliceCacheCapacity = 8;
    void updateSlice();
    void updateRemoteSliceDragFeedback(int sliceIndex);
    void requestRemoteSlice(int sliceIndex);
    void applyRemoteSliceResult(const RemoteCubeSliceResult &result);
    QString remoteSliceCacheKey(int sliceIndex) const;
    void touchRemoteSliceCacheKey(const QString &key);
    void cacheRemoteSliceResult(const RemoteCubeSliceResult &result);
    bool tryApplyCachedRemoteSlice(int sliceIndex);
    void startRemoteSliceFetch(int sliceIndex, bool isPrefetch, int requestId = 0);
    void prefetchNeighborRemoteSlices(int sliceIndex);

    // Contours
    int level;
    vtkNew<vtkFlyingEdges2D> contours;
    vtkNew<vtkActor> contoursActor;
    void updateContours();
    void updateContoursVisibility();

    // Moment
    vtkNew<vtkTrivialProducer> momentDisplaySource;
    vtkNew<vtkImageMapToColors> momentColors;
    vtkNew<vtkLookupTable> lutMoment;
    vtkNew<vtkLegendScaleActorWCS> legendMoment;
    vtkNew<vtkAxisActor2D> momentOverlayXAxis;
    vtkNew<vtkAxisActor2D> momentOverlayYAxis;
    vtkNew<vtkTextActor> momentOverlayXTitleActor;
    vtkNew<vtkTextActor> momentOverlayYTitleActor;
    vtkNew<vtkLineSource> momentProbeHorizontalLine;
    vtkNew<vtkLineSource> momentProbeVerticalLine;
    vtkNew<vtkActor> momentProbeHorizontalActor;
    vtkNew<vtkActor> momentProbeVerticalActor;
    vtkNew<vtkActor> momentCatalogueOverlayActor;
    vtkNew<vtkActor> momentCatalogueHoverOverlayActor;
    vtkNew<vtkActor> momentCatalogueSelectionOverlayActor;
    vtkNew<vtkLineSource> momentRegionTopLine;
    vtkNew<vtkLineSource> momentRegionBottomLine;
    vtkNew<vtkLineSource> momentRegionLeftLine;
    vtkNew<vtkLineSource> momentRegionRightLine;
    vtkNew<vtkActor> momentRegionTopActor;
    vtkNew<vtkActor> momentRegionBottomActor;
    vtkNew<vtkActor> momentRegionLeftActor;
    vtkNew<vtkActor> momentRegionRightActor;
    vtkNew<vtkRegularPolygonSource> momentRegionCircleSource;
    vtkNew<vtkActor> momentRegionCircleActor;
    vtkNew<vtkRegularPolygonSource> momentRegionAnnulusOuterSource;
    vtkNew<vtkRegularPolygonSource> momentRegionAnnulusInnerSource;
    vtkNew<vtkActor> momentRegionAnnulusOuterActor;
    vtkNew<vtkActor> momentRegionAnnulusInnerActor;
    vtkNew<vtkPoints> momentRegionAnnulusFillPoints;
    vtkNew<vtkCellArray> momentRegionAnnulusFillCells;
    vtkNew<vtkPolyData> momentRegionAnnulusFillData;
    vtkNew<vtkActor> momentRegionAnnulusFillActor;
    vtkNew<vtkPoints> momentRegionPolygonPoints;
    vtkNew<vtkCellArray> momentRegionPolygonCells;
    vtkNew<vtkPolyData> momentRegionPolygonData;
    vtkNew<vtkActor> momentRegionPolygonActor;
    vtkNew<vtkPolyData> momentRegionPolygonFillData;
    vtkNew<vtkContourTriangulator> momentRegionPolygonTriangulator;
    vtkNew<vtkActor> momentRegionPolygonFillActor;
    vtkNew<vtkPoints> momentPvPoints;
    vtkNew<vtkCellArray> momentPvCells;
    vtkNew<vtkPolyData> momentPvData;
    vtkNew<vtkActor> momentPvActor;
    vtkNew<vtkPoints> momentPvUpperPoints;
    vtkNew<vtkCellArray> momentPvUpperCells;
    vtkNew<vtkPolyData> momentPvUpperData;
    vtkNew<vtkActor> momentPvUpperActor;
    vtkNew<vtkPoints> momentPvLowerPoints;
    vtkNew<vtkCellArray> momentPvLowerCells;
    vtkNew<vtkPolyData> momentPvLowerData;
    vtkNew<vtkActor> momentPvLowerActor;
    vtkNew<vtkPoints> catalogueOverlayPoints;
    vtkNew<vtkCellArray> catalogueOverlayCells;
    vtkNew<vtkPolyData> catalogueOverlayData;
    std::vector<vtkSmartPointer<vtkTextActor>> momentCatalogueOverlayLabelActors;
    std::vector<vtkSmartPointer<vtkTextActor>> momentOverlayXTickActors;
    std::vector<vtkSmartPointer<vtkTextActor>> momentOverlayYTickActors;
    std::array<double, 4> lastMomentOverlayVisibleBounds{ std::numeric_limits<double>::quiet_NaN(),
                                                          std::numeric_limits<double>::quiet_NaN(),
                                                          std::numeric_limits<double>::quiet_NaN(),
                                                          std::numeric_limits<double>::quiet_NaN() };
    std::array<int, 2> lastMomentOverlayViewportSize{ -1, -1 };
    bool showWcsAxes{ true };
    void applyMomentMapResult(const MomentMapApplyResult &result);
    bool isBusy() const;
    void setRemoteCubeDisplayState(RemoteCubeDisplayState state);
    void showPersistentStatusMessage(const QString &text, int minDurationMs = 400);
    void clearPersistentStatusMessage();
    void setCubeOpenStateLabel(const QString &text);
    void setCubeOpenActionsEnabled(bool enabled);
    void setMomentActionsEnabled(bool enabled);
    bool hasBackendVisibleDataset() const;
    void updateBackendDerivedActionsAvailability();
    void setMomentOrder(int order);
};
#endif

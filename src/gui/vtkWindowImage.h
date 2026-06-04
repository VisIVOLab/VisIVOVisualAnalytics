#ifndef vtkWindowImage_h
#define vtkWindowImage_h

#include "CatalogueOverlayUtils.h"
#include "ImageLayerLoadTask.h"

#include <vtkNew.h>
#include <vtkSmartPointer.h>

#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <array>
#include <limits>
#include <memory>
#include <vector>

class ImageLayerController;
class ImageLayerImportService;
class LayerListModel;
class LUTCustomizerDialog;
class ProfileWidget;
class CatalogueTableModel;
class QAction;
class QCheckBox;
class QDockWidget;
class QLabel;
class ToolResultDialog;
class QTableView;
class SidebarPanel;
class vtkActor;
class vtkAxisActor2D;
class vtkCoordinate;
class QLineEdit;
class QSpinBox;
class vtkFlyingEdges2D;
class vtkImageStack;
class vtkLegendScaleActorWCS;
class vtkLookupTable;
class vtkCellArray;
class vtkContourTriangulator;
class vtkLineSource;
class vtkPoints;
class vtkPolyData;
class vtkRegularPolygonSource;
class vtkRenderer;
class vtkScalarBarActor;
class vtkTextActor;

QT_BEGIN_NAMESPACE
namespace Ui {
class vtkWindowImage;
}
QT_END_NAMESPACE

class vtkWindowImage : public QMainWindow
{
    Q_OBJECT

public:
    // A dataset must always be opened via the backend (datasetId is required).
    vtkWindowImage(const QString &filepath, const QString &backendUrl, const QString &datasetId,
                   const std::array<QString, 3> &remoteCtype,
                   const std::array<QString, 3> &remoteCunit,
                   const std::array<double, 3> &remoteCrval,
                   const std::array<double, 3> &remoteCrpix,
                   const std::array<double, 3> &remoteCdelt, const QString &remoteDegenerateAxesSummary,
                   const QString &remoteWcsStatus = QStringLiteral("ok"),
                   const QString &remoteWcsWarningMessage = QString(),
                   const QString &remoteSessionId = QString(),
                   const QString &remoteBackendToken = QString(), QWidget *parent = nullptr);
    ~vtkWindowImage();
    void closeEvent(QCloseEvent *event) override;

    /// Beam indicator — same contract as vtkWindowCube::setBeamInfo().
    /// Pass NaN major to hide. Call after construction.
    void setBeamInfo(double majorDeg, double minorDeg, double paDeg);

    /// Overlay contours from external 2-D data (e.g. a cube viewer's
    /// moment map or current slice). The data is not WCS-reprojected —
    /// it is assumed to share the same pixel grid.
    void overlayExternalContourData(const QString &label, vtkImageData *data);

    // ── VLKB layer support ────────────────────────────────────────────────────
    /// Asynchronously open @p backendPath on the backend and add its image data
    /// as a new layer in this viewer.  The layer is labelled @p displayName
    /// in the layer list rather than the raw backend temp-file name.
    /// Safe to call from the GUI thread while the viewer is open.
    void addLayerFromBackendPath(const QString &backendPath, const QString &displayName);

    /// Set/clear the human-readable label for an existing layer row.
    void setLayerDisplayName(int row, const QString &name);


private slots:
    void showLUTCustomizer();
    void updateLUTCustomizer();

    void addLocalFile();

    void vtkRender();
    void changeLegendWCS();
    void changeCurrentColorMap();
    void changeCurrentColorScale();
    void changeCurrentLayerOpacity();
    void showCurrentLayerSettings();

    // Interactors
    void setInteractorStyleImage();
    void setInteractorStyleProfile();
    void setInteractorStyleRegion();
    void toggleProbeFreeze();
    void finishRegionInteraction();
    void setProbeModeActive(bool active);
    void beginImageInteraction();
    void endImageInteraction();
    void syncCatalogueTableSelection(int index);

signals:
    void catalogueSourceSelectionChanged(int index);

public:
    enum class RegionMode
    {
        None,
        Box,
        Circle,
        Polygon,
        Annulus,
    };

    enum class MeasurementMode
    {
        None,
        Ruler,
        Angle,
    };

private:
    Ui::vtkWindowImage *ui;
    const QString filepath;
    const QString remoteBackendUrl;
    const QString remoteDatasetId;
    const QString remoteSessionId;
    const QString remoteBackendToken;
    const std::array<QString, 3> remoteDatasetCtype;
    const std::array<QString, 3> remoteDatasetCunit;
    const std::array<double, 3> remoteDatasetCrval;
    const std::array<double, 3> remoteDatasetCrpix;
    const std::array<double, 3> remoteDatasetCdelt;
    const QString remoteDegenerateAxesSummary;
    const QString remoteWcsStatus;
    const QString remoteWcsWarningMessage;
    bool wcsDiagnosticPublished{ false };
    bool remoteDisplayingPreview{ false };
    std::array<int, 2> remoteFullImageDims{ 0, 0 };
    bool transitioningToFull{ false };
    bool suspendLutEditorUpdates{ false };
    bool displayStateInitialized{ false };
    bool userAdjustedDisplayState{ false };
    int remoteLoadGeneration{ 0 };
    QPointer<LUTCustomizerDialog> lutCustomizer;
    QPointer<ProfileWidget> profileWidget;
    QPointer<ToolResultDialog> m_regionResultDialog;
    int m_regionDialogGeneration{ 0 };
    QPointer<QCheckBox> wcsAxesCheck;
    QPointer<QLabel> hoverReadoutLabel;
    QPointer<QLabel> dataStateLabel;
    QPointer<QLabel> sanityLabel;
    QPointer<QLabel> wcsStatusLabel;
    QPointer<QLabel> catalogueInfoLabel;
    QPointer<QDockWidget> catalogueDock;
    QPointer<QTableView> catalogueTableView;
    QPointer<CatalogueTableModel> catalogueTableModel;
    bool syncingCatalogueSelection{ false };
    QPointer<QAction> actionWcsSexagesimal;
    QPointer<QAction> actionWcsDecimal;
    QPointer<QAction> actionExtractSpectrum;
    QPointer<QAction> actionBoxRegion;
    QPointer<QAction> actionCircleRegion;
    QPointer<QAction> actionPolygonRegion;
    QPointer<QAction> actionAnnulusRegion;
    QPointer<QAction> actionLoadCatalogueOverlay;
    QPointer<QAction> actionShowCatalogueOverlay;
    QPointer<QAction> actionShowCatalogueLabels;
    QPointer<QAction> actionClearCatalogueOverlay;
    QPointer<QAction> actionShowWcsAxes;
    QFutureWatcher<ImageLayerLoadResult> layerLoadWatcher;
    QFutureWatcher<ImageLayerLoadResult> remoteImageWatcher;
    QFutureWatcher<ImageLayerLoadResult> remoteFullImageWatcher;
    /// Set by addLayerFromBackendPath before calling addLayerImage so that the
    /// layerLoadWatcher completion slot can rename the new layer to the VLKB label.
    QString m_pendingVlkbDisplayName;
    QTimer statusMessageClearTimer;
    QElapsedTimer statusMessageElapsed;
    int statusMessageMinDurationMs{ 0 };
    bool persistentStatusActive{ false };
    std::unique_ptr<ImageLayerImportService> importService;

    // ── Sidebar ───────────────────────────────────────────────────────────────
    SidebarPanel *m_sidebar{ nullptr };
    QPointer<QAction> actionShowControlsSidebar;
    void setupSidebar();

    // Layout B-style top toolbar: path-pill + tag chips + Find/Command/Export.
    void setupViewerToolbar();

    // Opens a non-modal dialog showing the FITS header cards for `filepath`.
    // Triggered by the primary tag chip in the viewer toolbar.
    void showFitsHeaderDialog();

    // Renderer
    vtkNew<vtkLegendScaleActorWCS> legendWCS;
    vtkNew<vtkAxisActor2D> overlayXAxis;
    vtkNew<vtkAxisActor2D> overlayYAxis;
    vtkNew<vtkTextActor> overlayXTitleActor;
    vtkNew<vtkTextActor> overlayYTitleActor;
    std::vector<vtkSmartPointer<vtkTextActor>> overlayXTickActors;
    std::vector<vtkSmartPointer<vtkTextActor>> overlayYTickActors;
    vtkNew<vtkTextActor> m_loadingActor;
    vtkNew<vtkScalarBarActor> colorbar;
    vtkNew<vtkCoordinate> coordinate;
    bool showWcsAxes{ true };
    bool probeModeActive{ false };
    vtkSmartPointer<vtkActor> m_beamActor;

    // Contour overlay — same pipeline as the cube viewer: vtkFlyingEdges2D
    // connected to the master layer, with Level/Lower/Upper controls in the
    // sidebar's CONTOURS section. Also supports external FITS overlays.
    QPointer<QAction> m_actToggleContours;
    QPointer<QAction> m_actLoadExternalContour;
    QPointer<QAction> m_actClearAllContours;
    QPointer<QAction> m_actExportToWorkspace;
    vtkSmartPointer<vtkFlyingEdges2D> m_contourFilter;
    vtkSmartPointer<vtkActor>         m_contourActor;
    bool                     m_contoursVisible{ false };
    /// True when the contour filter still points at stale data (placeholder
    /// or a previous master image). `setupContourPipeline()` clears it.
    /// Used so we can defer the expensive vtkFlyingEdges2D rebuild until the
    /// user actually asks for contours, instead of paying its 10–20 s cost
    /// on every preview→full swap of a large image.
    bool                     m_contourPipelineDirty{ true };
    int                      m_contourLevel{ 15 };
    double                   m_contourLower{ 0.0 };
    double                   m_contourUpper{ 1.0 };
    void setupContourPipeline();
    void updateContours();
    void setContoursVisible(bool vis);
    QPointer<QLineEdit> m_contourLowerEdit;
    QPointer<QLineEdit> m_contourUpperEdit;
    QPointer<QSpinBox>  m_contourLevelSpin;

    // External FITS contour layers
    struct ExternalContourLayer {
        QString label;
        vtkSmartPointer<vtkActor> actor;
    };
    QVector<ExternalContourLayer> m_externalContourLayers;
    void addExternalContours();
    void clearAllContours();
    RegionMode regionMode{ RegionMode::None };
    bool useSexagesimalWcsFormat{ false };
    bool wcsFormatExplicitlyChosen{ false };
    bool wcsOverlayInitialized{ false };
    bool probeFrozen{ false };
    bool probeValid{ false };
    bool regionDragging{ false };
    bool regionValid{ false };
    bool catalogueOverlayLoaded{ false };
    bool largeImageModeActive{ false };
    bool suspendHeavyOverlayUpdates{ false };
    bool ignoreNextPolygonRelease{ false };
    std::array<int, 2> probeVoxel{ -1, -1 };
    std::array<int, 2> regionAnchorVoxel{ -1, -1 };
    std::array<int, 2> regionCurrentVoxel{ -1, -1 };
    std::vector<std::array<int, 2>> regionPolygonVertices;
    std::vector<std::array<double, 2>> savedLinearDisplayRanges;
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
    std::array<double, 4> lastOverlayVisibleBounds{ std::numeric_limits<double>::quiet_NaN(),
                                                    std::numeric_limits<double>::quiet_NaN(),
                                                    std::numeric_limits<double>::quiet_NaN(),
                                                    std::numeric_limits<double>::quiet_NaN() };
    std::array<int, 2> lastOverlayViewportSize{ -1, -1 };
    int lastRenderedWcsFrame{ std::numeric_limits<int>::min() };
    int lastRenderedWcsFormat{ -1 };
    vtkNew<vtkLineSource> probeHorizontalLine;
    vtkNew<vtkLineSource> probeVerticalLine;
    vtkNew<vtkActor> probeHorizontalActor;
    vtkNew<vtkActor> probeVerticalActor;
    vtkNew<vtkLineSource> regionTopLine;
    vtkNew<vtkLineSource> regionBottomLine;
    vtkNew<vtkLineSource> regionLeftLine;
    vtkNew<vtkLineSource> regionRightLine;
    vtkNew<vtkActor> regionTopActor;
    vtkNew<vtkActor> regionBottomActor;
    vtkNew<vtkActor> regionLeftActor;
    vtkNew<vtkActor> regionRightActor;
    vtkNew<vtkRegularPolygonSource> regionCircleSource;
    vtkNew<vtkActor> regionCircleActor;
    vtkNew<vtkRegularPolygonSource> regionAnnulusOuterSource;
    vtkNew<vtkRegularPolygonSource> regionAnnulusInnerSource;
    vtkNew<vtkActor> regionAnnulusOuterActor;
    vtkNew<vtkActor> regionAnnulusInnerActor;
    vtkNew<vtkPoints> regionAnnulusFillPoints;
    vtkNew<vtkCellArray> regionAnnulusFillCells;
    vtkNew<vtkPolyData> regionAnnulusFillData;
    vtkNew<vtkActor> regionAnnulusFillActor;
    vtkNew<vtkPoints> regionPolygonPoints;
    vtkNew<vtkCellArray> regionPolygonCells;
    vtkNew<vtkPolyData> regionPolygonData;
    vtkNew<vtkActor> regionPolygonActor;
    vtkNew<vtkPolyData> regionPolygonFillData;
    vtkNew<vtkContourTriangulator> regionPolygonTriangulator;
    vtkNew<vtkActor> regionPolygonFillActor;
    vtkNew<vtkPoints> catalogueOverlayPoints;
    vtkNew<vtkCellArray> catalogueOverlayCells;
    vtkNew<vtkPolyData> catalogueOverlayData;
    vtkNew<vtkActor> catalogueOverlayActor;
    vtkNew<vtkPoints> catalogueHoverOverlayPoints;
    vtkNew<vtkCellArray> catalogueHoverOverlayCells;
    vtkNew<vtkPolyData> catalogueHoverOverlayData;
    vtkNew<vtkActor> catalogueHoverOverlayActor;
    vtkNew<vtkPoints> catalogueSelectionOverlayPoints;
    vtkNew<vtkCellArray> catalogueSelectionOverlayCells;
    vtkNew<vtkPolyData> catalogueSelectionOverlayData;
    vtkNew<vtkActor> catalogueSelectionOverlayActor;
    std::vector<vtkSmartPointer<vtkTextActor>> catalogueOverlayLabelActors;
    void setupRenderer();
    void mouseCallback();
    bool updateProbeFromDisplayPosition(int displayX, int displayY);
    void refreshProbeOverlay();
    void clearProbe();
    void updateProbeProfile();
    void updateWcsOverlay();
    void setWcsOverlayVisible(bool visible);
    void ensureOverlayTickActors(vtkRenderer *renderer);
    void invalidateWcsOverlayCache();
    void applyDefaultWcsFormatForSelectedFrame();
    void applyInitialAutoscale(vtkImageData *imageData, vtkLookupTable *lut, const char *context);
    void requestWcsOverlayRender();
    void refreshWcsOverlayImmediately();
    void updateWcsStatusIndicator();
    void updateDataStatePanel();
    void updateSanityPanel();
    QString currentWcsFrameLabel() const;
    void setRegionMode(RegionMode mode, bool active);
    bool updateRegionFromDisplayPosition(int displayX, int displayY);
    void refreshRegionOverlay();
    void clearRegion();
    void analyzeCurrentRegion();
    bool finalizePolygonRegion();
    void updateLargeImageMode(vtkImageData *imageData, const char *context);
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
    QString selectedFrameAxisTitle(int axis) const;

    // Stack
    vtkNew<vtkImageStack> stack;
    LayerListModel *layers;
    std::unique_ptr<ImageLayerController> layerController;
    void applyLoadedLayer(const ImageLayerLoadResult &result);
    void applyRemoteMasterLayer(const ImageLayerLoadResult &result);
    void startRemoteFullResolutionLoad();
    void markDisplayStateAdjusted(const char *reason);
    bool isBusy() const;
    void showPersistentStatusMessage(const QString &text, int minDurationMs = 400);
    void clearPersistentStatusMessage();
    void setLayerImportEnabled(bool enabled);
    int currentLayerIndex() const;
    void addLayerImage(const std::string &filepath);
    bool remoteHasWcsAxis(int axis) const;
    double remoteVoxelToWcs(int axis, double voxelIndex, bool *ok = nullptr) const;
    QString remoteFormatAxisCoordinate(int axis, double voxelIndex) const;
    QString remoteAxisTitle(int axis) const;
    int selectedWcsFrame() const;
    int remoteNativeCelestialFrame() const;
    bool remoteHasCelestialAxes() const;
    bool convertRemoteCelestialCoordinates(double nativeX, double nativeY, double &frameX,
                                           double &frameY) const;
    QString formatRemoteOverlayCoordinate(int axis, double value) const;
    QString remoteOverlayAxisTitle(int axis) const;
    QString formatDegreeCoordinate(double value) const;

    // ── Measurement tools (Ruler / Angle) ────────────────────────────────────
    MeasurementMode m_measMode{ MeasurementMode::None };
    std::vector<std::array<int, 2>> m_measPoints;
    vtkNew<vtkLineSource> m_measLine1;
    vtkNew<vtkLineSource> m_measLine2;
    vtkNew<vtkActor> m_measLineActor1;
    vtkNew<vtkActor> m_measLineActor2;
    vtkSmartPointer<vtkTextActor> m_measLabelActor;
    QPointer<QAction> m_actRuler;
    QPointer<QAction> m_actAngle;
    QPointer<QAction> m_actClearMeas;
    void setMeasurementMode(MeasurementMode mode);
    void addMeasurementPoint(int voxelX, int voxelY);
    void refreshMeasurementOverlay();
    void clearMeasurement();
    double angularSeparationDeg(double vx1, double vy1, double vx2, double vy2) const;

    // ── Histogram panel ──────────────────────────────────────────────────────
    QPointer<QWidget> m_histogramWindow;
    void showHistogramPanel();

    // ── Annotations ──────────────────────────────────────────────────────────
    struct Annotation {
        enum Type { Text, Arrow };
        Type type{ Text };
        QString text;
        double worldX{ 0 };
        double worldY{ 0 };
        double arrowEndX{ 0 };
        double arrowEndY{ 0 };
    };
    enum class AnnotPlaceMode { None, Text, ArrowTip, ArrowLabel };
    AnnotPlaceMode m_annotPlaceMode{ AnnotPlaceMode::None };
    QString m_pendingAnnotText;
    double m_pendingArrowTipX{ 0 };
    double m_pendingArrowTipY{ 0 };
    vtkNew<vtkPolyData> m_annotPreviewData;
    vtkNew<vtkActor> m_annotPreviewActor;
    void updateAnnotationPreview(double worldX, double worldY);
    std::vector<Annotation> m_annotations;
    std::vector<vtkSmartPointer<vtkTextActor>> m_annotTextActors;
    std::vector<vtkSmartPointer<vtkActor>> m_annotArrowActors;
    void addTextAnnotation();
    void addArrowAnnotation();
    void placeAnnotationAt(double worldX, double worldY);
    void cancelAnnotationPlacement();
    void clearAnnotations();
    void refreshAnnotationOverlay();
    void saveAnnotationsJson();
    void loadAnnotationsJson();
    QString annotationsJsonPath() const;

    // ── Blink / Compare ──────────────────────────────────────────────────────
    QTimer m_blinkTimer;
    bool m_blinkState{ false };
    int m_blinkIntervalMs{ 250 };
    QPointer<QAction> m_actBlink;
    void toggleBlink(bool active);
    void blinkTick();

    // ── Stokes / Radio polarimetry analysis ──────────────────────────────────
public:
    enum class StokesRole {
        None, I, Q, U, V,
        DerivedP,     ///< Linear pol intensity √(Q²+U²)
        DerivedPA,    ///< Linear pol angle ½·atan2(U,Q) in degrees
        DerivedFrac,  ///< Fractional pol P/I
        DerivedPdb,   ///< Bias-corrected P  √(max(0, P²−σ²))
        DerivedAlpha, ///< Spectral index   α = log(S₂/S₁) / log(ν₂/ν₁)
        DerivedRM,    ///< Faraday rotation measure (rad m⁻²)
    };

private:
    QHash<int, StokesRole> m_stokesRoles;  ///< layer index → Stokes role
    double m_beamMajorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double m_beamMinorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double m_beamPaDeg{ std::numeric_limits<double>::quiet_NaN() };

    // Polarization vector overlay
    vtkNew<vtkPoints> m_polVecPoints;
    vtkNew<vtkCellArray> m_polVecCells;
    vtkNew<vtkPolyData> m_polVecData;
    vtkNew<vtkActor> m_polVecActor;
    bool m_polVecsVisible{ false };
    int m_polVecGridStep{ 12 };
    double m_polVecScale{ 1.0 };
    double m_polVecSnrMin{ 3.0 };

    QPointer<QAction> m_actLoadStokesCompanions;
    QPointer<QAction> m_actComputeStokesP;
    QPointer<QAction> m_actComputeStokesPA;
    QPointer<QAction> m_actComputeStokesFrac;
    QPointer<QAction> m_actComputeStokesPdebiased;
    QPointer<QAction> m_actComputeSpectralIndex;
    QPointer<QAction> m_actComputeFaradayRM;
    QPointer<QAction> m_actTogglePolVecs;

    void detectAndLoadStokesCompanions();
    QString findStokesCompanionPath(const QString &iPath, const QString &role) const;
    int findLayerByStokesRole(StokesRole role) const;
    void computeStokesP();
    void computeStokesPA();
    void computeStokesFractional();
    void computeStokesPdebiased();
    void computeSpectralIndex();
    void computeFaradayRM();
    void addDerivedLayer(vtkSmartPointer<vtkImageData> data,
                         const QString &name, StokesRole role,
                         const QString &colorMap,
                         double rangeMin, double rangeMax);
    void rebuildPolarizationVectors();
    void setPolVectorsVisible(bool vis);
    double beamAreaPixels() const;
    bool hasBeamInfo() const;
    QString stokesRoleLabel(StokesRole r) const;
};

#endif

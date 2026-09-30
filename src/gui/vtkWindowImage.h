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
#include "vtk/LutScale.h"

#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <array>
#include <limits>
#include <memory>
#include <vector>

class WorkspaceChrome;
class ImageLayerController;
class ImageLayerImportService;
class QMessageBox;
class LayerListModel;
class LUTCustomizerDialog;
class ProfileWidget;
class CatalogueTableModel;
class QAction;
class QJsonObject;
class QJsonArray;
class QCheckBox;
class QDockWidget;
class QProgressDialog;
class QFrame;
class QLabel;
class ToolResultDialog;
class QTableView;
class QTreeView;
class SegmentedToggle;
class SidebarPanel;
class vtkActor;
class vtkAxisActor2D;
class vtkCoordinate;
class vtkActor2D;
class vtkInteractorObserver;
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
    /// Record what the host computed for THIS window, as products of this
    /// window. Public because MainWindow does the work and calls back.
    void recordPublicationFigure(const QString &outputPath);
    void recordImageQuality(const QVariantMap &metrics);

    // A dataset must always be opened via the backend (datasetId is required).
    vtkWindowImage(const QString &filepath, const QString &backendUrl, const QString &datasetId,
                   const std::array<QString, 3> &remoteCtype,
                   const std::array<QString, 3> &remoteCunit,
                   const std::array<double, 3> &remoteCrval,
                   const std::array<double, 3> &remoteCrpix,
                   const std::array<double, 3> &remoteCdelt, const QString &remoteBunit,
                   const QString &remoteDegenerateAxesSummary,
                   const QString &remoteWcsStatus = QStringLiteral("ok"),
                   const QString &remoteWcsWarningMessage = QString(),
                   const QString &remoteSessionId = QString(),
                   const QString &remoteBackendToken = QString(), QWidget *parent = nullptr);
    ~vtkWindowImage();
    void closeEvent(QCloseEvent *event) override;

    /// Beam indicator — same contract as vtkWindowCube::setBeamInfo().
    /// Pass NaN major to hide. Call after construction.
    void setBeamInfo(double majorDeg, double minorDeg, double paDeg);

    /// Switch the viewer to dynamic-spectrum / beamformed mode. Disables
    /// celestial-WCS specific tools (catalogue overlay, polarization vectors,
    /// Stokes analysis, beam indicator, frame switch), relabels the axes as
    /// "Time / Frequency", and re-routes the sanity panel to skip the
    /// celestial-WCS checks. Idempotent.
    void setDynamicSpectrumMode(bool enabled);

private slots:
    void runDynspecDedispersion();
    void runDynspecPulseProfile();
    void runDynspecRfiMask();

public:

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

    /// File -> Add New Layer: pick a FITS from the BACKEND's filesystem (the name
    /// said "local" back when that was the same thing) and add it as a layer.
    void addLayerFromBrowser();

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
    /// Double-click on the image: if a catalogue source is under the cursor,
    /// select it and zoom to it (the table's double-click already did this).
    void zoomToCatalogueSourceUnderCursor();
    /// Write the loaded overlay to CSV: what is on screen, with the sky frame
    /// named in the header rather than implied.
    void exportCatalogueOverlay();
    /// Open the SED of the catalogue source at @p entryIndex. For a band-merged
    /// catalogue the whole group of per-band detections becomes one SED; for a
    /// flat one the row's own flux columns do.
    void showCatalogueSourceSed(int entryIndex);
    void setProbeModeActive(bool active);
    void beginImageInteraction();
    void endImageInteraction();
    void syncCatalogueTableSelection(int index);

signals:
    /// Asks the host to render a publication figure / measure image quality for
    /// THIS window's dataset. The host captured this viewer's backend context
    /// when it created the window, so the tool acts on the image in front of
    /// the user instead of on "the last dataset opened" (the Data Hub's Science
    /// menu did the latter, and targeted the wrong one with two images open).
    void publicationFigureRequested();
    void imageQualityRequested();

    void catalogueSourceSelectionChanged(int index);

    /// Open this file in a window of its own — a layer that covers none of this
    /// image's sky, or the catalogue a VLKB query just saved. Only the host can
    /// open a dataset, and what the file is decides which viewer gets it.
    void openInOwnWindowRequested(const QString &path);

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

    // Apply the scientific copilot's visualisation directives to this image
    // viewer (colour map, camera). Display-only and reversible via Undo; cube-only
    // directive types are ignored. See docs/copilot-backend.md.
    void applyCopilotActions(const QList<QJsonObject> &actions);
    void undoCopilotActions();

private:
    struct CopilotSnapshot
    {
        bool valid{ false };
        bool colormapCaptured{ false };
        QString colormap;
        int layerIndex{ 0 };   // the layer whose colormap/scale was captured (multi-layer)
        bool cameraCaptured{ false };
        double camPos[3]{ 0, 0, 0 };
        double camFoc[3]{ 0, 0, 0 };
        double camUp[3]{ 0, 0, 0 };
        double camScale{ 1.0 };
        bool scaleCaptured{ false };
        LutScale::Mode scaleMode{ LutScale::Mode::Linear };   // full stretch before the batch
        double scaleGamma{ 1.0 };
        double lutRange[2]{ 0.0, 1.0 };   // LUT table range before the batch (log clamps it)
    };
    CopilotSnapshot m_copilotUndo;

    Ui::vtkWindowImage *ui;
    WorkspaceChrome *m_workspaceChrome = nullptr;   ///< shared command-bar + status-rail chrome
    QString m_windowScope;   ///< per-window product scope so this tree ignores sibling windows'
                             ///< products in the session-wide registry (see SessionDataTree).
    // Former SidebarPanel pages, migrated into the Inspector's Properties tab
    // (design #1d). Built in setupSidebar(), reparented in setupInspector().
    QPointer<QWidget> m_hiddenPageHolder;  ///< hidden container so no page is a free child of the window
    QPointer<QWidget> m_inspectorPageLayer;
    QPointer<QWidget> m_inspectorPageCatalogue;
    QPointer<QWidget> m_inspectorPageInfo;
    QPointer<QWidget> m_orphanToolsPage;   ///< kept alive, not shown (Analysis tab supersedes it)
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
    const QString remoteDatasetBunit;   ///< raw BUNIT from the FITS header (physical value unit)
    const QString remoteDegenerateAxesSummary;
    const QString remoteWcsStatus;
    const QString remoteWcsWarningMessage;
    bool wcsDiagnosticPublished{ false };
    /// When true, the dataset is a dynamic spectrum / beamformed product
    /// (time vs. frequency). Celestial-WCS specific tools and overlays are
    /// hidden; axis labels become "Time" / "Frequency".
    bool m_dynamicSpectrumMode{ false };
    /// Sidebar cards that only make sense for celestial datasets — hidden
    /// when the viewer is switched to dynamic-spectrum mode.
    QVector<QPointer<QFrame>> m_dynspecHiddenCards;
    /// Sidebar card with time-series specific tools — only visible in
    /// dynamic-spectrum mode.
    QPointer<QFrame> m_dynspecToolsCard;
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
    QPointer<QTreeView> catalogueTableView;
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
    QPointer<QAction> actionCrossmatch;
    QPointer<QAction> actionShowCatalogueOverlay;
    QPointer<QAction> actionShowCatalogueLabels;
    QPointer<QAction> actionClearCatalogueOverlay;
    QPointer<QAction> actionShowWcsAxes;
    QFutureWatcher<ImageLayerLoadResult> layerLoadWatcher;
    QFutureWatcher<ImageLayerLoadResult> remoteImageWatcher;
    QFutureWatcher<ImageLayerLoadResult> remoteFullImageWatcher;

    // ── Viewport-driven LOD (tile streaming) ────────────────────────────────
    // When the remote image has a multi-level pyramid, re-assemble only the
    // visible region at the zoom-appropriate level when the camera settles, so
    // zooming in reveals more detail than the bounded whole-image assembly.
    bool m_viewportLodEnabled{ false };   ///< true once a multi-level tiled image is loaded
    int  m_lodFullWidth{ 0 };
    int  m_lodFullHeight{ 0 };
    int  m_lodNumLevels{ 1 };
    int  m_lodTileSize{ 512 };
    int  m_lodGeneration{ 0 };            ///< bumped per request; stale replies discarded
    int  m_lodConsecutiveFailures{ 0 };   ///< bounds retry-on-failure so a dead backend isn't polled forever
    std::array<int, 5> m_lodApplied{ { -1, -1, -1, -1, -1 } };  ///< last {level,txMin,txMax,tyMin,tyMax}
    std::array<int, 5> m_lodPending{ { -1, -1, -1, -1, -1 } };  ///< signature of the in-flight request
    QTimer *m_lodSettleTimer{ nullptr };  ///< debounce camera-settle bursts
    QFutureWatcher<ImageLayerLoadResult> m_viewportLodWatcher;
    void onImageInteractionSettled();     ///< EndInteractionEvent → (re)arm the debounce
    void maybeStreamViewportLod();        ///< compute visible region + level, fetch if changed
    void onViewportLodReady();            ///< apply a viewport tile assembly (camera preserved)
    QFutureWatcher<CatalogueOverlayParseResult> m_crossmatchWatcher;  // async cross-match
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

    // ── The mode strip ───────────────────────────────────────────────────────
    // Pan, Probe, Region, Ruler and Annotate are mutually exclusive modes, and
    // until now they were armed from the Tools menu with nothing on screen to
    // say which one was on — you found out by clicking and seeing what happened.
    // The strip is that missing state, and it is two-way: arming a mode from the
    // menu moves the strip, and moving the strip arms the mode.
    enum class ViewerMode
    {
        Pan = 0,
        Probe,
        Region,
        Ruler,
        Annotate,
    };
    /// Build the strip row under the viewer toolbar.
    void setupModeStrip();

    // ── The overlays block ───────────────────────────────────────────────────
    // What is drawn ON TOP of the image — catalogue sources, polarisation
    // vectors, the WCS grid, annotations — was switched from four places: two
    // entries in Tools, one in View, and one that did not exist at all
    // (annotations could be added and cleared, never hidden). None of them said
    // how much was up there. Four rows, four counts, one place.
    /// The live Σ / μ box in the corner of the viewport, shown while a region is
    /// being dragged. Until now the numbers arrived only when the drag ended, in
    /// a dialog — so the region was chosen blind and adjusted by repetition.
    void updateRegionReadout();
    void hideRegionReadout();
    QPointer<QLabel> m_regionReadout;

    void setupOverlaysBlock();
    /// Re-read the toggles and the counts from the window's own state. Called
    /// whenever an overlay is loaded, cleared or toggled from anywhere.
    void refreshOverlaysBlock();
    /// Show or hide the annotation actors (the toggle that was missing).
    void setAnnotationsVisible(bool visible);

    QPointer<QWidget> m_overlaysBlock;
    QPointer<QCheckBox> m_overlayCatalogue;
    QPointer<QCheckBox> m_overlayLabels;
    QPointer<QCheckBox> m_overlayPolVectors;
    QPointer<QCheckBox> m_overlayWcsGrid;
    QPointer<QCheckBox> m_overlayAnnotations;
    /// The count beside each row. QPointer, not a raw pointer: the rows are
    /// children of the block, and a refresh can be triggered while the window is
    /// coming down.
    QHash<QCheckBox *, QPointer<QLabel>> m_overlayCounts;
    bool m_annotationsVisible{ true };
    bool m_syncingOverlays{ false };
    /// Arm the mode the user picked on the strip.
    void applyViewerMode(ViewerMode mode);
    /// Move the strip to whatever the window is actually doing. Called from
    /// every place that changes a mode, whichever surface started it.
    void syncModeStrip();
    /// The mode the window is in right now, read from the modes themselves —
    /// there is no separate copy of this state to fall out of step.
    ViewerMode currentViewerMode() const;

    QPointer<SegmentedToggle> m_modeStrip;
    /// Box / Circle / Polygon / Annulus — shown only while Region is armed.
    QPointer<SegmentedToggle> m_regionShapeStrip;
    QPointer<QLabel> m_modeHint;
    /// The shape to arm when Region is chosen on the strip: whichever was used
    /// last, so the choice survives leaving the mode and coming back.
    RegionMode m_lastRegionShape{ RegionMode::Box };
    /// Guard against the strip and the actions driving each other in circles.
    bool m_syncingModeStrip{ false };

    /// Populate the right Inspector shell (Properties/Analysis). The image is a
    /// single-view window, so pane-layout and Linked are disabled.
    void setupInspector();
    /// Reparent any stray direct-child widget of the window (not central/dock/
    /// toolbar/menu/status) into the hidden holder so it can't paint at (0,0).
    void parkOrphanDirectChildren();

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
    QPointer<QAction> m_actFilterFits;
    QPointer<QAction> m_actExportToWorkspace;
    QPointer<QAction> m_actExportRegions;
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
    /// True once the user has typed a bound. Until then the bounds follow the
    /// image's scalar range on every pipeline rebuild (there is one after each
    /// preview -> full swap); afterwards the user's numbers are kept.
    bool                     m_contourBoundsUserSet{ false };
    void setupContourPipeline();
    void updateContours();
    void setContoursVisible(bool vis);
    /// Accept a pair of contour bounds if it is usable, otherwise keep the last
    /// accepted pair and put it back in the fields.
    void commitContourBounds(double lower, double upper);
    /// Write the accepted bounds into the Lower / Upper fields (signal-blocked).
    void syncContourBoundFields();
    /// The CONTOURS block in the left dock: re-homes the already-wired Level /
    /// Lower / Upper widgets, which otherwise live only in the Tools page that
    /// the Inspector's Analysis tab superseded (and which is never shown).
    QWidget *buildContourControlsBlock();
    /// Built once, parked hidden, mounted in the Inspector's Parameters tab for
    /// as long as the contour tool is on.
    QPointer<QWidget> m_contourSettingsBlock;
    /// Same for the polarisation-vector overlay: grid step, SNR threshold and
    /// length scale, shown while the vectors are on.
    QWidget *buildPolVectorSettingsBlock();
    QPointer<QWidget> m_polVecSettingsBlock;
    /// Band filter of the catalogue overlay, rebuilt whenever the band set
    /// changes and mounted as that tool's settings.
    QPointer<QWidget> m_vlkbBandContent;
    /// Tools whose settings are currently claimable, most recently switched on
    /// LAST. Turning one off must hand the panel back to whichever is still on,
    /// not blank it: contours on, vectors on, vectors off used to leave the
    /// contours running with no way to change their levels.
    QList<QPair<QString, QPointer<QWidget>>> m_activeToolSettings;
    void claimToolSettings(const QString &toolName, QWidget *settings);
    void releaseToolSettings(const QString &toolName);
    void refreshToolSettingsPanel(bool reveal);
    QPointer<QCheckBox> m_contourShowCheck;
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
    // Full-resolution pixel coordinate of the probe (== displayed voxel × spacing);
    // drives the readout + change-detection so coordinates update at full-res
    // granularity even when the displayed image is decimated (LOD).
    std::array<int, 2> m_probeFullPixel{ -1, -1 };
    std::array<int, 2> regionAnchorVoxel{ -1, -1 };
    std::array<int, 2> regionCurrentVoxel{ -1, -1 };
    std::vector<std::array<int, 2>> regionPolygonVertices;
    std::vector<std::array<double, 2>> savedLinearDisplayRanges;
    std::vector<CatalogueOverlayEntry> catalogueOverlayEntries;
    std::vector<std::array<double, 2>> catalogueOverlayPixels;
    std::vector<std::vector<std::array<double, 2>>> catalogueOverlayPolylines;
    std::vector<std::array<unsigned char, 3>> catalogueOverlayPolylineColors;  ///< per-polyline RGB
    std::vector<QString> catalogueOverlayPolylineBand;   ///< per-polyline group tag (VLKB band)
    QSet<QString> m_vlkbHiddenBands;                      ///< bands toggled off
    void toggleVlkbBand(const QString &band, bool visible);   ///< show/hide a band's sources
    void ensureVlkbBandPanel();                          ///< dock with a checkbox per present band
    std::vector<int> catalogueOverlaySourceFirstPolyline;
    std::vector<int> catalogueOverlaySourcePolylineCount;
    std::vector<int> catalogueOverlayLabelIndices;
    QStringList catalogueOverlayLabels;
    QString catalogueOverlaySummary;
    /// Sky frame of the loaded overlay ("Galactic", "ICRS", …): the table's two
    /// sky columns are captioned from it.
    QString catalogueOverlayFrameLabel;
    /// The loaded catalogue's own field names, in file order.
    QStringList catalogueOverlayExtraFields;
    /// True while a selection change originated in the table, so the sync back
    /// does not scroll the view the user is clicking in.
    bool m_catalogueSelectionFromTable{ false };
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
    // Region export (CRTF / DS9) — save the active drawn ROI. Loading region
    // files (incl. CRTF) is handled by loadCatalogueOverlay (single loader).
    QJsonObject currentRegionToJson() const;            ///< active drawn ROI → canonical dict (1-based px)
    void exportRegionsToFile();
    bool finalizePolygonRegion();
    void updateLargeImageMode(vtkImageData *imageData, const char *context);
    void loadCatalogueOverlay();
    void crossmatchCatalogue();          // query an external catalogue over the image footprint
    void onCrossmatchReady();            // display the async cross-match result as an overlay
    void applyCatalogueOverlayResult(const CatalogueOverlayParseResult &parsed);
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

    // ── VLKB catalogue overlays (compact sources / filaments over the footprint) ──
    void overlayVlkbSources();     ///< btnSources — band-merged compact sources (+ band picker)
    void overlayVlkbFilaments();   ///< btnFilaments — filament outlines as polylines
    void overlayVlkbBubbles();     ///< bubble outlines as closed polylines
    /// Tools ▸ VLKB Catalogue Query… — the legacy's query composer, prefilled
    /// with this image's own Galactic footprint.
    void openVlkbQueryComposer();
    void onVlkbCatalogueReady();   ///< project the raw Galactic result → overlay
    /// Project a Galactic (glon,glat) position to full-res image pixels via the
    /// window's WCS (handles a non-Galactic native frame by converting first).
    bool galacticToImagePixel(double glon, double glat, std::array<double, 2> &pixel) const;
    /// Galactic bounding box (deg) of the current image footprint, for the query.
public:
    /// Outline the sky polygon of an inventory dataset on this image.
    ///
    /// `skyCorners` are native-frame (Galactic, for VLKB) degrees. Projected
    /// with the same call the catalogue overlay uses, so the outline lands in
    /// the same place its sources do — which matters because Hi-GAL cutouts are
    /// CAR and a gnomonic projection would put it somewhere else.
    void showInventoryFootprint(const QVector<QPointF> &skyCorners);
    /// Remove it (a branch row selected, or the inventory closed).
    void clearInventoryFootprint();

protected:
    bool imageGalacticFootprint(double &glonMin, double &glonMax,
                                double &glatMin, double &glatMax) const;
    QFutureWatcher<VlkbCatalogueRaw> m_vlkbCatWatcher;
    /// Progress affordance for the VLKB query. A status-bar line at the very
    /// bottom of a full-screen window is not feedback: the query takes
    /// seconds, nothing moves, and the overlay looks like it did nothing.
    QPointer<QProgressDialog> m_vlkbProgressSheet;
    /// Which VLKB catalogue the pending query is for. Sources are ellipses;
    /// filaments and bubbles are both contours, and differ only in the table
    /// they come from and in whether the contour closes.
    enum class VlkbOverlayKind
    {
        Sources,
        Filaments,
        Bubbles,
    };
    VlkbOverlayKind m_vlkbCatKind{ VlkbOverlayKind::Sources };
    /// True for the two contour catalogues — the shape the pending result carries.
    bool vlkbCatIsContour() const { return m_vlkbCatKind != VlkbOverlayKind::Sources; }
    static QString vlkbCatalogueNoun(VlkbOverlayKind kind);
    // Rubber-band region select: drag a rectangle to query only that area (like the
    // legacy client); a click without a drag falls back to the whole image footprint.
    void beginVlkbRegionSelect(VlkbOverlayKind kind);
    void endVlkbRegionSelect();
    void onVlkbSelectPress();
    void onVlkbSelectMove();
    void onVlkbSelectRelease();
    void runVlkbCatalogueQuery(double glonMin, double glonMax, double glatMin, double glatMax);
    void runVlkbCatalogueQueryWholeImage(VlkbOverlayKind kind);   ///< click-without-drag fallback
    bool m_vlkbSelectActive{ false };
    bool m_vlkbSelectDragging{ false };
    QString m_vlkbBand{ QStringLiteral("250") };
    int m_vlkbSelStart[2]{ 0, 0 };
    int m_vlkbSelEnd[2]{ 0, 0 };
    unsigned long m_vlkbPressTag{ 0 };
    unsigned long m_vlkbMoveTag{ 0 };
    unsigned long m_vlkbReleaseTag{ 0 };
    vtkSmartPointer<vtkActor2D> m_vlkbRubberActor;
    /// Outline of the inventory row the user last clicked, drawn over the image
    /// so you can see WHERE a dataset falls before downloading it.
    vtkSmartPointer<vtkActor2D> m_inventoryFootprintActor;
    vtkSmartPointer<vtkInteractorObserver> m_vlkbPrevStyle;   ///< restored after selection

    // Stack
    vtkNew<vtkImageStack> stack;
    LayerListModel *layers;
    std::unique_ptr<ImageLayerController> layerController;
    void applyLoadedLayer(const ImageLayerLoadResult &result);
    // preserveView: keep the current camera + large-image mode (viewport-LOD
    // reload of a sub-region); default resets the camera to fit (initial load).
    void applyRemoteMasterLayer(const ImageLayerLoadResult &result, bool preserveView = false);
    void startRemoteFullResolutionLoad();
    void markDisplayStateAdjusted(const char *reason);
    bool isBusy() const;
    void showPersistentStatusMessage(const QString &text, int minDurationMs = 400);
    void clearPersistentStatusMessage();
    void setLayerImportEnabled(bool enabled);
    int currentLayerIndex() const;
    /// Re-elide the LAYER SETTINGS caption for the width and font it has NOW.
    void elideLayerSettingsHeader();
    bool eventFilter(QObject *watched, QEvent *event) override;
    /// Tools ▸ Smooth / Regrid… — the legacy's *Filter FITS*, acting on this
    /// window's image and returning the result as a layer over it.
    void openFilterFitsDialog();

    /// Tell the user why a layer was not added — non-modally, so no nested
    /// event loop runs while the pending-load queue is being put right. When
    /// the file simply belongs elsewhere, this is also where the choice between
    /// a window of its own and adding it anyway is offered.
    /// @param displayName  the label this load carried, so "Add Anyway" can put
    ///                      it back rather than adding an unnamed layer
    /// @param partOfSet     whether it belonged to an all-or-nothing set
    void showLayerLoadFailure(const ImageLayerLoadResult &result, const QString &displayName,
                              bool partOfSet);

    /// @param ignoreOverlap add the layer even if it covers none of this
    ///        image's sky — the user's answer to the refusal, since the sky
    ///        bounds behind it are a bounding box and can be too narrow.
    void addLayerImage(const std::string &filepath, bool ignoreOverlap = false);
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
    /// Moved off the Data Hub's Science menu: single-map tools, resolved by the
    /// host against this window's own dataset.
    /// Members only so imageTools() can list them: they were locals, which is why
    /// they existed in the Tools menu and in no other surface.
    /// One row per tool: the group it belongs to and the action that runs it.
    /// The Tools menu and Inspector ▸ Analysis are both built from this, so a
    /// tool cannot appear on one surface and be missing from the other.
    struct ImageTool
    {
        QString group;
        QAction *action{ nullptr };
    };
    /// Register what a tool just produced, so it survives the window it was
    /// shown in. See vtkWindowImage_Products.cpp for the three kinds.
    /// @p layerIndex >= 0 binds the product to a layer of this window.
    QString registerAnalysisProduct(const QString &kind, const QString &label,
                                    const QVariantMap &params,
                                    const QString &stableId,
                                    const QVariantMap &resultView = {},
                                    int layerIndex = -1,
                                    const QVariantMap &origin = {});
    /// Redraw the region a product was measured on. Returns false when the
    /// product carries no geometry.
    bool restoreProductOrigin(const QVariantMap &origin);
    /// The mapping displayed-voxel -> world for the master layer as it is shown
    /// right now (world = pos + voxel * scale). Recorded with a region so the
    /// overlay can be put back on a different LOD. False if there is no image.
    bool currentVoxelFrame(double scale[2], double pos[2]) const;
    /// Product id scoped to this file AND this window, so two windows on the
    /// same image do not collide.
    QString scopedProductId(const QString &stem) const;
    /// Put a product back on screen: select its layer, open its file, or replay
    /// its recorded result window.
    void openProduct(const QString &productId);
    void showSessionRowContextMenu(const QString &viewTag, const QString &productId,
                                   const QPoint &globalPos);

    QVector<ImageTool> imageTools() const;
    /// Rebuild the Tools menu from imageTools(), grouped into submenus.
    void regroupImageToolsMenu();

    /// productId → the layer it produced, for the products that are layers.
    /// The cross-match request that produced the pending result: the finished
    /// handler is generic, so its product would otherwise have no parameters.
    QString m_crossmatchCatalogue;
    double  m_crossmatchRadiusArcsec{ 0.0 };
    int     m_crossmatchRunCount{ 0 };
    void recordCrossmatch(int matches);

    /// Collapse the time axis of a dynamic spectrum into a spectrum. Only
    /// meaningful in dynspec mode; a sky image has no spectral axis.
    void extractSpectrum2D();
    int m_extractedSpectrumCount{ 0 };
    /// The curve behind each extracted-spectrum product, so reopening its row
    /// replots it instead of showing only the summary.
    QHash<QString, QPair<QVector<double>, QVector<double>>> m_extractedSpectra;

    QHash<QString, int> m_productLayer;
    /// Layer display name per product: the row index above is not an identity,
    /// the layer list can be reordered by drag and drop.
    QHash<QString, QString> m_productLayerName;
    /// Current row of the layer a product produced, or -1. Resolves by name and
    /// falls back to the recorded row.
    int productLayerRow(const QString &productId) const;
    QPointer<QAction> m_actHistogram;
    QPointer<QAction> m_actAddText;
    QPointer<QAction> m_actAddArrow;
    QPointer<QAction> m_actClearAnnot;
    QPointer<QAction> m_actSaveAnnot;
    QPointer<QAction> m_actLoadAnnot;
    QPointer<QAction> m_actPublicationFigure;
    QPointer<QAction> m_actImageQuality;
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
    /// Result of the companion directory listing, which runs OFF the GUI thread:
    /// BackendClient is synchronous, and a remote backend that is slow to answer
    /// would otherwise freeze the window inside a nested event loop.
    struct StokesListing
    {
        QSet<QString> names;
        QString error;
    };

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

    QPointer<QAction> m_actExportCatalogue;
    QPointer<QAction> m_actVlkbSources;
    QPointer<QAction> m_actVlkbFilaments;
    QPointer<QAction> m_actVlkbBubbles;
    QPointer<QAction> m_actVlkbQuery;
    QPointer<QAction> m_actLoadStokesCompanions;
    QPointer<QAction> m_actComputeStokesP;
    QPointer<QAction> m_actComputeStokesPA;
    QPointer<QAction> m_actComputeStokesFrac;
    QPointer<QAction> m_actComputeStokesPdebiased;
    QPointer<QAction> m_actComputeSpectralIndex;
    QPointer<QAction> m_actComputeFaradayRM;
    QPointer<QAction> m_actTogglePolVecs;

    void detectAndLoadStokesCompanions();
    QFutureWatcher<StokesListing> m_stokesListingWatcher;
    void onStokesListingReady(const StokesListing &listing);
    /// Queue of (path, "Stokes Q"-style label) waiting for the single layer
    /// loader. Loading is asynchronous through one watcher, so companions have
    /// to go in one at a time or all but the last are silently dropped.
    /// One layer waiting for the single layer-load pipeline.
    struct PendingLayerLoad
    {
        QString path;
        QString displayName;
        /// True for a member of an all-or-nothing set (the Stokes companions):
        /// if one fails the rest are abandoned, because half a Stokes set is
        /// worse than none and the user cannot see which half they got. False
        /// for independent layers — six VLKB cutouts are six answers, and one
        /// failing is no reason to drop the other five.
        bool partOfSet{ false };
        /// Set by the "Add Anyway" answer to a refused layer: the queue is the
        /// only way back in when another load started while the dialog was up.
        bool ignoreOverlap{ false };
    };
    /// Used by the Stokes companion sequence and by a multi-dataset VLKB
    /// download, which would otherwise drop every layer after the first — the
    /// viewer loads one at a time, and "busy" used to mean "discarded".
    QList<PendingLayerLoad> m_pendingLayerLoads;
    /// The failure message currently on screen. A batch of cutouts can fail one
    /// after another, and one dialog per failure buries the window.
    QPointer<QMessageBox> m_layerFailureDialog;
    /// Failures not shown because that dialog was already up.
    int m_suppressedLayerFailures{ 0 };
    /// Whether the load currently running belongs to such a set.
    bool m_currentLoadPartOfSet{ false };
    /// True while the companion search is inside a nested event loop (backend
    /// listing, file chooser), which leaves the menu action clickable.
    bool m_stokesDiscoveryActive{ false };
    void startNextQueuedLayerLoad();
    /// Start the next queued layer IF nothing is loading. Called wherever the
    /// viewer stops being busy — the layer loader, the preview and full-res
    /// image loads, the Stokes listing — because a cutout that arrived during
    /// any of those was queued and then waited for a pump that never came.
    void pumpLayerQueue();
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
    /// False when there is nothing valid to draw (missing or mismatched Q/U):
    /// the caller must then not switch the overlay on.
    bool rebuildPolarizationVectors();
    void setPolVectorsVisible(bool vis);
    double beamAreaPixels() const;
    bool hasBeamInfo() const;
    QString stokesRoleLabel(StokesRole r) const;
};

#endif

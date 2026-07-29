#ifndef vtkWindowCube_H
#define vtkWindowCube_H

#include "CatalogueOverlayUtils.h"
#include "CubeOpenPreviewTask.h"
#include "LutScale.h"

// Forward-declared here; full headers are included in vtkWindowCube.cpp where
// they are constructed and their methods are called.
class CubeUiAssembler;
class WorkspaceChrome;
class MomentMapController;
class NoiseController;
class PvController;

#include "CubeViewportGeometry.h"

#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>
#include <vtkNew.h>
#include <vtkScalarBarActor.h>

#include <QFutureWatcher>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QList>
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
class QActionGroup;
class QMenu;
class QShortcut;
class SidebarPanel;
class CatalogueTableModel;
class QDockWidget;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;
class QSplitter;
class SegmentedToggle;
class QHBoxLayout;
class QCheckBox;
class QComboBox;
class QJsonObject;
class QJsonArray;
class QNetworkAccessManager;
class QNetworkReply;
class QStackedWidget;
class QVTKOpenGLNativeWidget;
class QToolButton;
class RemoteRenderWidget;
class LUTCustomizerDialog;
class ProfileWidget;
class PvDiagramWidget;
class ToolResultDialog;
class ChannelOverlay;
class QTableView;
class vtkAxisActor2D;
class vtkActor;
class vtkCubeAxesActor;
class vtkGPUVolumeRayCastMapper;
class vtkColorTransferFunction;
class vtkContourTriangulator;
class vtkCoordinate;
class vtkFlyingEdges2D;
class vtkFlyingEdges3D;
class vtkGenericOpenGLRenderWindow;
class vtkRenderWindowInteractor;
class vtkCubeSource;
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
class vtkTexture;
class vtkTrivialProducer;
class vtkVolume;

class CubeVRController;

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
    // Sanitization already performed on the worker thread: NaN voxels
    // replaced with invisibleSentinel; cubeRange is the finite-only visible range.
    double invisibleSentinel{ -1.0 };
    // Fraction of NaN/inf voxels found in the subvolume (0..1). Cached so the
    // sanity panel does not need to rescan the full cube on the GUI thread.
    double blankFraction{ 0. };
};

// Kinematic-model overlay (F2). The model cube fetch, the float→vtkImageData
// copy and the FlyingEdges contour all run on a worker thread; `actor` is the
// ready-to-add overlay (null when `empty`), so the GUI thread only calls
// AddActor + Render. `empty` = model peak ≤ 0 (no emission landed on a channel).
struct RemoteKinematicModelResult
{
    bool valid{ false };
    bool empty{ false };
    QString errorMessage;
    vtkSmartPointer<vtkActor> actor;
    // Kept client-side so appearance (contour level, colour, opacity,
    // representation) can be re-applied live from the control panel WITHOUT a
    // re-fetch: `image` is the model cube (display coords), `contour` its
    // FlyingEdges filter (re-`SetValue` to change the level), `peak` scales it.
    vtkSmartPointer<vtkImageData> image;
    vtkSmartPointer<vtkFlyingEdges3D> contour;
    double peak{ 0. };
    double fraction{ 0.5 };
    qint64 numVertices{ 0 };
    QString spectralMode;
    QString spectralUnit;
};

/// GUI-side result of a Kinematic Lasso selection (wraps the backend reply so the
/// cube header stays decoupled from BackendClient.h, like RemoteKinematicModelResult).
struct RemoteKinematicSelectResult
{
    bool valid{ false };
    QString errorMessage;
    int roiWidth{ 0 }, roiHeight{ 0 }, roiDepth{ 0 };
    int originX{ 0 }, originY{ 0 }, originZ{ 0 };
    int fullWidth{ 0 }, fullHeight{ 0 }, fullDepth{ 0 };
    int count{ 0 };
    int bbox[6]{ 0, 0, 0, 0, 0, 0 };
    QByteArray data;   // ROI binary mask (float32 0/1)
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
                  const QString &remoteBackendToken = QString(),
                  // Size-aware loading policy from the backend's OpenDataset
                  // response. ``"auto_full"`` keeps legacy behaviour
                  // (preview → auto requestHighResCube). ``"preview_only"``
                  // stops at preview and surfaces explicit user actions
                  // ("Load full resolution", "Crop full resolution") via
                  // the View menu / status-bar buttons. Default is
                  // "auto_full" so any pre-existing call site (and any
                  // backend older than the fields) stays unchanged.
                  const QString &recommendedLoadMode = QStringLiteral("auto_full"),
                  qint64 fileSizeBytes = 0,
                  qint64 fullResThresholdBytes = qint64(4) << 30,
                  QWidget *parent = nullptr);
    ~vtkWindowCube() override;
    void closeEvent(QCloseEvent *event) override;

    /// Set whether plots (PV Diagram, spectra) should use a dark background.
    /// Called once by MainWindow after construction based on Settings::getPlotTheme().
    void setDarkPlots(bool dark) { m_darkPlots = dark; }

    /// Raw BUNIT (brightness value unit, e.g. "Jy/beam") from the FITS header.
    /// Set once by MainWindow after construction; shown in the cursor readout.
    void setBrightnessUnit(const QString &unit) { m_brightnessUnit = unit; }
    /// Provide beam metadata (BMAJ/BMIN in degrees, BPA in degrees E of N).
    /// Call after construction; the beam ellipse is drawn in the 2D slice
    /// renderer's bottom-left corner. Pass NaN major to hide the indicator.
    void setBeamInfo(double majorDeg, double minorDeg, double paDeg);

    // ── Server-side rendering (SSR, Phase 2) ──────────────────────────────────
    // The 3D viewport can either run locally (the existing
    // QVTKOpenGLNativeWidget pipeline) or be served by an EGL-equipped
    // backend over a WebSocket. The choice is exposed in the View →
    // Rendering submenu (Auto / Local / Remote) and surfaced to the user
    // via a dismissible banner above the 3D view + a status-bar badge
    // showing the current mode and (when remote) the live FPS.
    enum class RenderMode { Auto, Local, Remote };

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
    void showLUTCustomizer3D();

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

    /// Emitted right after a successful Tools → Export … as FITS so any
    /// open Data Hub can refresh its Workspace Exports panel immediately
    /// (without waiting for the 4-second background poll). Carries no
    /// payload — listeners just re-issue a /v1/exports/list call.
    void workspaceExportsChanged();

    /// Emitted when the cube viewer produces a 2-D product (moment map or
    /// current slice) that an open image viewer can overlay as contours.
    /// @p label  Human-readable name (e.g. "Moment 0", "Slice CH 42").
    /// @p data   The 2-D vtkImageData (caller retains ownership).
    void contourDataReady(const QString &label, vtkImageData *data);

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
    /// Stable key for persisting per-dataset analysis parameters (the FITS file
    /// path — unlike the session-scoped backend dataset id, it's the same across
    /// sessions). See AnalysisParamStore.
    QString datasetPersistKey() const { return this->filepath; }

    enum class RegionMode
    {
        None,
        Box,
        Circle,
        Polygon,
        Annulus,
    };

public slots:
    // Live noise-region preview overlays, used while the NoiseRegionDialog
    // is open (and kept on through the result dialog) so the user can see
    // exactly where the noise was sampled — relative to the data footprint
    // on the slice AND relative to the spectral range inside the volume.
    //
    // The slot paints two things in pixel coordinates:
    //   - on the 2D slice: an amber outline rectangle (x0..x1, y0..y1)
    //   - in the 3D view : a semi-transparent amber box that spans
    //     (x0..x1, y0..y1, chStart..chEnd), i.e. uses the channel range
    //     as its Z extent (the volume's Z axis IS the channel index in
    //     this viewer's coordinate system).
    //
    // Driven by NoiseRegionDialog::regionPreviewChanged.
    void setNoiseRegionPreview(int x0, int x1, int y0, int y1,
                               int chStart, int chEnd);
    void clearNoiseRegionPreview();

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
    WorkspaceChrome *m_workspaceChrome = nullptr;   ///< shared command-bar + status-rail chrome
    QPointer<QSplitter> m_paneSplitter;             ///< central 3D+2D pane container (design #1d)
    QPointer<QSlider> m_cuttingPlaneOpacitySlider;  ///< re-hosted into the flat LAYERS section
    // Step 1 — centre toolbar + per-pane headers.
    QPointer<QFrame> m_pane3dFrame;                  ///< [header + 3D view] wrapper (active border)
    QPointer<QFrame> m_pane2dFrame;                  ///< [header + 2D view] wrapper
    QPointer<QLabel> m_pane3dDot;                    ///< active indicator dot (3D header)
    QPointer<QLabel> m_pane2dDot;                    ///< active indicator dot (2D header)
    QPointer<QHBoxLayout> m_pane3dHeaderControls;    ///< where setupRenderModeUi() drops Auto/Local/Remote
    int m_activePane{ 0 };                           ///< 0 = 3D, 1 = 2D
    // Focus mode (§4.2): remember the dock pair before collapsing so leaving it
    // restores exactly the previous state (it is a toggle, not a mode).
    bool m_focusPrevSessionCollapsed{ false };
    bool m_focusPrevInspectorCollapsed{ true };
    QPointer<QWidget> m_pane3dHeader;                ///< 3D header widget (click → active)
    QPointer<QWidget> m_pane2dHeader;                ///< 2D header widget
    QPointer<QToolButton> m_paneLinkedBtn;           ///< toolbar Linked chip (synced with m_actLinkViews)
    void setupPaneChrome();                          ///< centre toolbar + per-pane headers (Step 1)
    void setActivePane(int pane);
    void refreshPaneActiveStyles();

    // ── §3.1 pane model: 4 slots, a view registry, one reconcile ──────────────
    enum class ViewId { None, View3D, Slice2D, Moment0, Spectrum };
    struct PaneSlot
    {
        QPointer<QFrame> frame;          ///< header + view, carries the active border
        QPointer<QWidget> header;        ///< click → active
        QPointer<QLabel> dot;            ///< active indicator
        QPointer<QToolButton> titleBtn;  ///< plain text @ 1 pane, dropdown @ >1
        QPointer<QLabel> qualifier;      ///< mono meta (e.g. "volume")
        QPointer<QWidget> viewHost;      ///< the mounted view is reparented in here
        QHBoxLayout *viewHostLayout{ nullptr };
        QHBoxLayout *controls{ nullptr };   ///< per-view control slot in the header
        QPointer<QToolButton> maxBtn;    ///< ⤢ maximize / ⤡ restore (§3.2)
        ViewId view{ ViewId::None };
    };
    std::array<PaneSlot, 4> m_slots;
    int m_paneCount{ 1 };                 ///< 1 / 2 / 4 (spec §3.1 default is 1)
    QPointer<QSplitter> m_leftColumn;     ///< slot 0 (top) + slot 2 (bottom)
    QPointer<QSplitter> m_rightColumn;    ///< slot 1 (top) + slot 3 (bottom)
    QPointer<QWidget> m_viewParkingLot;   ///< hidden holder for off-screen views
    // 2-pane direction: Auto derives from the container width; Rows/Cols is the
    // user's explicit override (second click on the "2" glyph), persisted (§3.1).
    enum class PaneDirection { Auto, Rows, Cols };
    PaneDirection m_paneDirection{ PaneDirection::Auto };
    QPointer<QWidget> m_momentEmptyState;    ///< shown in a Moment0 slot with no computed map
    QPointer<QWidget> m_spectrumEmptyState;  ///< shown in a Spectrum slot with no probe yet
    QWidget *emptyStateWidget(ViewId id);    ///< lazily builds the "what's missing + action" block
    bool viewHasData(ViewId id) const;       ///< false → mount the empty state, not the render
    QPointer<SegmentedToggle> m_paneCountToggle;
    QPointer<SegmentedToggle> m_pick3d2d;
    QPointer<SegmentedToggle> m_renderModeSeg;   ///< Auto/Local/Remote — re-hosted into the View3D slot
    QPointer<QLabel> m_linkedQualifier;          ///< "camera · channel · LUT" (narrows at 4 panes)

    QWidget *viewWidget(ViewId id) const;      ///< the (always-existing) widget for a view
    QString viewTitle(ViewId id) const;
    void buildPaneSlots();                     ///< construct the 4 slot frames + columns
    void assignViews(const std::array<bool, 4> &visible);   ///< fill empty VISIBLE slots (§3.3)
    void clearViewFromOtherSlots(int keepSlot, ViewId v);   ///< enforce one-slot-per-view
    bool paneStacked() const;   ///< 2-pane direction: explicit override, else width-derived
    bool m_restoringLayout{ false };           ///< suppress saveLayoutSettings during restore
    int firstVisibleSlot() const;
    void mountView(int slot);                  ///< reparent the slot's view out of the parking lot
    void parkView(int slot);                   ///< reparent a slot's view into the parking lot
    void refreshPaneChrome();                  ///< dots, titles, qualifiers, per-view controls
    void refreshPaneCursors();                 ///< arrow on non-active 2D data area, crosshair on active (§3.2)
    void syncPick3d2d();                        ///< keep the 1-pane 3D/2D toggle in step with slot 0's view
    // Fullscreen ⤢ (§3.2): maximize one pane WITHIN the workspace (no showFullScreen).
    // We remember the pre-maximize pane count + active pane and just toggle frame /
    // column visibility, so restore re-applies the saved arrangement with no re-render.
    int m_maximizedPane{ -1 };                 ///< -1 = not maximized, else the slot index
    int m_preMaxPaneCount{ 1 };
    int m_preMaxActive{ 0 };
    QPointer<QShortcut> m_escRestoreShortcut;  ///< enabled only while maximized (§3.2)
    // Click-to-activate (§3.2): the whole gesture that activates a non-active 2D pane
    // is swallowed (press + release + the trailing double-click), so an armed tool
    // fires only on the NEXT, separate click — not on the activating click.
    QPointer<QWidget> m_paneActivationGesture;
    void maximizePane(int slot);
    void restorePaneMaximize();
    void saveLayoutSettings();                 ///< §4.3 (layout_v2/cube/)
    void restoreLayoutSettings();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;   ///< pane-header click → active
    // Former SidebarPanel pages, migrated into the Inspector's Properties tab
    // (design #1d — no separate rail in the central widget). Built in
    // setupSidebar(), reparented in setupInspector().
    QPointer<QWidget> m_hiddenPageHolder;  ///< hidden container so no page is a free child of the window
    QPointer<QWidget> m_inspectorPage3d;
    QPointer<QWidget> m_inspectorPage2d;
    QPointer<QWidget> m_inspectorPageInfo;
    QPointer<QWidget> m_orphanToolsPage;   ///< kept alive, not shown (Analysis tab supersedes it)
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

    QPointer<LUTCustomizerDialog> lutCustomizer;   ///< 2D slice/moment LUT editor
    QPointer<LUTCustomizerDialog> lutCustomizer3D; ///< 3D volume LUT editor
    QPointer<ProfileWidget> profileWidget;
    QPointer<ProfileWidget> probePlotWidget;
    // Floating channel scrubber anchored at the bottom of the 3D viewport.
    // Created as a child of ui->vtkCube; positioned via an installed event
    // filter on resize. See ChannelOverlay.h.
    QPointer<ChannelOverlay> channelOverlay;
    QPointer<PvDiagramWidget> pvDiagramWidget;
    // Active Region Analysis result dialog. Tracked so the 2D region overlay
    // can persist for as long as the user is still inspecting the analysis
    // — see maybeClearRegionAfterDisplay().
    QPointer<ToolResultDialog> m_regionResultDialog;
    QPointer<QLabel> cubeOpenStateLabel;
    QPointer<QLabel> hoverReadoutLabel;
    QPointer<QLabel> momentProvenanceLabel;
    QPointer<QLabel> catalogueInfoLabel;
    QPointer<QDockWidget> catalogueDock;
    QPointer<QTableView> catalogueTableView;
    QPointer<CatalogueTableModel> catalogueTableModel;
    bool syncingCatalogueSelection{ false };
    // ── Plot theme ───────────────────────────────────────────────────────────
    bool m_darkPlots{ true };

    // ── Sidebar ───────────────────────────────────────────────────────────────
    SidebarPanel *m_sidebar{ nullptr };
    QPointer<QAction> actionShowControlsSidebar;
    // Owns all sidebar widget pointers and the four page-build methods.
    std::unique_ptr<CubeUiAssembler> m_uiAssembler;

    /// Lives across "Open in VR" clicks so the action can act as a toggle:
    /// first click opens, second click (while still in the headset) closes.
    /// Persists between sessions so we can detect ``isActive()`` cheaply.
    QPointer<CubeVRController> m_vrController;
    void setupSidebar();

    // Layout B-style top toolbar: path-pill + tag chips + Find/Command/Export.
    void setupViewerToolbar();

    // Opens a non-modal dialog showing the FITS header cards for `filepath`.
    // Triggered by the primary tag chip in the viewer toolbar.
    void showFitsHeaderDialog();

    // ── 3D View Settings – VTK-side state (UI widgets are in m_uiAssembler) ──
    QString             m_activeColorMapName;
    // Menu actions for volume blend mode (created programmatically).
    QPointer<QAction>   m_actionComposite;
    QPointer<QAction>   m_actionMIP;
    QPointer<QAction>   m_actionMinIP;
    QPointer<QMenu>     m_menuVolumeRendering;
    /// Update only the volume color transfer function for a named LUT.
    /// Reads the scalar range from the current cube data (requires applyCubeOpenResult
    /// to have been called first). Does NOT Render and does NOT touch the combo box.
    void applyVolumeLutToPipeline(const QString &name);
    /// Update the LUT combo (silently) and apply to the VTK pipeline. No Render.
    void setVolumeLut(const QString &name);
    /// Full user-interaction path: update state, apply pipeline, Render.
    void applyColorMapByName(const QString &name);
    void applyVolumeBlendMode(int idx);  // 0=Composite, 1=MIP, 2=MinIP
    void updateVolumeModeVisibility(bool volumeActive);

    // ── Moment card state machine ─────────────────────────────────────────────
    // Must stay in sync with CubeUiAssembler::MomentCardData::State (same
    // enumerator values) — a static_assert in vtkWindowCube.cpp enforces this.
    enum class MomentCardState { NoMoment, Computing, Ready, Error };
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
    /// "Export Moment Map as FITS…" — enabled only once a moment is on
    /// screen (the controller pushes the first result). Stays enabled.
    QPointer<QAction> m_actExportMomentFits;
    QPointer<QAction> m_actExportSubCube;
    QPointer<QAction> m_actMaskRegion;
    QPointer<QAction> m_actExportChannel2d;
    QPointer<QAction> m_actSliceHistogram;
    QPointer<QAction> m_actSendSliceToImage;
    QPointer<QAction> m_actKinematicModel;
    QPointer<QAction> m_actExportMovie;
    QPointer<QAction> m_actChannelMaps;

    // Beam ellipse overlay (synthesised beam indicator in the 2D slice view).
    // Set via setBeamInfo() after construction; drawn as a filled ellipse in
    // the bottom-left corner of the slice renderer.
    double m_beamMajorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double m_beamMinorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double m_beamPaDeg{ 0.0 };
    vtkSmartPointer<vtkActor> m_beamActor;
    QPointer<QAction> actionLoadCatalogueOverlay;
    QPointer<QAction> actionShowCatalogueOverlay;
    QPointer<QAction> actionShowCatalogueLabels;
    QPointer<QAction> actionClearCatalogueOverlay;
    QFutureWatcher<CubeOpenStageResult> cubeOpenWatcher;
    QFutureWatcher<RemoteCubePreviewResult> remotePreviewWatcher;
    QFutureWatcher<RemoteCubeSubvolumeResult> remoteHighResCubeWatcher;
    QFutureWatcher<AsyncIsosurfaceResult> isosurfaceWatcher;
    QFutureWatcher<RemoteKinematicModelResult> m_kinematicWatcher;

    // ── Kinematic Lasso (interactive kinematics-aware 3-D selection) ─────────
    bool m_lassoActive{ false };
    // Selection fires on the LEFT double-click (left-drag is left free for rotation);
    // this tag holds the LeftButtonDoubleClickEvent observer.
    unsigned long m_lassoReleaseTag{ 0 };
    double m_lassoReach{ 8.0 };       ///< geodesic cost budget (selection reach)
    double m_lassoLambdaV{ 1.0 };     ///< velocity-axis weight (the kinematic knob)
    int m_lassoRoiRadius{ 0 };   ///< 0 = object-bounded grow (whole connected source)
    // Display geometry snapshot at the moment of the pick, so the preview places
    // the selection with the SAME mapping used to derive the seed (robust to the
    // display swapping mid-fetch). ROI displays have a non-zero origin + spacing 1
    // (world == full-res voxels); preview/full have origin 0 (world == display voxels).
    bool m_lassoSnapRoi{ false };
    double m_lassoSnapOrigin[3]{ 0., 0., 0. };
    int m_lassoSnapDim[3]{ 0, 0, 0 };
    QFutureWatcher<RemoteKinematicSelectResult> m_lassoSelectWatcher;
    vtkSmartPointer<vtkActor> m_lassoActor;
    vtkSmartPointer<vtkImageData> m_lassoImage;
    // The displayed selection's raw ROI mask (little-endian float32 0/1, x-fastest)
    // + its full-res voxel origin/dims — kept so Apply sends the EXACT mask shown
    // (foundation for per-channel 2-D refinement) and the 2-D overlay can slice it.
    QByteArray m_lassoMaskBytes;
    int m_lassoMaskOrigin[3]{ 0, 0, 0 };   ///< full-res voxel origin (x,y,z)
    int m_lassoMaskDims[3]{ 0, 0, 0 };     ///< ROI dims (w,h,d) = (x,y,z)
    double m_lassoDispOrigin[3]{ 0., 0., 0. };   ///< 3-D display origin of the ROI mask
    double m_lassoDispSpacing[3]{ 1., 1., 1. };  ///< 3-D display spacing (preview vs full)
    void rebuildLassoIsosurface(bool render = true);  ///< (re)build the green 3-D surface from m_lassoMaskBytes
    // ── Phase 2: per-channel 2-D mask refinement (draw a polygon on the channel map)
    bool m_lassoEditActive{ false };
    bool m_lassoEditAdd{ true };                  ///< true = add pixels, false = subtract
    unsigned long m_lassoEditClickTag{ 0 };
    unsigned long m_lassoEditMoveTag{ 0 };
    unsigned long m_lassoEditRightTag{ 0 };
    vtkWeakPointer<vtkRenderWindowInteractor> m_lassoEditIren;  ///< exact interactor the observers live on
    std::vector<std::array<int, 2>> m_lassoEditVerts;   ///< in-progress polygon (full-res x,y)
    vtkSmartPointer<vtkActor> m_lassoEditPolyActor;     ///< live rubber-band on the 2-D view
    QPushButton *m_lassoEditToggle{ nullptr };
    QPushButton *m_lassoEditAddBtn{ nullptr };
    QPushButton *m_lassoEditSubBtn{ nullptr };
    void setLassoEditActive(bool on);
    void onLassoEditClick();
    void onLassoEditMove();
    void commitLassoEditPolygon();
    void refreshLassoEditPreview();
    bool mapDisplayToFullVoxel(int dx, int dy, int &outX, int &outY) const;
    vtkSmartPointer<vtkActor> m_lassoSeedActor;   ///< marker at the clicked seed
    vtkSmartPointer<vtkTextActor> m_lassoHintActor;  ///< in-viewport "click a source" prompt
    void showLassoHint(bool on);                  ///< show/hide the call-to-action overlay
    QPointer<QAction> m_actKinematicLasso;
    int m_lassoLastSeed[3]{ -1, -1, -1 };         ///< primary picked full-res seed (z,y,x)
    bool m_lassoHasSeed{ false };
    // ⇧-click ADDs another source (union), ⌥-click CARVEs a region out. Each is (z,y,x).
    QVector<std::array<int, 3>> m_lassoAddSeeds;
    QVector<std::array<int, 3>> m_lassoSubSeeds;
    bool m_lassoDiscardResult{ false };           ///< discard the in-flight grow (cleared mid-fetch)
    bool m_lassoPendingRegrow{ false };           ///< a slider moved during a grow → re-grow on finish
    // Live refinement panel (built lazily on first selection).
    QPointer<QDockWidget> m_lassoPanel;
    QSlider *m_lassoReachSlider{ nullptr };
    QSlider *m_lassoLambdaSlider{ nullptr };
    QLabel  *m_lassoReachValue{ nullptr };
    QLabel  *m_lassoLambdaValue{ nullptr };
    QLabel  *m_lassoCountLabel{ nullptr };
    QPushButton *m_lassoIsolateBtn{ nullptr };    ///< Apply buttons — disabled while a grow is in flight
    QPushButton *m_lassoRemoveBtn{ nullptr };
    void setLassoApplyEnabled(bool on);           ///< gate Apply on the displayed selection being current
    vtkSmartPointer<vtkActor> m_lassoMask2dActor; ///< selection outline on the 2-D channel map
    void updateLassoMaskOverlay(bool render = true);  ///< redraw the 2-D overlay for the current channel
    void onLassoRelease();                        ///< selection handler (bound to the left double-click)
    void onLassoSelectReady();
    void requestLassoSelection();                 ///< (re)grow from the current seeds
    void ensureLassoPanel();
    void placeLassoSeedMarker(const double world[3]);
    void applyLassoMask(const QString &mode);     ///< isolate/remove the selection → new cube
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
    // The shape of the last finalised region, kept after analysis sets
    // regionMode back to None (geometry + regionValid persist while the result
    // dialogs are open) so Region IO export still knows the shape to serialise.
    RegionMode m_exportableRegionShape{ RegionMode::None };
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
    QVariantMap m_momentProvenanceParams;   ///< key/value recipe → Inspector › Provenance (§3.0b)
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
    // Pattern B: backend-driven size-aware loading policy.
    // ``preview_only`` suppresses the auto requestHighResCube() call that
    // fires after the preview lands; the user must explicitly opt into
    // a full-resolution load (or ROI crop) via the View menu actions
    // populated by setupLoadModeUi(). ``auto_full`` is the legacy path.
    QString recommendedLoadMode{ QStringLiteral("auto_full") };
    qint64 datasetFileSizeBytes{ 0 };
    qint64 datasetFullResThresholdBytes{ qint64(4) << 30 };
    bool userForcedFullResolution{ false };
    // True for exactly ONE upcoming applyRemoteSliceResult, set by
    // requestRemoteSlice() when the user explicitly asked for this
    // slice. Prevents cascading prefetch chains (each prefetched
    // slice triggering its own ±1 prefetches → wavefront expansion).
    bool prefetchAllowedForNextResult{ false };
    // Smart prefetch: instead of firing ±1 immediately on every
    // user-initiated slice apply, schedule it for ``idlePrefetchDelayMs``
    // later and CANCEL the timer whenever another slice request
    // arrives. End result:
    //   • Rapid scroll through many channels → zero prefetch (each
    //     new request cancels the prior schedule before it fires)
    //   • A real "stop" (user found their channel) → ±1 prefetch
    //     500 ms later, ready for fine-tuning ± a channel
    // Net win: no tunnel-saturation during scrubbing, neighbour
    // caching kicks in once the user actually settles.
    QTimer idlePrefetchTimer;
    static constexpr int idlePrefetchDelayMs = 500;
    static constexpr int remoteLoadingStateDelayMs = 250;
    // Slider drag fires QSlider::valueChanged on every pixel of motion.
    // With a 100 ms debounce, even the tiniest pauses during a normal
    // mouse drag (very common — human hand isn't perfectly steady)
    // each emit a slice POST. Empirically a 1-second drag triggers
    // 8-12 slice requests, all wasted except the last; the queue then
    // takes ~10 s to clear through the SSH tunnel (~2 MB/slice on the
    // wire), so the user perceives a multi-second lag.
    //
    // 300 ms is the human-noticed-pause threshold for direct-manipulation
    // UIs: a deliberate pause feels long, normal hand jitter does not.
    static constexpr int remoteSliceDebounceDelayMs = 300;
    bool remoteIsosurfaceRequestInFlight{ false };
    double inFlightRemoteIsosurfaceThreshold{ std::numeric_limits<double>::quiet_NaN() };

    std::array<double, 2> currentCubeVisibleRange{ 0., 0. };
    double currentCubeInvisibleSentinel{ -1.0 };
    // Fraction of NaN/inf voxels in the currently displayed cube. Populated
    // by applyCubeOpenResult from the worker-thread sanitization stats, and
    // consumed by updateSanityPanel so the panel never rescans the volume.
    double currentCubeBlankFraction{ 0. };
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
    // Region IO (CRTF/DS9) on the cube's 2-D view (vtkWindowCube_RegionIO.cpp).
    QPointer<QAction> m_actExportCubeRegion;
    QPointer<QAction> m_actImportCubeRegion;
    QJsonObject currentRegionToJson() const;   ///< active drawn ROI → canonical dict (1-based px)
    void exportRegionsToFile();
    void importRegionsFromFile();
    /// Map the first parsed canonical region (image-frame, 1-based px) onto the
    /// cube's region model, draw it and run region analysis.
    void applyImportedRegion(const QJsonArray &regions);
    /// Clear the 2D region overlay once both result-dialog and spectrum plot
    /// have been closed. Wired to QObject::destroyed of both widgets.
    void maybeClearRegionAfterDisplay();
    /// Tools → "Export Sub-Cube as FITS…". Opens a small dialog asking for
    /// X / Y / Z bounds (pre-filled from the current region if drawn, else
    /// full extent), then posts to /v1/cube/save_subregion. Async — uses
    /// QtConcurrent to keep the UI responsive while the backend writes.
    void exportSubCubeAsFits();
    /// Tools → "Mask 3-D Region…". Selects a 3-D box (same dialog as the sub-cube
    /// export) + an isolate/remove choice, then posts to /v1/cube/mask to blank
    /// inside/outside the box, keeping full dimensions + WCS, and registers the
    /// masked cube as a new session dataset (the van der Hulst 3-D-selection-
    /// coupled-to-analysis use case). Async; shows the box in the 3-D volume.
    void maskCubeRegion3d();
    /// Tools → "Kinematic Lasso". Toggles the click-to-select mode: a click in the
    /// 3-D view ray-casts a seed voxel and the backend grows a kinematics-aware
    /// geodesic selection, shown as a green isosurface. See vtkWindowCube_Lasso.cpp.
    void setKinematicLassoActive(bool active);
    void clearKinematicLassoSelection();
    /// Tools → "Export Current Channel as 2-D FITS…". Writes the currently
    /// displayed channel as a true 2-D FITS image (NAXIS=2, no degenerate
    /// spectral axis) — the canonical input for the Stokes / spectral-index
    /// / Faraday-RM workflows in the image viewer. Async via
    /// /v1/cube/save_channel_2d.
    void exportCurrentChannelAs2dFits();
    /// Tools → "Pixel Histogram…". Opens a floating QCustomPlot window with
    /// a 256-bin histogram of the **current 2-D slice** pixel values. Two
    /// draggable cursors (red = low, green = high) live-clip the slice LUT
    /// range. Re-opening on a different channel rebuilds against the new
    /// slice data — useful for identifying multi-plane RM products (peak
    /// pol intensity vs RM vs frac pol) at a glance.
    void showSliceHistogramPanel();
    /// Tools → "Export Moment Map as FITS…". Re-runs the moment with the
    /// last accepted config and persists it as a 2-D FITS via
    /// /v1/products/moment_fits. Async, same pattern as above.
    void exportMomentMapAsFits();
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
    /// The 2-D render window / host widget currently shown in the 2-D slot. Since
    /// §3.0 slice and moment have SEPARATE host widgets (m_vtkImage / m_vtkMoment),
    /// so "the current 2-D window" is a function of the mode, not `ui->vtkImage`.
    vtkRenderWindow *active2dWin() const;
    QVTKOpenGLNativeWidget *active2dWidget() const;
    /// The 2-D gutter for a given view, with the colorbar column width MEASURED from
    /// the LUT's actual extremes (§3.0b) so the labels never clip. Shared by the fit,
    /// the colorbar and the WCS overlay so all three agree on where the data ends.
    CubeGeom::ImageGutter gutter2dFor(bool slice) const;
    /// Device-pixel ratio of the host widget for a 2-D view — the gutter and
    /// colorbar live in device px, so every authored (logical) number scales by it.
    double dprFor(bool slice) const;

    // ── Loading placeholders (VTK 2D text actors shown while data is in flight) ─
    vtkNew<vtkTextActor> m_cube3dLoadingActor;   ///< Shown in 3D view until first preview
    vtkNew<vtkTextActor> m_slice2dLoadingActor;  ///< Shown in 2D view until first slice
    bool m_initialThresholdApplied{ false };     ///< Guard: set threshold only on first load
    /// Update UI widgets + VTK pipeline without triggering user-interaction signals.
    /// Does NOT call Render() — caller is responsible for the next render.
    void applyThreshold(float value);
    /// Update only the VTK pipeline objects (opacity TF, isovalue) for a given threshold.
    /// Requires currentCubeVisibleRange to be valid. Does NOT Render.
    void applyThresholdToPipeline(double threshold);

    // Cube
    vtkNew<vtkLookupTable> lutVolume;  ///< tracks 3D color-map state; kept in sync by applyVolumeLutToPipeline
    vtkNew<vtkTrivialProducer> cubeDisplaySource;
    vtkNew<vtkFlyingEdges3D> isosurfaceFilter;
    vtkNew<vtkActor> isosurface;
    vtkSmartPointer<vtkActor> currentIsosurfaceActor;
    // Kinematic-model overlay (F2). The contoured tilted-ring model actor added
    // over the cube renderer; null when no overlay is shown.
    vtkSmartPointer<vtkActor> m_kinematicModelActor;
    // Model cube + contour filter kept client-side so the live control panel can
    // re-style (level / colour / opacity / representation) without re-fetching.
    vtkSmartPointer<vtkImageData> m_kinematicModelImage;
    vtkSmartPointer<vtkFlyingEdges3D> m_kinematicContour;
    double m_kinematicModelPeak{ 0. };
    // Live display geometry (dims + origin) snapshotted when an async model
    // build is launched. The result is registered against this geometry, so if
    // the display is reloaded (preview→full-res / ROI) while the worker runs,
    // the finished handler detects the mismatch and discards the stale actor.
    std::array<int, 3> m_kinematicLaunchDim{ 0, 0, 0 };
    std::array<double, 3> m_kinematicLaunchOrigin{ 0., 0., 0. };
    // Live overlay control panel (dock), created lazily and shown while an
    // overlay exists; its widgets mutate the actor/contour directly.
    QPointer<QDockWidget> m_kinematicPanel;
    QPointer<QSlider> m_kinOpacitySlider;
    QPointer<QSlider> m_kinLevelSlider;
    QPointer<QComboBox> m_kinRepCombo;
    QPointer<QCheckBox> m_kinVisibleCheck;
    QPointer<QCheckBox> m_kinShow2dCheck;
    // Model contour drawn on the 2-D channel map (sliceWin) for the displayed
    // channel — the SlicerAstro / ³ᴰBarolo model-vs-data diagnostic.
    vtkSmartPointer<vtkActor> m_kinematic2dContourActor;
    /// Prompt for tilted-ring parameters and launch the (async) model build +
    /// contour off the GUI thread (see vtkWindowCube_Kinematic.cpp).
    void showKinematicModelDialog();
    /// Apply a finished async kinematic-model build: add the overlay actor.
    void onKinematicModelReady();
    /// Build (once) the live overlay control panel and wire its widgets.
    void ensureKinematicPanel();
    /// Apply the panel's representation choice (wireframe/surface) to the actor.
    void applyKinematicRepresentation();
    /// Redraw the model contour on the 2-D channel map for the current channel
    /// (no-op / removed when disabled or no overlay). Called by the slice-apply
    /// path on channel change and by the panel controls.
    void updateKinematic2dContour(bool render = true);
    /// Remove the kinematic-model overlay actor and hide the panel.
    void clearKinematicModelOverlay();

    // Movie export (F3): capture an animation (channel scan or camera orbit) to
    // an MP4 via ffmpeg, or a PNG frame sequence when ffmpeg is unavailable.
    bool m_exportMovieInProgress{ false };
    /// Prompt for movie options and run the capture/encode (vtkWindowCube_Movie.cpp).
    void showExportMovieDialog();

    // Linked views (F3): sync camera / channel / colour-scale across all open
    // cube viewers that have "Link views" enabled (vtkWindowCube_Link.cpp).
    static QList<QPointer<vtkWindowCube>> s_cubeWindows;   // every open cube viewer
    bool m_linkViews{ false };
    bool m_applyingLinkedUpdate{ false };                  // echo guard
    int m_lastLinkedChannel{ -1 };                         // dedup for channel broadcasts
    unsigned long m_cubeCameraObserverTag{ 0 };       // InteractionEvent (drag)
    unsigned long m_cubeCameraEndObserverTag{ 0 };    // EndInteractionEvent (wheel zoom / release)
    // The exact interactor the camera observer was added to, kept weakly so the
    // observer is removed from the right object (and only if still alive).
    vtkWeakPointer<vtkRenderWindowInteractor> m_cubeCameraInteractor;
    QPointer<QAction> m_actLinkViews;
    /// Toggle this viewer's participation in the linked-views group.
    void setLinkViews(bool on);
    /// Camera-interaction observer on the 3-D view → broadcast to linked peers.
    void onCubeCameraInteraction();
    void broadcastLinkedCamera();
    // Camera is shared RELATIVE to each cube's own data bounds (direction,
    // view-up, and zoom/pan as fractions of the data diagonal), so cubes of
    // different size/extent stay correctly framed on their own data.
    void applyLinkedCamera(const double dir[3], const double up[3], const double relOffset[3],
                           double relDist, double relScale, double viewAngle);
    void broadcastLinkedChannel(int channel0);
    // Channel sync for heterogeneous cubes: match by SPECTRAL VALUE (velocity)
    // when the units match AND the velocity falls within the peer's range (same
    // object → physical alignment); otherwise map PROPORTIONALLY by channel
    // fraction (unrelated cubes → both scan their full range together).
    void applyLinkedChannel(int channel0, double fraction, double spectralValue,
                            bool hasSpectral, const QString &spectralUnit);
    // Shared colour: colormap name + volume stretch (scale mode + gamma). The
    // table RANGE is intentionally NOT shared — it is data-dependent and each
    // cube is auto-ranged to its own data.
    void broadcastLinkedColour();
    void applyLinkedColour(const QString &name, int scaleMode, double gamma);
    // Shared volume blend mode (0=Composite, 1=MIP, 2=MinIP).
    int m_volumeBlendMode{ 0 };
    void broadcastLinkedBlendMode();
    void applyLinkedBlendMode(int idx);
    vtkNew<vtkVolume> volume;
    vtkNew<vtkColorTransferFunction> volumeColorTransferFunction;
    vtkNew<vtkPiecewiseFunction> volumeOpacity;
    vtkSmartPointer<vtkGPUVolumeRayCastMapper> volumeMapper;  // stored for blend-mode changes
    vtkNew<vtkPlaneSource> remoteCuttingPlaneSource;
    vtkNew<vtkActor> remoteCuttingPlaneActor;
    // Texture sampling the 2D slice colors (sliceColors output) mapped onto
    // the cutting plane in the 3D view, so the plane shows the actual slice
    // content instead of a plain green fill.
    vtkSmartPointer<vtkTexture> remoteCuttingPlaneTexture;

    // ── 3D View options (configurable via the View menu) ─────────────────────
    QPointer<QAction> actionShowCuttingPlane;       // toggle plane visibility
    QPointer<QAction> actionShow3dWcsAxes;          // toggle WCS axis labels
    QPointer<QAction> actionPlaySlices;             // play/pause slice loop
    QPointer<QAction> actionPickSpectrum3d;         // pick spectrum on plane click
    QActionGroup    *cuttingPlaneOpacityGroup{ nullptr };
    QActionGroup    *animationFpsGroup{ nullptr };
    QActionGroup    *animationModeGroup{ nullptr };
    enum class AnimationMode { Loop, Bounce, Once };
    AnimationMode animationMode{ AnimationMode::Loop };
    int animationFps{ 5 };
    int animationDirection{ +1 };
    QTimer sliceAnimationTimer;
    void setCuttingPlaneOpacity(double opacity);
    void advanceSliceAnimation();
    void setSliceAnimationActive(bool active);
    void apply3dWcsAxesVisibility(bool visible);
    void refresh3dWcsAxes();   // re-fits ranges/labels from current cube + WCS
    vtkSmartPointer<vtkCubeAxesActor> wcs3dAxesActor;
    // Pick handler tag on the cube interactor (so we can disconnect cleanly).
    unsigned long cube3dPickObserverTag{ 0 };
    void install3dPickObserver();
    void remove3dPickObserver();
    void handle3dPickEvent();
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

    // Histogram window (singleton, lives until the user closes it)
    QPointer<QWidget> m_histogramWindow;

    // Colour-scale stretch (Linear/Log/Sqrt/Square/Power) tracked per display so
    // it survives reopening the LUT dialog and colormap swaps.
    LutScale::Mode m_sliceScaleMode{ LutScale::Mode::Linear };
    double m_sliceScaleGamma{ 2.0 };
    LutScale::Mode m_volumeScaleMode{ LutScale::Mode::Linear };
    double m_volumeScaleGamma{ 2.0 };

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
    // Noise-region preview overlay: separate set of line sources / actors so
    // it doesn't collide visually with an active Box-region drawing (which
    // uses sliceRegion*). Same pixel-coord convention; drawn in amber.
    // Two layers:
    //   * 2D slice  → four line sources outlining the (x,y) rectangle
    //   * 3D volume → vtkCubeSource bounded by (x,y, channels) plus a
    //                  semi-transparent face actor and a wireframe edge
    //                  actor that share the same data.
    vtkNew<vtkLineSource> noisePreviewTopLine;
    vtkNew<vtkLineSource> noisePreviewBottomLine;
    vtkNew<vtkLineSource> noisePreviewLeftLine;
    vtkNew<vtkLineSource> noisePreviewRightLine;
    vtkNew<vtkActor> noisePreviewTopActor;
    vtkNew<vtkActor> noisePreviewBottomActor;
    vtkNew<vtkActor> noisePreviewLeftActor;
    vtkNew<vtkActor> noisePreviewRightActor;
    vtkNew<vtkCubeSource> noisePreviewCubeSource;
    vtkNew<vtkActor>      noisePreviewCubeFaceActor;
    vtkNew<vtkActor>      noisePreviewCubeEdgeActor;
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
    unsigned long lastSliceOverlayCameraMTime{ 0 };   ///< re-run overlay on 2D zoom/pan
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
    unsigned long lastMomentOverlayCameraMTime{ 0 };   ///< re-run overlay on 2D zoom/pan
    bool showWcsAxes{ true };

    vtkNew<vtkScalarBarActor> m_sliceColorbar;    ///< 2D slice colorbar (right gutter)
    vtkNew<vtkScalarBarActor> m_momentColorbar;   ///< moment-map colorbar (right gutter)
    std::array<int, 2> m_slice2dFitSize{ -1, -1 };
    std::array<int, 2> m_moment2dFitSize{ -1, -1 };
    double m_slice2dFitRight{ -1. };    ///< gutter.right the slice was last fit for (§3.0b)
    double m_moment2dFitRight{ -1. };
    QString m_brightnessUnit;   ///< raw BUNIT for the cursor readout (may be empty)

    /// Aspect-preserving gutter fit + colorbar placement for the currently-visible
    /// 2-D pane. Cheap and idempotent: returns immediately unless the viewport
    /// changed size, so it cannot fight a user zoom/pan. Pass force = true after
    /// new data. Use for resize / mode-switch, where the visible pane IS the target.
    void layout2dPane(bool force = false);
    /// Same, but for an explicit pane (@p slice = true → slice, false → moment)
    /// regardless of which is currently shown — for data-arrival and setup, where a
    /// background slice result must not letterbox the moment camera and vice-versa.
    void layout2dPaneFor(bool slice, bool force);

    void applyMomentMapResult(const MomentMapApplyResult &result);
    bool isBusy() const;

    /// Register a derived product in the session-wide ProductRegistry so it
    /// appears nested under this dataset in the Session Data tree. @p window is
    /// the result window/widget to raise when the tree row is activated. When
    /// @p stableId is non-empty it is used as the product id, so re-computing an
    /// in-place product (e.g. the moment map) updates its row instead of adding
    /// a duplicate. Returns the assigned product id.
    QString registerAnalysisProduct(const QString &kind, const QString &label,
                                    const QVariantMap &params, QWidget *window,
                                    const QString &stableId = QString());

    /// Populate the right Inspector shell (Properties/Analysis tabs) and wire the
    /// pane-layout + Linked controls. Tools trigger the existing dialog QActions.
    void setupInspector();
    /// Build the FLAT left-dock controls (DISPLAY—3D / LAYERS / INTERACTION▶ /
    /// SLICE ANIMATION▶) by re-hosting the already-wired controls into flat rows
    /// — no bordered cards. sidebarCardStyle() is left intact for other viewers.
    QWidget *buildFlatLeftDockControls();
    /// Apply a saved-ish pane arrangement to the view docks (1/2/4).
    void applyPaneLayout(int panes);
    /// Reparent any stray direct-child widget of the window (not central/dock/
    /// toolbar/menu/status) into the hidden holder so it can't paint at (0,0).
    void parkOrphanDirectChildren();

    // ── Server-side rendering plumbing ────────────────────────────────────────
    // The 3D viewport widget is wrapped in a QStackedWidget with two pages:
    //   page 0 → ui->vtkCube                (Local — existing pipeline)
    //   page 1 → m_remoteRenderContainer    (Remote — banner + RemoteRenderWidget)
    // We swap the visible page on mode change; nothing else in the cube viewer
    // (slice fetch, moment compute, region tools, …) changes — slices and
    // products keep flowing over the existing REST endpoints, only the 3D
    // pixels switch transport.
    QStackedWidget       *m_renderStack{ nullptr };
    QPointer<QVTKOpenGLNativeWidget> m_vtkMoment;   ///< dedicated host for momentWin (§3.0)
    QWidget              *m_remoteRenderContainer{ nullptr };
    RemoteRenderWidget   *m_remoteRenderWidget{ nullptr };
    QLabel               *m_remoteRenderBannerLabel{ nullptr };
    QToolButton          *m_remoteRenderBannerClose{ nullptr };
    // Soft "Remote rendering recommended" banner shown on the Local
    // page when the cube exceeds LocalFullThresholdMb. Visibility is
    // toggled by ``updateLocalRecommendRemoteBanner``; the banner
    // never auto-switches, the user decides whether to take the
    // recommendation via its embedded "Switch to Remote" button.
    QFrame               *m_localRecommendRemoteBanner{ nullptr };
    QLabel               *m_localRecommendRemoteLabel{ nullptr };
    // Switch button is held as a member so it can be disabled while
    // the remote negotiation is in flight — prevents the rage-click
    // sequence we used to handle via just the m_remoteSwitchInProgress
    // flag (the flag was correct but invisible; users couldn't tell
    // the click had registered and kept clicking).
    QPushButton          *m_localRecommendRemoteSwitchBtn{ nullptr };
    // Reentrancy guard for ``applyRenderMode(Remote)``. The user can
    // rapid-click the "Switch to Remote" button during the 3-5 s
    // WebRTC negotiation; without this flag each click would issue
    // another /v1/render/create_session request (or worse, a duplicate
    // RemoteRenderWebRTCClient::connectToSession) and the pile-up
    // would crash inside libdatachannel on teardown of the orphans.
    bool                  m_remoteSwitchInProgress{ false };
    // Wall-clock when the user clicked "Switch to Remote". Drives the
    // progressive status message tier below (3-5 s → "Connecting",
    // 30 s+ → "Allocating on backend GPU", 1 min+ → "Large cube,
    // loading may take several minutes…"). Without this the banner
    // sits on a static "Connecting…" for minutes on a 22 GB cube
    // and the user can't tell whether the click registered.
    QElapsedTimer         m_remoteSwitchElapsed;
    QTimer               *m_remoteSwitchProgressTimer{ nullptr };
    QLabel               *m_renderModeBadge{ nullptr };  // status-bar pill
    QLabel               *m_renderFpsBadge{ nullptr };   // status-bar FPS
    QAction              *m_actionRenderAuto{ nullptr };
    QAction              *m_actionRenderLocal{ nullptr };
    QAction              *m_actionRenderRemote{ nullptr };
    RenderMode            m_renderMode{ RenderMode::Auto };
    RenderMode            m_effectiveRenderMode{ RenderMode::Local };
    QString               m_renderSessionId;  // valid only while in Remote mode
    QNetworkAccessManager *m_renderNam{ nullptr };
    // True once we have either confirmed that the backend can do SSR or
    // confirmed that it cannot — used to suppress repeat probes.
    bool                  m_renderBackendProbed{ false };
    bool                  m_renderBackendSupportsSsr{ false };
    // Phase-3 transport choice for the remote render widget. Populated
    // from /v1/render/diagnostics::transports the first time we enter
    // Remote mode. ``true`` means the backend exposes the H.264-WebRTC
    // path AND the client was built with VISIVO_HAS_WEBRTC.
    bool                  m_remoteBackendSupportsWebRTC{ false };

    void setupRenderModeUi();
    void applyRenderMode(RenderMode requested, bool fromUserAction);
    void requestRemoteRenderSession();
    void releaseRemoteRenderSession();
    /// Connect the RemoteRenderWidget against the given session,
    /// picking the best transport advertised by the backend. The
    /// diagnostics probe is cached: subsequent calls reuse the
    /// previously-discovered ``m_remoteBackendSupportsWebRTC`` flag.
    void connectRemoteRenderWidgetWithTransport(const QString &sessionId);
    void onRemoteRenderConnectionStateChanged(const QString &state);
    void onRemoteRenderFpsChanged(double fps);
    void updateRenderModeBadges();
    /// Push every visible piece of local volume-render state to the
    /// backend in one go — LUT, threshold, blend mode, cutting plane.
    /// Called once on Remote-mode entry (when the WS turns Live) so a
    /// fresh session is bootstrapped to the same look the user already
    /// sees locally. Cheap (5 JSON commands).
    void pushRenderStateToRemote();
    /// Send a single state-mutation to the remote renderer. No-op
    /// unless we're in Remote mode AND the widget is Live.
    void sendRemoteRenderStateCommand(const QVariantMap &command);
    /// Helper: re-push the current threshold (lowerBound) to the
    /// remote renderer if it has been initialised. Used to fix race
    /// conditions where a stateful command (blend mode, LUT) arrives
    /// before the backend has caught up to the user's slider value —
    /// the call is cheap and idempotent server-side.
    void resendThresholdToRemote();

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

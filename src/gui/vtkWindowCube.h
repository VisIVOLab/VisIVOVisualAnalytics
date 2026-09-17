#ifndef vtkWindowCube_H
#define vtkWindowCube_H

#include "BackendClient.h"
#include "CatalogueOverlayUtils.h"
#include "CubeOpenPreviewTask.h"
#include "LutScale.h"
#include "LinkedWindowRegistry.h"
#include "RemoteSliceCache.h"
#include "SpectralAxisTypes.h"

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
class QProgressDialog;
struct AnalysisProduct;
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
class QVBoxLayout;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QJsonObject;
class QJsonArray;
class QNetworkAccessManager;
class QNetworkReply;
class QStackedWidget;
class QVTKOpenGLNativeWidget;
class Pane2DView;
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
class vtkSphereSource;
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

    /// The cube's rest frequency in Hz (header RESTFRQ/RESTFREQ; 0 when absent),
    /// handed to every spectrum plot so a REST-frame line list can be placed on
    /// a velocity axis — impossible without it.
    void setRestFrequency(double restFreqHz);
    /// Whether the backend can render server-side at all (its EGL probe). A
    /// CPU-only or macOS backend cannot, and then offering "Switch to Remote"
    /// sends the user into a request the backend refuses.
    void setServerSideRenderingAvailable(bool available);
    /// (Re)compose the "full resolution is held back" banner: whether to show
    /// it at all, its text, its tooltip and which of its two actions apply.
    /// Called at setup AND whenever the backend's capability arrives, since the
    /// setter lands after the constructor has already composed it once.
    void refreshRemoteRecommendBanner();
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

    /// §5.2: the active pane changed (or its view kind changed) — the DISPLAY dock
    /// rebinds its section header + body to the newly-active pane.
    void activePaneChanged(int pane);

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

    /// Emitted when the copilot asks to SHOW source finding for this cube. The
    /// host (MainWindow) opens the SoFiA-2 dialog for this cube's dataset and
    /// overlays the resulting mask in 3D — reusing the interactive source-find
    /// path rather than duplicating it here.
    void sourceFinderRequested();

    /// A product that lives as a FITS FILE on the backend (a baseline-subtracted
    /// cube, a mask, a sub-cube) should be openable again from its Session Data
    /// row. The window itself cannot open a dataset — MainWindow owns that — so
    /// it asks.
    void openBackendFitsRequested(const QString &path, const QString &label);

public:
    enum class RemoteCubeDisplayState
    {
        Preview,
        LoadingFullResolution,
        FullResolution,
    };

    // Lifted to SpectralAxisTypes.h (E-04); kept accessible as
    // vtkWindowCube::SpectralAxisKind / ::SpectralAxisDescriptor via these aliases.
    using SpectralAxisKind = ::SpectralAxisKind;
    using SpectralAxisDescriptor = ::SpectralAxisDescriptor;

public:
    /// Stable key for persisting per-dataset analysis parameters (the FITS file
    /// path — unlike the session-scoped backend dataset id, it's the same across
    /// sessions). See AnalysisParamStore.
    QString datasetPersistKey() const { return this->filepath; }

    /// Build a product id scoped to THIS window: "<stem>@<datasetKey>#<windowScope>". The
    /// parentKey stays datasetPersistKey() (so the tree still nests under the dataset), but the
    /// id is unique per window so a second viewer on the same dataset never collides in the
    /// ProductRegistry. Every product-id construction goes through here.
    QString scopedProductId(const QString &stem) const;

    /// Copy a product's backend FITS into Workspace Exports (durable storage).
    /// Offered on the Session Data row of any product whose file lives in the
    /// backend's TEMP directory, which is where "and how do I keep it?" had no
    /// good answer.
    void saveProductCopyToWorkspace(const QString &backendDatasetId,
                                    const QString &label,
                                    const QString &sourcePath);

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
    /// Record who is currently describing the amber region overlay, and clear it
    /// only on that owner's behalf. Three tools draw it now (Estimate Noise,
    /// Export Sub-Cube, Mask 3-D Region) from three non-modal windows that can
    /// be open at once, so without this, closing any one of them wiped the box
    /// another was still showing.
    void claimRegionPreview(QObject *owner) { m_regionPreviewOwner = owner; }
    void clearRegionPreviewIfOwner(QObject *owner)
    {
        if (owner && m_regionPreviewOwner == owner)
            this->clearNoiseRegionPreview();
    }
    void clearNoiseRegionPreview();

private:
    struct MomentMapApplyResult
    {
        vtkSmartPointer<vtkImageData> imageData;
        std::array<double, 2> imageRange;
        int order{ 0 };   ///< moment order — scopes the product id so orders coexist (§3.2)
        // The recipe needed to re-export THIS moment (§3.2): with several moments cached,
        // export must use the DISPLAYED product's recipe, not the last-computed one.
        int channelStart{ 0 };
        int channelEnd{ 0 };
        bool maskEnabled{ false };
        double thresholdValue{ 0. };
        bool thresholdAuto{ false };
    };

    // §3.2 Increment B: retain every computed 2-D product's image so distinct
    // moments (Moment 0 AND Moment 1) coexist. The single moment pipeline
    // (momentDisplaySource) overwrites on each compute; this cache does not. Keyed
    // by the SAME productId the registry uses, so tree / dropdown / cache agree.
    // Whole-map statistics of a 2-D field (ignoring NaN/masked pixels). `valid` is false
    // when there are no finite pixels — every summary stat is then undefined (rendered "-").
    struct MapStats
    {
        bool valid{ false };
        long count{ 0 };
        double min{ 0. }, max{ 0. }, mean{ 0. }, median{ 0. }, std{ 0. }, rms{ 0. }, sum{ 0. };
    };
    static MapStats computeMapStats(vtkImageData *img);

    struct Product2D
    {
        vtkSmartPointer<vtkImageData> imageData;
        std::array<double, 2> imageRange{ 0., 0. };
        QString lutName;                                 ///< colormap (by kind: velocity → diverging)
        LutScale::Mode scaleMode{ LutScale::Mode::Linear };
        double gamma{ 2.0 };
        QString label;                                   ///< identical to the tree/dropdown label
        bool clampOutliers{ false };                     ///< velocity: hide out-of-range noise spikes (§3.2)
        // Export recipe — so "Export Moment as FITS" re-exports the DISPLAYED product,
        // not whatever was computed last (§3.2).
        int order{ 0 };
        int channelStart{ 0 };
        int channelEnd{ 0 };
        bool maskEnabled{ false };
        double thresholdValue{ 0. };
        bool thresholdAuto{ false };
        QString unit;                                    ///< physical unit (Inspector stats)
        MapStats stats;                                  ///< cached whole-map stats
        bool statsComputed{ false };                     ///< compute once (imageData is immutable)
    };
    QHash<QString, Product2D> m_products2d;

    // #5: a 1-D spectrum product (Pin Spectrum, region spectrum). Like Product2D but the
    // renderable payload is two parallel vectors rendered by a per-slot ProfileWidget — the
    // FIRST product kind that is NOT a 2-D image, so ViewId::Product now dispatches on the
    // product's renderer ("spectrum1d" → ProfileWidget, else "image2d"/"moment2d" → Pane2DView).
    struct Spectrum1D
    {
        QVector<double> key;      ///< spectral axis (velocity / frequency)
        QVector<double> values;   ///< intensity per channel
        QString xLabel, yLabel;   ///< axis titles
        QString label;            ///< identical to the tree/dropdown label
    };
    QHash<QString, Spectrum1D> m_products1d;
    std::array<class ProfileWidget *, 4> m_paneSpectra{ nullptr, nullptr, nullptr, nullptr };
    class ProfileWidget *ensureSlotSpectrumView(int slot, const QString &xLabel,
                                                const QString &yLabel);   ///< per-slot spectrum view

    // Spectrum controls migrated to Inspector › Analysis (they used to clutter each spectrum
    // pane). Built once, shown only while the ACTIVE pane is a spectrum product, and driven onto
    // that pane's chrome-less ProfileWidget. See buildSpectrumInspectorControls() +
    // updateSpectrumInspectorControls().
    void buildSpectrumInspectorControls(class InspectorPanel *panel);
    void updateSpectrumInspectorControls();   ///< retarget/refresh for the active pane
    QWidget       *m_spectrumInspector { nullptr };   ///< the whole section (hidden when N/A)
    QLabel        *m_specInspTitle     { nullptr };
    QLabel        *m_specInspStats     { nullptr };
    QComboBox     *m_specInspSmooth    { nullptr };
    QPushButton   *m_specInspFit       { nullptr };
    QPushButton   *m_specInspClearFit  { nullptr };   ///< remove the fitted-curve overlay
    QPushButton   *m_specInspLoadLines { nullptr };
    QPushButton   *m_specInspClearLines{ nullptr };
    QPushButton   *m_specInspPin       { nullptr };
    QPushButton   *m_specInspClearPins { nullptr };
    QPushButton   *m_specInspSaveCsv   { nullptr };
    QPushButton   *m_specInspSavePng   { nullptr };
    QPointer<class ProfileWidget> m_specInspTarget;   ///< the pane widget the controls drive

    // Full-resolution 3-D cube statistics from the backend (the local block is only the decimated
    // preview). Fetched once per open; the Inspector "3D cube" rows are refreshed when it lands.
    // (The result type stays in the .cpp so this header remains decoupled from BackendClient.h.)
    QPointer<QLabel> m_cubeStatMedian;
    QPointer<QLabel> m_cubeStatStd;
    QPointer<QLabel> m_cubeStatSum;
    bool m_cubeStatsRequested{ false };   ///< guard: fetch only once per window
    void fetchCubeStatsAsync();

    /// The renderer a product uses: "spectrum1d" (m_products1d) / "image2d" (m_products2d) / "".
    QString productRenderer(const QString &productId) const;
    /// True when @p productId is a product this window can mount in a pane — an image
    /// (m_products2d), a spectrum (m_products1d), or the PV diagram. Centralises the predicate
    /// used by activateSessionView / showProductInPane / the Show-in menu.
    bool isPaneRenderableProduct(const QString &productId) const;
    /// #5: capture the current probe spectrum as a pinned 1-D product (Pin Spectrum).
    void pinCurrentSpectrum();
    /// #5: promote a 2-D generator result (Line-width, …) to a first-class IMAGE product —
    /// cache it in m_products2d, register it (renderer "image2d"), and mount it in a free pane
    /// (no-steal), or refresh every visible copy on a recompute of the same stableKey.
    QString addImageProduct(const QString &kind, const QString &stableKey, const QString &label,
                            vtkImageData *img, const QString &unit, const QVariantMap &params,
                            const QString &lutName);
    /// #5: spectrum1d twin of addImageProduct — promote a 1-D generator result (Stack, …) to a
    /// first-class SPEC product: cache it in m_products1d, register it (renderer "spectrum1d"),
    /// mount it in a free pane (no-steal), or refresh every visible copy on a recompute.
    QString addSpectrumProduct(const QString &kind, const QString &stableKey, const QString &label,
                               const QVector<double> &key, const QVector<double> &values,
                               const QString &xLabel, const QString &yLabel,
                               const QVariantMap &params);
    // #5b: the live Extract-Spectrum preview renders in a PANE (transitory "Spectrum (live)"
    // product), NOT a floating window. Started/stopped with the probe tool; updated on hover.
    QString m_spectrumLiveId;   ///< the transitory live product id (empty when the tool is off)
    /// The current probed curve has already been promoted to a SPEC product by an
    /// explicit "Pin Spectrum", so ending the tool must not register it again.
    /// Cleared whenever updateProbePlot() writes a NEW curve.
    bool m_lastProbeSpectrumPinned{ false };
    /// Label of that product ("Spectrum 2"), so the pane can SAY the curve is
    /// already saved. Without it, ending the tool on an already-saved curve
    /// looked like the spectrum being thrown away.
    QString m_lastProbeSpectrumProduct;
    /// Write the live spectrum pane's state + the two ways out of the tool into
    /// its header (hover tracks / click pins / click again resumes).
    void updateLiveSpectrumHeader();
    void startLiveSpectrum();   ///< place a live spectrum pane + tree entry
    void stopLiveSpectrum();    ///< drop the live pane + tree entry
    void updateLiveSpectrumPane(const QVector<double> &key, const QVector<double> &values,
                                const QString &pixelLabel);
    // Last probe spectrum captured for pinning (filled on every probe update).
    QVector<double> m_lastProbeKey;
    QVector<double> m_lastProbeValues;
    QString m_lastProbeXLabel, m_lastProbeYLabel, m_lastProbeContext;
    int m_pinnedSpectrumCount{ 0 };   ///< suffix for the next Pin Spectrum product id/label
    int m_stackCount{ 0 };            ///< suffix for the next Stack SPEC product (each is distinct)

    void ensureMomentStats(Product2D &p);          ///< compute + cache the product's whole-map stats
    // The STATISTICS › 2D image block follows the ACTIVE 2-D pane (Slice or a Moment),
    // labelled by the pane, with the full stat set. There is ONE authoritative 2-D block —
    // no separate per-moment section duplicating the same numbers ((a)).
    void updateActiveMapStats();
    QPointer<QWidget> m_activeMapStatsSection;     ///< the single active-pane 2D stats block

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
    QPointer<QPushButton> m_animPlayBtn;    ///< re-hosted into SLICE ANIMATION
    QPointer<QComboBox>   m_animFpsCombo;
    QPointer<QComboBox>   m_animModeCombo;
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
    // §5.1b: probe / region / PV act on a spectral cube; a moment map is collapsed in
    // velocity, so those tools are disabled (with a reason tooltip) while a moment pane is
    // active, on TOP of their backend-derived enable state. Re-enabled for Slice / cube.
    void applyMomentToolGate();
    bool m_momentToolGated{ false };     ///< true while the moment gate is disabling the tools
    /// Re-entrancy guard: the gate un-arms checkable tools, whose toggled()
    /// handlers reset region / probe state and call the gate right back.
    bool m_inToolGate{ false };
    bool m_pvBaselineEnabled{ false };   ///< PV's backend-derived enable (restore target)
    bool m_cubeActionsEnabled{ false };  ///< probe/region baseline (cube open + usable)
    /// Session-tree row → active pane: if a visible pane already shows the view/product,
    /// make it active; otherwise mount it in the active pane. @p viewTag is
    /// "view3d"/"slice2d"; @p productId targets a derived product (empty for view rows).
    void activateSessionView(const QString &viewTag, const QString &productId);
    // "Show in ▸": right-click a product in the tree → build a menu of visible panes + New
    // pane, mount the product there (duplicable). Renderable products only.
    /// "Show in ▸" for any Session Data row: an intrinsic view or a product.
    void showSessionRowContextMenu(const QString &viewTag, const QString &productId,
                                   const QPoint &globalPos);
    void showProductInPane(const QString &productId, int slot);
    // Pop-out: the same pane frame moves to its own window; the grid slot shows a placeholder.
    void detachPane(int slot);
    void bringBackPane(int slot);
    bool paneDetached(int slot) const;
    // Remember each detached window's geometry (size/position/screen) for the session, so a
    // bring-back → re-detach re-opens where the user left it.
    std::array<QByteArray, 4> m_detachedGeom;
    QWidget *makeDetachPlaceholder(int slot);
    void refitDetachablePane(int slot);   ///< post-reparent render/refit per view kind
    void syncSessionTreeToActivePane();   ///< reverse sync: highlight the active pane's tree row
    void pushProductPaneBadges();         ///< (#4) which pane(s) show each product → tree badges
    void refreshPaneActiveStyles();
    void refreshPaneInteractors();   ///< §5.1: only the active pane's interactor is live

    // ── §3.1/§3.2 pane model: 4 slots, a view registry, one reconcile ─────────
    // A pane shows one of the fixed views (3D / 2D slice / spectrum) OR an arbitrary
    // computed Product (a moment map, …) identified by its registry productId — so
    // two different moments can occupy two panes (§3.2). `kind` alone drives every
    // fixed-view branch; `productId` is meaningful only when kind == Product.
    enum class ViewId { None, View3D, Slice2D, Spectrum, Product };
    struct PaneView
    {
        ViewId kind{ ViewId::None };
        QString productId;   ///< only for Product
        bool operator==(const PaneView &o) const
        {
            return kind == o.kind && productId == o.productId;
        }
        bool operator!=(const PaneView &o) const { return !(*this == o); }
        bool isNone() const { return kind == ViewId::None; }
    };
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
        QPointer<QToolButton> lutBtn;    ///< §5.4 header LUT strip → colormap popover (2D panes)
        QPointer<QToolButton> detachBtn; ///< pop-out to its own window / re-attach
        // Pop-out: the SAME frame (slot/product/LUT/badge preserved) lives temporarily in its
        // own top-level window; the grid keeps its place with a "Pane N · detached" placeholder
        // (no layout re-flow). Both null when the pane is docked in the grid.
        QPointer<QWidget> detachedWindow;
        QPointer<QWidget> placeholder;
        PaneView view;
    };
    std::array<PaneSlot, 4> m_slots;

    /// Mount @p view in @p slot, or in a new pane when @p slot < 0 (grows the
    /// layout rather than stealing an occupied pane).
    void showViewInPane(const PaneView &view, int slot);

    /// Views displaced by a transient tool result, keyed by the BORROWER's product
    /// id — never by slot index: a 4→1 collapse moves pane views between slots, so
    /// a slot key goes stale and the displaced view is lost.
    QHash<QString, PaneView> m_borrowedPanes;
    /// True while this pane holds a product that displaced another view.
    bool paneIsLent(int slot) const;
    /// Put @p productId where it can be SEEN: a free pane, else grow the layout
    /// (1→2→4), else — only when @p mayBorrow — borrow the least relevant pane and
    /// remember what it showed. Returns the slot, or -1 when there is no room (the
    /// product then waits in Session Data). Mounts the view itself.
    /// @p avoidActivePane keeps the focused pane out of the candidates — right for
    /// a live tool preview (the focused pane is the source being hovered), wrong
    /// for a finished product, which may perfectly well fill the empty pane the
    /// user is looking at.
    int acquirePaneForProduct(const QString &productId, bool mayBorrow,
                              bool avoidActivePane = true);
    /// Pick the pane cheapest to displace, recording its view. -1 if none may go.
    int borrowPaneSlot(const QString &productId);
    /// Give back every pane borrowed for @p productId.
    void releaseBorrowedPane(const QString &productId);

    /// The fixed 2-D slice / 3-D view is mounted in a VISIBLE pane. A tool binds
    /// to its view, so this — not which pane has the focus — is what gates it.
    bool slice2dVisible() const;
    bool view3dVisible() const;
    /// A view of @p kind is assigned to a slot, visible or not — the difference
    /// between "hidden by a maximize" and "not in the layout".
    bool viewAssigned(ViewId kind) const;
    /// A tool that draws on the 2-D slice / listens to the 3-D interactor is armed.
    bool anyTool2dArmed() const;
    bool anyTool3dArmed() const;
    /// This pane shows the view an armed tool is listening to. Such a pane keeps
    /// taking mouse input even when it is not the active pane — otherwise the
    /// user cannot select another pane (to reach its controls) without silently
    /// making the armed tool inert.
    bool paneIsArmedTarget(int slot) const;
    /// Un-arm every armed interaction tool. True if any was armed (Esc).
    bool disarmArmedTools();
    // §3.2 B3: each pane that shows a Product gets its OWN display-only Pane2DView
    // (independent camera / LUT / colorbar / WCS overlay), reading the shared per-product
    // imageData from m_products2d — so Moment 0 ∥ Moment 1 (and the same product twice)
    // render side by side without doubling the image memory. Lazily created per slot.
    std::array<Pane2DView *, 4> m_paneViews{ nullptr, nullptr, nullptr, nullptr };
    // The productId currently seeded into each slot's Pane2DView. lutInitialized() alone is
    // per-slot, so a slot reused for a DIFFERENT product would keep the old LUT; comparing
    // this reseeds the by-kind default on a product SWITCH while still keeping the pane's own
    // LUT on a same-product recompute (§5.3 / §5.4b).
    std::array<QString, 4> m_paneProductId;
    // §5.4b: a non-curated map picked via "More maps…" persists as a 7th entry (top of the
    // popover + dock combo) for THIS pane for the session, so it's re-selectable after
    // switching away. One per pane; a newer non-standard pick replaces it (never grows).
    std::array<QString, 4> m_paneCustomColorMap;
    Pane2DView *ensureSlotProductView(int slot);   ///< create + WCS-wire the slot's view

    // §5.2: the DISPLAY dock section follows the active pane. Page 0 = the 3D controls
    // (unchanged, holds m_uiAssembler/ui singletons); page 1 = the 2D/Moment pane page
    // (colormap + CARTA range presets + scale/symmetric) driving the active Pane2DView.
    QPointer<QPushButton> m_displayHeaderBtn;

    // LAYERS rows kept so updateDisplaySectionForActivePane() can show only the
    // ones that act on the pane in front of the user.
    QPointer<QWidget> m_layerRowVolume;

    /// LAYERS \u25b8 3D rendering: hides the volume/isosurface without touching
    /// the Isosurface/Volume mode. Applied by apply3dRenderingVisibility().
    bool m_show3dRendering{ true };
    QPointer<QWidget> m_layerRowCuttingPlane;
    QPointer<QWidget> m_layerRowCuttingPlaneOpacity;   ///< indented, follows the checkbox
    QPointer<QWidget> m_layerRowContours;
    QPointer<QWidget> m_layerRow3dWcsAxes;
    QPointer<QStackedWidget> m_displayStack;
    QPointer<QComboBox> m_paneLutCombo;
    QPointer<QDoubleSpinBox> m_paneRangeMin;
    QPointer<QDoubleSpinBox> m_paneRangeMax;
    QPointer<QWidget> m_paneScaleRow;      ///< 2D Slice only (linear/log/asinh)
    QPointer<SegmentedToggle> m_paneScaleSeg;
    QPointer<QWidget> m_paneSymmetricRow;  ///< Moment only ("Symmetric about 0")
    QPointer<QCheckBox> m_paneSymmetric;
    bool m_applyingPaneDisplay{ false };   ///< guard dock↔pane two-way binding
    QWidget *buildPaneDisplayPage();       ///< the 2D/Moment DISPLAY page (page 1)
    void updateDisplaySectionForActivePane();   ///< header + page + field values

    /// Install a data-derived log floor on lutSlice before a switch into
    /// Mode::Log. False when the image has no positive values, in which case the
    /// caller must not change the mode. See the definition.
    bool anchorSliceLogFloor(LutScale::Mode mode);

    /// One tool, as it appears everywhere the tools are offered.
    struct CubeTool
    {
        QString group;
        QPointer<QAction> action;   ///< carries its own text, tooltip and state
    };

    /// THE list of this window's tools, grouped. One table feeding two places —
    /// the Tools menu and the Inspector's Analysis tab — because they were
    /// hand-written lists that had already drifted: the menu carried 31 entries
    /// in a flat run and the Analysis tab 11 of them. Add a tool here and it
    /// appears in both. (A third copy lived in the left dock; it was removed —
    /// that panel is for how a view is DRAWN, not for what can be computed.)
    QVector<CubeTool> cubeTools() const;

    // Two Tools entries were created as locals; kept so cubeTools() can list
    // them alongside the rest.
    /// Tooltips of every gated tool as first seen, so the pane gate can put them
    /// back instead of clearing them (the 3-D-only tools, Pin Spectrum, Export
    /// Region, Send Slice to Image Viewer, Link Views — each carries an
    /// explanatory sentence the gate temporarily replaces with its reason).
    QHash<QAction *, QString> m_toolBaseTooltips;

    QPointer<QAction> m_actPinSpectrum;
    QPointer<QAction> m_actOpenInVR;

    /// Rebuild the Tools menu from cubeTools(), one submenu per group.
    void regroupToolsMenu();
    /// Rebuild the View menu in a deliberate order (Workspace / Display /
    /// Animation / Data, then the developer entries). Runs once, after every
    /// entry exists.
    void regroupViewMenu();


    /// Push m_show3dRendering onto both 3-D props and repaint.
    void apply3dRenderingVisibility();

    /// Cube channel containing world Z, or -1 when outside / unavailable.
    int channelAtWorldZ(double worldZ) const;

    /// Place / hide the sphere marking the last 3-D pick.
    void showPick3dMarker(const double world[3]);
    void hidePick3dMarker();

    /// March the click ray through the cube and return the voxel that produced
    /// the pixel, using the criterion implied by the current blend mode.
    /// False when the ray misses the cube or finds nothing above threshold.
    bool pickVolumeAlongRay(vtkRenderer *renderer, const int displayPos[2],
                            double worldOut[3], int &channelOut) const;

    /// Show the indented Opacity row only on the 3-D pane with the plane on.
    void updateCuttingPlaneOpacityRowVisibility();
    void showPaneLUTCustomizer();          ///< Edit → the active pane's own LUT
    Pane2DView *activePane2DView() const;  ///< m_paneViews[m_activePane] if it's a Product pane
    // §5.4 follow-up: the DISPLAY page drives EITHER a Product pane (its own Pane2DView) or
    // the 2D Slice (the shared lutSlice/m_sliceScaleMode). These dispatch on the active kind
    // so the page handlers and updateDisplaySectionForActivePane() stay agnostic.
    bool activePaneIsSlice() const;
    vtkImageData *activeDisplayImageData() const;
    QString activeDisplayColorMapName() const;
    LutScale::Mode activeDisplayScale() const;
    double activeDisplayRangeLo() const;
    double activeDisplayRangeHi() const;
    void setActiveDisplayColorMap(const QString &name);
    void setActiveDisplayScale(LutScale::Mode mode);
    void setActiveDisplayRange(double lo, double hi);
    bool activeDisplayInvert() const;
    void setActiveDisplayInvert(bool on);
    void resetActiveDisplayLut();               ///< §5.4 popover Reset → default LUT by kind
    void showActiveDisplayLUTEditor();
    void applyPaneDisplayRange(bool fromMin);   ///< dock spinbox → active pane setRange
    // §5.4: the header LUT strip is a shortcut button — click activates the pane + opens a
    // popover (colormap list + Edit LUT… · Invert · Reset), writing the same per-pane state.
    void refreshPaneLutStrip(int slot);         ///< update the strip swatch + visibility
    void showPaneLutStripPopover(int slot);     ///< the colormap popover for a pane
    void showMoreColorMapsDialog();             ///< §5.4b full-registry picker (minus MinMax)
    void flashPaneLutStrip(int slot);           ///< §5.5: ~400ms confirmation flash on a receiver
    bool m_sliceInvert{ false };                ///< slice LUT inverted (Pane2DView tracks its own)
    void propagatePaneLutWithinWindow(int srcSlot);   ///< §5.5 Linked·LUT: same-product panes (full)
    void propagatePaneRangeWithinWindow(int srcSlot); ///< §5.5: range only (cheap, for live drag)
    void propagate2DCamera(int srcSlot);              ///< Linked 2D: sync pan/zoom to same-WCS image panes
    int m_paneCount{ 1 };                 ///< 1 / 2 / 4 (spec §3.1 default is 1)
    QPointer<QSplitter> m_leftColumn;     ///< slot 0 (top) + slot 2 (bottom)
    QPointer<QSplitter> m_rightColumn;    ///< slot 1 (top) + slot 3 (bottom)
    QPointer<QWidget> m_viewParkingLot;   ///< hidden holder for off-screen views
    // 2-pane direction: Auto derives from the container width; Rows/Cols is the
    // user's explicit override (second click on the "2" glyph), persisted (§3.1).
    enum class PaneDirection { Auto, Rows, Cols };
    PaneDirection m_paneDirection{ PaneDirection::Auto };
    QPointer<QWidget> m_momentEmptyState;    ///< shown in a Product moment slot with no computed map
    QPointer<QWidget> m_spectrumEmptyState;  ///< shown in a Spectrum slot with no probe yet
    // A None pane shows a neutral "No product available" state. Unlike moment/spectrum
    // (one on screen at a time), SEVERAL panes can be None at 4-up, so this is per-slot
    // (a QWidget lives in one parent) rather than a shared singleton.
    std::array<QPointer<QWidget>, 4> m_noProductState;
    QWidget *noProductStateWidget(int slot);
    QWidget *emptyStateWidget(const PaneView &v);  ///< lazily builds the "what's missing + action" block
    bool viewHasData(const PaneView &v) const;     ///< false → mount the empty state, not the render
    QPointer<SegmentedToggle> m_paneCountToggle;
    QPointer<SegmentedToggle> m_pick3d2d;
    QPointer<SegmentedToggle> m_renderModeSeg;   ///< Auto/Local/Remote — re-hosted into the View3D slot
    QPointer<QLabel> m_linkedQualifier;          ///< "camera · channel · LUT" (narrows at 4 panes)
    QString m_momentPipelineProductId;           ///< which product's image is loaded in m_vtkMoment (§3.2 B2)
    QString m_lastComputedMomentProductId;       ///< most recently computed moment (may be off-screen, §3.2)

    QWidget *viewWidget(const PaneView &v) const;  ///< the render widget for a view (Product→moment pipeline in B2)
    QString viewTitle(const PaneView &v) const;
    QList<PaneView> availableViews() const;        ///< fixed views + every renderable product (dropdown order)
    void loadProductIntoMomentPipeline(const QString &productId, bool force = false);  ///< swap the single moment pipeline's image
    void buildPaneSlots();                     ///< construct the 4 slot frames + columns
    void assignViews(const std::array<bool, 4> &visible);   ///< fill empty VISIBLE slots (§3.3)
    void clearViewFromOtherSlots(int keepSlot);  ///< one slot per RENDER WIDGET (3D + shared 2D pipelines)
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
    /// Window activation is when the set of OTHER open windows can have changed
    /// unnoticed (an image viewer opened or closed), which gates Send Slice to
    /// Image Viewer — so the tool gate is re-run here rather than going stale.
    void changeEvent(QEvent *event) override;
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
    /// The backend is on THIS machine (loopback host). It changes what a
    /// recommendation to render remotely actually buys: no transfer is saved,
    /// only the client's own GPU memory.
    bool backendIsLocal() const;
    /// Path of the app's Settings.ini, for cheap raw QSettings reads/writes.
    /// NOT via Settings: that constructor launches python3 with 30-second
    /// synchronous waits to locate an interpreter, which must not happen on a
    /// UI path (main.cpp avoids it the same way for the theme).
    static QString settingsIniPath();
    /// Datasets for which the user dismissed the remote-rendering suggestion.
    static QStringList remoteRecommendDismissed();
    static void dismissRemoteRecommend(const QString &datasetKey);
    /// Key for the persisted dismissal: the dataset path is not enough, because
    /// the list is application-wide and the same path on two backends is two
    /// different cubes.
    QString remoteRecommendKey() const;
    /// Fetch the whole cube at full resolution (the tail of View ▸ Load Full
    /// Resolution, without its warning dialog — callers that have already told
    /// the user the cost use this).
    void startFullResolutionLoad();
    /// Fill in a baseline product once its result arrives: the new dataset id and
    /// the output path go into the params, so the row survives its dialog and can
    /// be reopened from Session Data.
    /// Double-click on a product with no window: reopen the FITS it left behind.
    void reopenProductFromFile(const QString &productId);
    /// Put a measurement's recorded numbers back on screen, rebuilt from the
    /// product's params. A statistics product has no file and no pane, so this
    /// is what "open" means for it.
    void showRecordedResult(const AnalysisProduct &product);
    void completeBaselineProduct(const QString &productId, const QString &newDatasetId,
                                 const QString &outputPath, const QString &summary);
    /// Undo a half-started switch to remote rendering (guard, ticker, buttons,
    /// mode badge) and say why in the banner.
    void abortRemoteSwitch(const QString &reason);
    /// True only across the connectToSession() call, whose internal reset emits
    /// a Disconnected that is not a failure.
    bool m_ignoreSyntheticDisconnect{ false };
    /// Bumped whenever a switch to remote rendering is cancelled or times out,
    /// so a create_session reply that lands afterwards knows it is stale.
    quint64 m_remoteSwitchGeneration{ 0 };
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
    // STEP6-P4: the shared LUT customizer edits EITHER lutSlice or lutMoment, decided at init
    // time. Its lutUpdated callback fires later (non-modal), when viewingSlice() may have changed,
    // so remember which LUT the dialog actually targets — only a SLICE edit broadcasts the slice
    // colour to linked peers.
    bool m_lutCustomizerTargetsSlice{ false };
    QPointer<LUTCustomizerDialog> lutCustomizer3D; ///< 3D volume LUT editor
    QPointer<LUTCustomizerDialog> m_paneLutCustomizer;  ///< §5.2: per-pane (Pane2DView) LUT editor
    QPointer<Pane2DView> m_paneLutTarget;               ///< the pane the dialog is editing
    QPointer<ProfileWidget> profileWidget;
    QPointer<ProfileWidget> probePlotWidget;
    // Floating channel scrubber anchored at the bottom of the 3D viewport.
    // Created as a child of ui->vtkCube; positioned via an installed event
    // filter on resize. See ChannelOverlay.h.
    QPointer<ChannelOverlay> channelOverlay;
    QPointer<PvDiagramWidget> pvDiagramWidget;
    // #5: the PV diagram is a first-class pane product (renderer "pvdiagram") — the single
    // reused PvDiagramWidget is HOSTED IN a pane (it renders its own position/velocity axes,
    // which Pane2DView's celestial WCS can't), not shown as a floating window. Non-empty while
    // a PV product exists; identifies the pv product id in productRenderer / viewHasData.
    QString m_pvProductId;
    // #5: per-window scope token (from ProductRegistry::newOwnerScope) — salts every product id
    // and tags each product's ownerId, so two viewers on the same dataset never collide and each
    // Session tree shows only its own products. See scopedProductId().
    QString m_windowScope;
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
    /// View ▸ Session Data Panel: the LEFT dock (views, products, display
    /// controls). Its counterpart below toggles the right-hand Inspector.
    QPointer<QAction> actionShowSessionPanel;
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
    /// SoFiA-2 source finding, requested from the host (which holds this
    /// window's backend context). Was reachable only from the Data Hub.
    /// The tilted-ring settings window and its progress sheet, kept alive across
    /// the build so the tool behaves like the rest.
    /// Held from the moment Overlay is pressed until the result has been
    /// HANDLED. QFutureWatcher::isRunning() goes false when the worker ends,
    /// which is earlier, and a click in between started a second run.
    bool m_kinematicBusy{ false };
    /// The two 3-D-box tool windows, one each, re-raised rather than duplicated,
    /// with a busy flag that lives HERE so closing and reopening a window cannot
    /// start a second run of the same tool.
    /// The statistics product registered most recently, so the result-window
    /// recipe that arrives immediately after can be attached to it.
    QString m_lastStatisticsProductId;
    /// Whoever last drew the amber region overlay (see claimRegionPreview).
    QPointer<QObject> m_regionPreviewOwner;
    QPointer<QDialog> m_subCubeDialog;
    QPointer<QDialog> m_maskRegionDialog;
    bool m_subCubeBusy{ false };
    bool m_maskRegionBusy{ false };
    QPointer<QDialog> m_kinematicDialog;
    QPointer<QProgressDialog> m_kinematicSheet;
    /// Edit ▸ Undo: reverses the copilot's last batch of DISPLAY changes. Enabled
    /// only while a snapshot exists to go back to.
    QPointer<QAction> m_actUndoCopilot;

    /// Members only so regroupViewMenu() can put the View menu in a deliberate
    /// order: the entries are created all over this file, in the order the code
    /// happens to run, which is how the menu ended up with twenty-seven entries
    /// in no order at all.
    QPointer<QAction> m_actFocusMode;
    QPointer<QAction> m_actLoadFullRes;
    QPointer<QAction> m_actCropFullRes;
    QPointer<QAction> m_actDiagnostics;
    QPointer<QAction> m_actCommandPalette;
    QPointer<QMenu> m_menuCuttingPlaneOpacity;
    QPointer<QMenu> m_menuAnimationSpeed;
    QPointer<QMenu> m_menuAnimationMode;
    QPointer<QAction> m_actSourceFind;
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
    void requestLassoGrow();                      ///< single-budget grow (level-field fallback)

    // ── Instant Reach scrubbing ──────────────────────────────────────────────
    // The lasso validation on two cubes showed the useful Reach depends on the
    // source's surface brightness, cannot be derived from the header, and so has
    // to stay a control the user drags (validation/README.md). Dragging it used
    // to cost one backend grow per step. Instead we fetch the geodesic LEVEL
    // FIELD once per seed/λv and threshold it locally: `add <= r` minus
    // `sub <= r`, which is exactly the voxel set a grow at r would return.
    BackendKinematicLevelsResult m_lassoLevels;   ///< cached field (invalid = none)
    // Identity the cache answers for. Tagged from the SNAPSHOT taken when the
    // request went out, never from the live members: the user can move a slider
    // or re-click while the fetch is in flight, and tagging with the live values
    // would label a stale field as current.
    double m_lassoLevelsLambdaV{ 0.0 };           ///< λv the cache was computed for
    QVector<std::array<int, 3>> m_lassoLevelsSeeds;      ///< seeds it was computed for
    QVector<std::array<int, 3>> m_lassoLevelsSubSeeds;
    int m_lassoLevelsSeed[3]{ -1, -1, -1 };       ///< primary seed it was computed for
    // The same three, snapshotted at request time and carried to the handler.
    double m_lassoLevelsReqLambdaV{ 0.0 };
    QVector<std::array<int, 3>> m_lassoLevelsReqSeeds;
    QVector<std::array<int, 3>> m_lassoLevelsReqSubSeeds;
    int m_lassoLevelsReqSeed[3]{ -1, -1, -1 };
    QFutureWatcher<BackendKinematicLevelsResult> m_lassoLevelsWatcher;
    /// Drop a displayed selection: mask bytes, 3-D surface, 2-D contour, 2-D edit
    /// mode, count label and Apply. Shared by the local and the backend "nothing
    /// selected" paths so an empty answer looks the same however it was reached.
    void clearPresentedLassoSelection(bool render);
    /// True when the cache answers for the CURRENT seeds and λv, so a Reach change
    /// needs no network round trip. λv changes the cost function itself, so it
    /// invalidates; Reach does not.
    bool lassoLevelsUsable() const;
    /// True when a hand-refined mask is on screen, so a Reach change would throw
    /// the user's 2-D edits away.
    bool m_lassoMaskHandEdited{ false };
    /// Outcome of a local threshold. ``Empty`` is NOT a failure: with a carve
    /// seed, ``(add <= r) & ~(sub <= r)`` can legitimately select nothing, and
    /// conflating that with ``NoCache`` would leave the previous surface on
    /// screen — and applyable — while the slider says otherwise.
    enum class LassoLevels { Shown, Empty, NoCache };
    /// Threshold the cached field at `reach` and show it.
    LassoLevels showLassoSelectionFromLevels(int reach, bool render = true);
    void onLassoLevelsReady();
    /// Shared tail of a grow and a local threshold: store the ROI mask, place it
    /// in display space, rebuild the isosurface. Extracted so the instant path and
    /// the network path can never diverge in how they present a selection.
    void presentLassoSelection(const RemoteKinematicSelectResult &r);
    /// Recompute the ROI mask's world placement from the CURRENT cube display.
    /// False when there is no display image to place it against.
    bool updateLassoDisplayPlacement();
    /// Re-place and redraw an already-shown selection after the cube display is
    /// swapped (preview ↔ full-res ↔ ROI). Re-places the existing mask rather than
    /// re-thresholding the level field, so 2-D hand refinements survive.
    void refreshLassoAfterDisplaySwap();
    void ensureLassoPanel();
    void placeLassoSeedMarker(const double world[3], bool render = true);
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
    /// Monotonic id source. Kept apart from currentRemoteSliceRequestId, which
    /// names the wanted result and can move backwards when an in-flight fetch is
    /// adopted; ids themselves must never be reused while a watcher holds one.
    int m_remoteSliceRequestIdSeq{ 0 };
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
    /// The loaded catalogue's own field names, in file order.
    QStringList catalogueOverlayExtraFields;
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
    // E-07/M6: request-scoped provenance of linked-originated slice fetches. The
    // m_applyingLinkedUpdate RAII guard is destroyed before an ASYNC slice
    // result returns, so we record the requestId of every linked-driven fetch
    // here and consume it (per request, not a global one-shot) when its result
    // arrives — robust against a user cached-scrub interleaving between a linked
    // request and its late result. m_applyingLinkedResult is set (RAII) only for
    // the synchronous span of applyRemoteSliceResult for such a result.
    QSet<int> m_linkedRemoteRequestIds;
    bool m_applyingLinkedResult{ false };
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
    vtkSmartPointer<vtkActor> m_pick3dMarkerActor;        ///< where the last 3-D pick landed
    vtkSmartPointer<vtkSphereSource> m_pick3dMarkerSource;
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
    static LinkedWindowRegistry<vtkWindowCube> s_registry;   // every open cube viewer (E-04 step 2)
    bool m_linkViews{ false };
    // §5.5: Linked is a MASTER with per-facet toggles rendered as two chips in the header
    // ("channel" · "LUT"). Each gates one propagation kind; both grey out when the master
    // is off.
    bool m_linkCamera{ true };   // STEP6-P5: gates ALL camera sync (2D panes + 3D, intra + cross)
    bool m_linkChannel{ true };
    bool m_linkLut{ true };
    bool m_linkRender{ true };    // gates the Volume↔Isosurface representation sync
    bool m_linkBlend{ true };     // gates the volume blend-mode (Composite/MIP/MinIP) sync
    bool m_linkNoReceiverHintShown{ false };   // "nothing to link" is one-shot per layout
    // STEP6-P2/P3: the shared Linked-2D view, stored GUTTER-INDEPENDENTLY (world point at the
    // data-area centre + world height filling it — see Pane2DView::cameraStateNormalized) so
    // panes with different colorbar gutters stay aligned. A pane that re-fits (range/LUT/resize)
    // re-adopts THIS instead of resetting to full-frame. Set from the pane last panned/zoomed.
    double m_linked2DCenter[2]{ 0., 0. };
    double m_linked2DWorldHeight{ 0. };
    bool   m_linked2DValid{ false };
    void reassertLinked2DCamera(int slot);     // re-apply the shared camera after a pane fit()
    // STEP6-P1: the singular 2-D SLICE viewport (ui->vtkImage) participates in the Linked-2D
    // camera too — it uses the SAME CubeGeom letterbox, so the gutter-independent math transfers.
    int  sliceSlot() const;                    // the visible slot showing Slice2D, or -1
    bool sliceCameraNormalized(double dataCenterXY[2], double &worldHeight) const;
    void applySliceCameraNormalized(const double dataCenterXY[2], double worldHeight,
                                    bool render = true);
    void onSliceCameraInteraction();           // slice interactor pan/zoom → propagate
    void captureSliceCamPose();                // record the slice camera's pan/zoom as "seen"
    // STEP6-P1: last-seen slice camera PAN/ZOOM pose (focal x/y + parallel scale). Tracking the
    // POSE — not the raw MTime — means a window/level LEFT-drag (no pan/zoom) or a bare
    // ResetCameraClippingRange (kinematic/lasso overlays) does NOT look like a camera move, so
    // onSliceCameraInteraction only propagates on a real pan/zoom. Seeded by the initial
    // fit + every programmatic slice-camera set.
    double m_sliceCamFocalX{ 0. };
    double m_sliceCamFocalY{ 0. };
    double m_sliceCamScale{ 0. };
    QPointer<QToolButton> m_linkCameraChip;    // per-facet toggle (camera)
    QPointer<QToolButton> m_linkChannelChip;   // per-facet toggle
    QPointer<QToolButton> m_linkLutChip;       // per-facet toggle
    QPointer<QToolButton> m_linkRenderChip;    // per-facet toggle (Volume/Isosurface)
    QPointer<QToolButton> m_linkBlendChip;     // per-facet toggle (blend mode)
    void refreshLinkedFacetChips();            // enabled/checked state follows the master
    void showLinkNoReceiverHint(int srcSlot);  // §5.5 one-shot hint near the chip
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
    // STEP6-P4: cross-window 2-D camera sync. The 2-D view is copied GUTTER-INDEPENDENTLY (world
    // point at the data-area centre + world height, STEP6-P3) — this only makes sense between cubes
    // that share the spatial WORLD grid, so it is gated on sameSpatialGrid2D (D1: same-grid only).
    void broadcastLinked2DCamera(const double dataCenterXY[2], double worldHeight);
    void applyLinked2DCamera(const double dataCenterXY[2], double worldHeight);
    // STEP6-P4: cross-window 2-D SLICE colour sync — colormap name + stretch (scale mode + gamma)
    // + invert. Like broadcastLinkedColour (the 3-D volume path) the table RANGE is NOT shared
    // (data-dependent), so this needs no same-grid gate — colormap/stretch transfer to any cube.
    void broadcastLinked2DSliceLut();
    void applyLinked2DSliceLut(const QString &name, int scaleMode, double gamma, bool invert);
    // STEP6-P4: cross-window MOMENT / image-product colour sync. Product ids are per-window
    // scoped ("<stem>@<dataset>#<win>"), so peers are matched by the WINDOW-INDEPENDENT stem
    // (e.g. "moment0", "linewidth_fwhm") — deterministic for moments + line-width. Colormap +
    // scale + gamma + invert transfer (range is NOT shared, like the slice/volume paths).
    static QString productStem(const QString &productId);   // the part before '@'
    void broadcastLinked2DImageLut(const QString &stem, const QString &name, int scaleMode,
                                   double gamma, bool invert);
    void applyLinked2DImageLut(const QString &stem, const QString &name, int scaleMode,
                               double gamma, bool invert);
    // Apply a normalized 2-D view to every visible 2-D pane in THIS window (slice + image
    // products); @p skipSlot excludes the source pane (−1 = none). Shared by the intra-window
    // propagate and the cross-window apply.
    void applyNormalizedTo2DPanes(const double dataCenterXY[2], double worldHeight, int skipSlot);
    // True when @p peer shares this cube's 2-D spatial world grid (dimensions + x/y spacing +
    // x/y origin), so a world-coordinate camera transfers directly (STEP6-P4 / D1).
    bool sameSpatialGrid2D(const vtkWindowCube *peer) const;
    // Shared volume blend mode (0=Composite, 1=MIP, 2=MinIP).
    int m_volumeBlendMode{ 0 };
    void broadcastLinkedBlendMode();
    void applyLinkedBlendMode(int idx);
    // Linked 3-D representation (Volume ↔ Isosurface). Like the blend mode this is
    // gated only by the Link-views master (no per-facet chip).
    void broadcastLinkedRenderMode();
    void applyLinkedRenderMode(bool isosurface);

    // ── Copilot actions ─────────────────────────────────────────────────────
    // Apply the scientific copilot's visualisation directives (from the Copilot
    // panel's `actionsRequested` signal) to this viewer, in order. Display-only
    // and reversible: a snapshot of the affected state is taken before the batch
    // so undoCopilotActions() can restore it. See docs/copilot-backend.md.
    void applyCopilotActions(const QList<QJsonObject> &actions);
    void undoCopilotActions();
    void setCopilotRenderMode(bool iso);   ///< flip Volume↔Isosurface + resync sidebar/menu
    void applyCopilotBlendMode(int idx);   ///< blend mode + resync sidebar AND menu actions
    struct CopilotSnapshot
    {
        bool valid{ false };
        QString colormap;
        QString threshold;
        bool isosurface{ false };
        int blendMode{ 0 };
        int channel0{ 0 };   // 0-based
        // Camera is captured/restored only when a batch includes reset_camera, so
        // Undo reverts a copilot reset without clobbering the user's own moves.
        bool cameraCaptured{ false };
        double camPos[3]{ 0, 0, 0 };
        double camFoc[3]{ 0, 0, 0 };
        double camUp[3]{ 0, 0, 0 };
        double camScale{ 1.0 };
    };
    CopilotSnapshot m_copilotUndo;
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
    /// Give @p plot the spectral-axis context the line overlay needs (CTYPE,
    /// unit, rest frequency).
    void pushSpectralAxisContext(ProfileWidget *plot) const;
    double m_restFreqHz{ 0.0 };
    /// Backend-declared server-side rendering capability. Defaults to TRUE so a
    /// backend that predates the flag keeps the old behaviour (offer it, and let
    /// the request fail with its own message) rather than losing the option.
    bool m_serverSideRenderingAvailable{ true };
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
    QString formatRemoteOverlayCoordinate(int axis, double value, double axisSpan = -1.0) const;
    QString remoteOverlayAxisTitle(int axis) const;
    QString formatDegreeCoordinate(double value, double axisSpan = -1.0) const;
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
    // LRU cache of decoded remote slices (E-04 step 1: RemoteSliceCache owns the
    // cache/LRU/capacity that were three inline members). remoteSliceFetchesInFlight
    // is fetch-orchestration state and deliberately stays on the window.
    static constexpr int remoteSliceCacheCapacity = 8;
    RemoteSliceCache<RemoteCubeSliceResult> remoteSliceCache{ remoteSliceCacheCapacity };
    /// Slice cache key -> the requestId of the fetch already running for it, so
    /// a second ask for the same channel can adopt that fetch instead of being
    /// deduped into a request whose result nothing will accept.
    /// Slice cache key -> the watcher of the fetch already running for it. A
    /// second ask for the same channel PROMOTES that fetch (see
    /// requestRemoteSlice) instead of being deduped into a request whose result
    /// nothing will accept - which is also why the value is the watcher and not
    /// just its id: a prefetch has to stop being a prefetch when adopted.
    QHash<QString, QPointer<QObject>> remoteSliceFetchesInFlight;
    /// The channel currently ON SCREEN (set where a slice is actually applied).
    /// The spin box also shows channels that are still being fetched.
    int m_displayedSliceIndex{ -1 };
    void updateSlice();
    void updateRemoteSliceDragFeedback(int sliceIndex);
    void requestRemoteSlice(int sliceIndex);
    void applyRemoteSliceResult(const RemoteCubeSliceResult &result);
    QString remoteSliceCacheKey(int sliceIndex) const;
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
    /// Point the contour levels at the data once the backend reports the cube's
    /// real range. Until this runs the levels are the 0..1 placeholder set in the
    /// constructor, which for any real radio cube lies entirely outside the data
    /// - Show Contours then toggles an empty actor and looks broken.
    void refreshContourBoundsFromData();
    bool m_contourBoundsInitialised{ false };

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
    /// @p revealProvenance auto-switches the Inspector to Provenance (the default:
    /// a finished result's parameters are what the user wants next). Pass false
    /// for a transient product created BY arming a tool — the user is working in
    /// the Analysis tab and must not be thrown out of it.
    QString registerAnalysisProduct(const QString &kind, const QString &label,
                                    const QVariantMap &params, QWidget *window,
                                    const QString &stableId = QString(),
                                    const QString &renderer = QString(),
                                    bool revealProvenance = true,
                                    const QVariantMap &origin = {});
    /// Redraw the region, or re-place the probe marker, a product was generated
    /// from. Returns false when the product carries no geometry.
    bool restoreProductOrigin(const QVariantMap &origin);
    /// Set by restoreProductOrigin when the restored geometry belongs to a
    /// surface other than the one on screen; the caller appends it to its own
    /// status line instead of overwriting the warning.
    QString m_originRestoreNote;
    /// Per moment product: how many times ITS pixels have been replaced. The
    /// product id is stable across a recompute of the same order, so it alone
    /// cannot tell a measurement's map from the one now on screen - and a single
    /// global counter would flag every other map as recomputed too.
    QHash<QString, int> m_momentProductGeneration;
    /// True while a slice error is on the status bar, so an unrelated stale
    /// result finishing afterwards does not wipe the message the user needs.
    bool m_sliceErrorOnStatusBar{ false };
    /// Add the 2-D surface a region was measured on (channel N of the cube, or
    /// the moment map) to its origin. Geometry alone cannot tell them apart, and
    /// statistics from one surface mean nothing on another.
    QVariantMap stampRegionSurface(QVariantMap origin) const;
    int m_regionStatsCount{ 0 };

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
    /// Local-backend action: load the full grid here instead of switching to
    /// server-side rendering, which on one machine buys only the resolution.
    QPushButton *m_localRecommendLoadFullBtn{ nullptr };
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

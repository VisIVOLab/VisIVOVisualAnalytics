#ifndef vtkWindowCatalogue3D_h
#define vtkWindowCatalogue3D_h

#include "Catalogue3DParser.h"
#include "CataloguePlacement.h"
#include "CatalogueSelectionState.h"
#include "LinkedCameraSync.h"

#include "app/BackendClient.h"
#include "app/SampProtocol.h"
#include "app/CosmologyModel.h"
#include "app/VrLink.h"

#include <vtkNew.h>
#include <vtkSmartPointer.h>

#include <QDockWidget>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <array>

#include <vector>

class WorkspaceChrome;

// VTK forward declarations
class vtkActor;
class vtkBillboardTextActor3D;
class vtkCamera;
class vtkCubeAxesActor;
class vtkCubeSource;
class vtkGenericOpenGLRenderWindow;
class vtkFloatArray;
class vtkGlyph3DMapper;
class vtkIntArray;
class vtkLookupTable;
class vtkMaskPoints;
class vtkOrientationMarkerWidget;
class vtkPoints;
class vtkPolyData;
class vtkPolyDataAlgorithm;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkSphereSource;
class vtkArrowSource;
class vtkAppendPolyData;
class QAction;
class VrHeadsetDialog;
class QCheckBox;
class QComboBox;
class QDialog;
class QDoubleSpinBox;
class QListWidget;
class QPushButton;
class QLabel;
class QToolButton;
class QTextEdit;
class QTableView;
class Catalogue3DTableModel;
class MainWindow;
class QMenu;
class QStackedWidget;
class QSlider;
class QTimer;
class QFormLayout;
class LUTCustomizerDialog;
class VolumeColorbar;
class vtkImageData;
class RemotePointView;
class RaytraceOutput;
class RaytraceSettingsPanel;
class RaytraceViewport;
class QFrame;
class QFormLayout;
class QVBoxLayout;
class RemoteRenderWidget;
class vtkPointGaussianMapper;
class vtkPiecewiseFunction;
class AssistantPanel;
QT_BEGIN_NAMESPACE
namespace Ui {
class vtkWindowCatalogue3D;
}
QT_END_NAMESPACE

// Result struct for the background filter query task
struct Catalogue3DFilterTaskResult
{
    bool valid{ false };
    QString error;
    std::vector<Catalogue3DEntry> entries;
    std::vector<Catalogue3DEntry> unplaced;   ///< rows with no distance (issue #265)
    QString statusMsg;
    int totalRows{ 0 };
    /// Rows the backend returned, before invalid ones were skipped: what a
    /// page consumed, which is what the next page's offset moves by.
    int returnedRows{ 0 };
    int step{ 1 };   ///< number of disjoint sample phases
    int limit{ 0 };  ///< the page size that step was taken for
};

// Result struct for the background cosmology recalculation task
struct Catalogue3DCosmologyTaskResult
{
    bool valid{ false };
    QString error;
    QString model;
    /// The custom model the distances were computed with, when model is "Custom".
    visivo::cosmology::Model custom;
    /// Rows with a redshift above the custom-model route's z ≤ 20: they HAVE a
    /// redshift, so they are not "without a usable one", and they are placed
    /// by the fallback (catalogue distance, else local Planck18).
    int beyondRange{ 0 };
    // Computed comoving distances in Mpc, same order as the entries vector at
    // submission time.  Empty on error.
    std::vector<double> distancesMpc;
};

// How to name a catalogue that is not a file the user picked: a table
// received over SAMP is staged on the backend under a hashed name, which means
// nothing to the user. Empty: the file's own name.
struct Catalogue3DLabel
{
    QString displayName;          ///< shown wherever the file's name would be
    bool temporaryCopy{ false };  ///< a staged copy: its path is not shown as the data's
};

// ---------------------------------------------------------------------------
// vtkWindowCatalogue3D
//
// A standalone QMainWindow that renders a CSV catalogue in 3D space.
// Each source is positioned via RA/Dec/distance (or redshift) converted to
// Cartesian coordinates. Morphology drives discrete color coding; hover and
// click interaction expose source metadata in a side panel.
// ---------------------------------------------------------------------------
class vtkWindowCatalogue3D : public QMainWindow
{
    Q_OBJECT

public:
    // `paths` (optional) opens several catalogue files of the same format as ONE
    // concatenated catalogue; `filepath` is the primary/label path. Empty or
    // single-element `paths` behaves as a normal single-file open.
    // `label` (optional) names a catalogue whose file name means nothing to
    // the user (one received over SAMP).
    explicit vtkWindowCatalogue3D(const QString &filepath, const QString &backendUrl,
                                  const QString &backendToken,
                                  const QStringList &paths = QStringList(),
                                  const Catalogue3DLabel &label = Catalogue3DLabel(),
                                  QWidget *parent = nullptr,
                                  vtkWindowCatalogue3D *comparisonSource = nullptr);
    ~vtkWindowCatalogue3D() override;

    // ── SAMP row linking (issue #130, vtkWindowCatalogue3D_Samp.cpp) ──────────
    /// The key SAMP row messages are matched on: the backend path of a
    /// single-file catalogue. Empty for a combined (multi-file) one: its rows
    /// are numbered across all its files, which no other tool's table is.
    QString sampTableKey() const;
    /// Rows of the whole table on the backend (not only those loaded here).
    qint64 tableRowCount() const
    {
        return this->m_tableRows > 0 ? this->m_tableRows : static_cast<qint64>(this->m_totalCount);
    }
    QString cataloguePath() const { return this->filepath; }
    /// The catalogue opened on the backend and some rows loaded.
    /// The catalogue opened and its first rows loaded (a failed open leaves a
    /// window saying so).
    bool catalogueOpened() const
    {
        return !this->remoteDatasetId.isEmpty() && this->m_openFailedAtMs < 0;
    }
    /// When the first load failed, just before its error box (ms since the
    /// epoch; -1: it did not fail).
    qint64 openFailedAtMs() const { return this->m_openFailedAtMs; }
    /// Time the byte-order question (issue #165) was up during the open: the
    /// user's, not the open's ("Last 5 jobs").
    qint64 byteOrderPromptMs() const { return this->m_byteOrderPromptMs; }
    bool hasLoadedRows() const { return !this->remoteDatasetId.isEmpty() && !this->entries.empty(); }
    /// table.highlight.row: select the source of original-table row @p row and
    /// fly to it, as a click on it would. False (and a status line saying so)
    /// when that row is not among the rows loaded here.
    bool highlightSampRow(qint64 row, const QString &from);
    /// table.select.rowList: mark the sources of @p rows (original-table
    /// rows) and say how many there are; an empty list clears the marks.
    void selectSampRows(const QList<qint64> &rows, const QString &from);

private slots:
    void toggleLabels(bool checked);
    void toggleMorphColors(bool checked);
    void scaleChanged(int value);
    void resetCamera();
    void applyCameraPreset(double azimuth, double elevation);
    void updateGeometryMode();
    void updateSizeMode();
    void toggleSceneAxes(bool checked);
    void toggleBoundingBox(bool checked);
    void toggleShells(bool checked);
    void syncTableSelection(int index);

    /// Apply scientific-copilot visualisation directives to this catalogue view.
    /// Only camera control (reset_camera) applies to a point cloud; other
    /// directives (colormap/threshold/render mode — cube/image only) are ignored.
    /// The camera is snapshotted first so undoCopilotActions() can restore it.
    void applyCopilotActions(const QList<QJsonObject> &actions);
    void undoCopilotActions();   ///< revert the last copilot camera change

signals:
    void sourceSelectionChanged(int index);
    /// The user selected the source of original-table row @p row (a click in
    /// the view or the table, never a SAMP message): for table.highlight.row.
    void sampRowActivated(const QString &tableKey, qint64 row);
    /// Emitted when a session-expiry error is detected and the user requests
    /// that the dataset be reopened.  Connect to MainWindow::doOpenCataloguePath.
    void reopenRequested(const QString &filepath, const QStringList &paths = QStringList());
    /// The filters the rows were last queried with changed (Apply, or a
    /// failed query that put the previous ones back): Selection Diagnostics
    /// follows these, not the list being edited.
    void appliedFiltersChanged(const std::vector<BackendTabularFilter> &filters);

protected:
    // Refuse to close (and self-delete via WA_DeleteOnClose) while a large scene
    // build is pumping the event loop — that would delete the window under
    // buildScene() and use-after-free its members.
    void closeEvent(QCloseEvent *event) override;

private:
    enum class GeometryMode
    {
        Ellipsoid,
        Sphere,
        Point,
        Cross,
        Velocity,   // arrows oriented by vx/vy/vz, scaled by speed
        Gaussian,   // emissive Gaussian splats (a simulation's particles)
        Raytraced,  // ray-traced on the GPU (RaytraceViewport, issue #101)
    };

    enum class SizeMode
    {
        Fixed,
        MajorAxis,
        Lls,
        Flux,
    };

    // ── Control pages ─────────────────────────────────────────────────────────
    // Built once, then mounted in the two docks the image and cube viewers use:
    // the view controls in the LEFT column under Session Data (design #1d — the
    // rendering controls live beside what is being rendered), the source info
    // and the filters in the RIGHT Inspector. They used to live in a rail of
    // their own that existed nowhere else in the application.
    QPointer<QWidget> m_pageViz;
    QPointer<QWidget> m_pageFilters;
    QPointer<QAction> actionShowControlsSidebar;   ///< "Inspector Panel"
    QPointer<QAction> m_actionSessionPanel;        ///< "Session Data Panel"
    void buildControlPages();

    // ── Shared viewer chrome ──────────────────────────────────────────────────
    // The command bar (path · kind tag · ⌘K · backend chip) and the status rail
    // the image and cube viewers have always had. This window is a viewer too.
    WorkspaceChrome *m_workspaceChrome{ nullptr };
    void setupViewerChrome();
    /// Session Data on the left, Inspector on the right — the same two docks as
    /// the image and cube viewers, with this window's pages mounted in them.
    void setupWorkspaceDocks();
    /// Re-word the command bar's kind tag and the cosmology status line from the
    /// CURRENT state. Both were written once at startup, so a filter or another
    /// page of sources left the bar claiming the number the window opened with.
    void refreshViewerChrome();
    /// "3D Catalogue, <name>  (100000 of 150000 sources)".
    void updateWindowTitle();
    Catalogue3DLabel m_label;
    /// The catalogue's name as shown: the label's, else the file's.
    QString shownName() const;
    /// The command bar's tag of a table of particles: VBT, GADGET (#31).
    QString particleTag() const;
    /// Show Load more / Load all when the backend holds more rows than loaded.
    void updateLoadMoreButtons();
    /// A full local materialisation is useful for modest tables, but becomes
    /// counterproductive once rows x columns would create tens of millions of
    /// QVariant values in the desktop process.  The backend can still query the
    /// complete table and Load more can refine its representative sample.
    bool canLoadAllLocally() const;
    QPointer<QToolButton> m_kindTag;
    /// The two Inspector rows that do not stay put: the count changes with a
    /// filter or another page of sources, and the session changes when an
    /// expired one is recovered. Held as widgets because re-running
    /// setProperties() would delete the Source Info section mounted under them.
    QPointer<QLabel> m_propSources;
    QPointer<QLabel> m_propSession;
    /// Metadata returned for a Gadget snapshot and its selected particle type.
    /// Kept so the Inspector can state stored units and physical conversions.
    QJsonObject m_snapshotMetadata;
    QJsonObject m_componentMetadata;
    /// What the kind tag opens: the catalogue's columns and which of them the
    /// backend recognised as coordinates — this window's answer to the image
    /// viewer's FITS header.
    void showCatalogueFieldsDialog();
    /// Under the cosmology controls: which model the positions on screen were
    /// computed with, and how many sources it moved.
    QPointer<QLabel> m_cosmologyStatus;
    /// Under it (issue #265): how many sources are placed, unplaced for want of
    /// a distance, and placed from cz < 1000 km/s; and the button that lists
    /// the unplaced ones (hidden when there are none).
    QPointer<QLabel> m_placementStatus;
    QPointer<QPushButton> m_unplacedButton;
    QPointer<QAction> m_actionUnplaced;
    void refreshPlacementStatus();
    /// The "Unplaced sources" table: Name, RA, Dec, cz [km/s], Reason.
    void showUnplacedSources();
    QPointer<QDialog> m_unplacedDialog;
    /// The rows the open table lists, to rebuild it when they change.
    std::vector<qint64> m_unplacedDialogKey;
    std::vector<qint64> unplacedRowKey() const;
    /// H0 of the applied cosmology into the schema, for the redshift-space
    /// distance (issue #260). Called wherever m_appliedCosmologyModel changes.
    void applyHubbleConstant();

    // ── Scientific copilot ────────────────────────────────────────────────────
    AssistantPanel *m_copilot{ nullptr };
    /// State captured before the copilot's last batch of changes, so Undo can
    /// restore exactly that batch. Each ``*Captured`` flag guards its own field.
    struct CopilotUndo
    {
        bool cameraCaptured{ false };
        double position[3]{ 0, 0, 0 };
        double focalPoint[3]{ 0, 0, 0 };
        double viewUp[3]{ 0, 0, 1 };
        double parallelScale{ 1.0 };
        bool scaleCaptured{ false };
        QString scaleText;
        bool colorFieldCaptured{ false };
        QString colorFieldText;
    } m_copilotUndo;

    // ── Setup ──────────────────────────────────────────────────────────────
    void setupRenderer();
    void applyFilter();
    void buildScene();
    void buildLabels();
    void buildShells();
    void updateGlyphPipeline();
    void updateGlyphScales();
    void updatePointPositions();
    void updateColorMapping();
    void ensureCatalogueDock();
    void refreshCatalogueTable();
    void populateMappingControls();
    void updateDockSelectionDetails();
    QStringList numericRawFieldNames() const;
    bool rawFieldValue(const Catalogue3DEntry &entry, int fieldIndex, double &value) const;
    double mappedAxisValue(const Catalogue3DEntry &entry, const QString &mappingKey) const;
    double mappedSizeValue(const Catalogue3DEntry &entry, bool &ok) const;
    double mappedColorValue(const Catalogue3DEntry &entry, bool &ok) const;
    QString fallbackDetailsText(const Catalogue3DEntry &entry) const;

    // ── Morphology LUT ─────────────────────────────────────────────────────
    void buildMorphologyLut();
    // Returns the RGB triple for a given morphology string (deterministic).
    static std::array<double, 3> morphologyColor(const QString &morph, int fallbackIndex);

    // ── Interaction ─────────────────────────────────────────────────────────
    // Called by the VTK mouse observer (MouseMove / LeftButton Press+Release).
    void onMouseEvent(unsigned long eid);
    // Projects all source points to display space; returns index of nearest
    // within hoverThresholdPx, or -1.
    int pickNearestSource(int displayX, int displayY) const;
    /// The same over a view of @p width x @p height pixels seen by @p camera
    /// (the Remote or ray-traced view's, not this window's renderer).
    int pickNearestSource(vtkCamera *camera, double width, double height, int displayX,
                          int displayY) const;
    void setHoveredSource(int idx);
    void setSelectedSource(int idx);
    /// Brings source @p idx to the middle of the view on screen (local,
    /// Remote or ray-traced). @p frame: a SubFind target is framed by its
    /// radius; otherwise the distance is divided by @p zoomFactor. A SAMP
    /// highlight only recentres (recentreLocalView) and never reframes.
    void centerOnSource(int idx, double zoomFactor = 3.0);
    /// SAMP: the local view looks at source @p idx from where it is.
    void recentreLocalView(int idx);
    /// The radius a SubFind target is framed by, and its column; 0 when the
    /// table is not a SubFind catalogue or the row has none.
    double framingRadius(int idx, QString *field = nullptr) const;
    /// A double-click on the Remote or ray-traced view (they have no VTK
    /// picker): picks the nearest marker with that view's camera and frames it.
    void frameSourceAtViewPoint(QWidget *view, const QPoint &pos);
    /// centerOnSource once the shown view's @p camera is known (@p error:
    /// why there is none).
    void frameSourceWithCamera(int idx, double zoomFactor, const QJsonObject &camera,
                               const QString &error);
    bool eventFilter(QObject *watched, QEvent *event) override;

    // ── UI helpers ──────────────────────────────────────────────────────────
    void updateInfoPanel();
    /// "Distance 12.3 Mpc, from z (comoving, Planck18)", and "cz < 1000 km/s:
    /// uncertain" when that applies (issue #265): the pick readout's lines.
    QStringList pickDistanceLines(const Catalogue3DEntry &e) const;
    /// After the rows were placed again (model, distance mode): the selected
    /// source's marker, panel and table row follow it (issue #298).
    void refreshSelectionAfterReplacement();
    // Reposition and resize a highlight sphere to sit on source[idx].
    /// The marker's radius from the data alone: the glyph it wraps, or a
    /// fraction of the particles' mean spacing (highlightRadius then keeps it
    /// to a few pixels on screen, SelectionMarkerSize.h).
    double highlightRadiusInData(int idx, double radiusFactor, bool wrapsGlyph) const;
    /// Re-sizes the hover and selection markers for the current camera.
    void refreshHighlightSizes();
    unsigned long m_markerObserverTag{ 0 };   ///< renderer StartEvent → refreshHighlightSizes
    void updateHighlightSphere(vtkSphereSource *sphere, vtkActor *actor, int idx,
                               double radiusFactor);

    // ── Coordinate frame ────────────────────────────────────────────────────
    enum class CoordFrame { FK5_J2000, Galactic, Supergalactic };
    CoordFrame activeFrame{ CoordFrame::FK5_J2000 };
    // Recompute sceneX/Y/Z for all current entries using activeFrame.
    // FK5_J2000: no-op (parser already computes in this frame).
    // Galactic:  converts raw raDeg/decDeg → (l,b) then to Cartesian.
    void applyFrameToEntries();

    // ── Cosmology ────────────────────────────────────────────────────────────
    // Trigger async recalculation of distances using the selected model.
    // For Planck18 simply clears the per-entry distanceMpc override (reuses
    // the local computation), otherwise calls the backend batch endpoint.
    void triggerCosmologyUpdate();
    void onCosmologyFinished();
    /// The model the selector names, as a full model: a preset, or the custom
    /// model the user handed over from the calculator (issue #29).
    visivo::cosmology::Model selectedCosmology() const;
    /// The backend draws the positions itself (the server-side point view or
    /// the ray-traced view) and knows the presets only.
    bool serverDrawsPositions() const;
    /// Put the selector back on Planck18 when a server-drawn view is turned on
    /// under the custom model, and say why.
    void dropCustomCosmologyForServerView(const QString &view);
    /// Tools ▸ Cosmology Calculator, opened on the model the sources use.
    void openCosmologyCalculator();
    /// Open a second quantity of this simulation without reopening its backend dataset.
    void openQuantityComparison();
    void setupComparisonLink(vtkWindowCatalogue3D *source);
    void teardownComparisonLink();
    /// Keep the persistent comparison card and window title in step with the group.
    void updateComparisonUi();
    struct ComparisonRepresentation
    {
        QString geometry;
        int sizeSlider{ 100 };
        int renderBudget{ 1000000 };
        bool raytraced{ false };
    };
    ComparisonRepresentation comparisonRepresentation() const;
    void applyComparisonRepresentation(const ComparisonRepresentation &representation);
    void broadcastComparisonRepresentation();
    void setComparisonLinked(bool linked);
    void setComparisonRepresentationLinked(bool linked);
    void setupSnapshotTimeline(vtkWindowCatalogue3D *source);
    void updateSnapshotTimelineUi();
    void switchSnapshotIndex(int index);
    void stepSnapshot(int delta);
    void toggleSnapshotPlayback();
    void broadcastComparisonSelection(int index);
    void broadcastComparisonRaytraceCamera(const QJsonObject &camera);
    void onComparisonCameraFrame();
    void applyComparisonCamera(const LinkedCameraSync::Pose &pose);
    void applyComparisonRaytraceCamera(const QJsonObject &camera);
    LinkedCameraSync::Pose comparisonCameraPose() const;
    void openInVrHeadset();   // issue #190: vtkWindowCatalogue3D_VrHeadset.cpp
    QPointer<VrHeadsetDialog> m_vrHeadsetDialog;
    /// The applied frame, distance mode and cosmology the headset places a
    /// sky catalogue with (issue #260); empty for a Cartesian table or a VBT.
    VrLink::Sky vrSky() const;
    /// The same, and whether the applied model is Custom (refused).
    CataloguePlacement::HeadsetSky headsetSky() const;
    /// An open headset dialog follows a change of them (its next show).
    void pushSkyToVrDialog();
    /// What the headset dialog is given: this window's backend identity,
    /// dataset and sky placement as they are now.
    VrLink::Inputs vrHeadsetInputs() const;
    /// The dataset was reopened in a new session: an open headset dialog
    /// drops the expired one and pairs with the new one (issue #305).
    void pushSessionToVrDialog();
    /// The custom model the positions on screen were computed with (valid
    /// when m_appliedCosmologyModel is "Custom").
    visivo::cosmology::Model m_appliedCustomCosmology;
    // Recompute every source's scene position from entryDistanceMpc() (which now
    // honours schema.distanceMode) and rebuild the scene/labels. Does NOT touch the
    // per-entry distanceMpc overrides — callers clear those first when they must.
    void recomputeScenePositionsAndRebuild();

    // ── Data ────────────────────────────────────────────────────────────────
    Ui::vtkWindowCatalogue3D *ui;
    QString filepath;
    QStringList m_cataloguePaths;   // multi-file open (>1 → concatenated); else empty/single
    QString backendUrl;
    QString backendToken;
    QString remoteDatasetId;
    qint64 m_openFailedAtMs{ -1 };
    qint64 m_byteOrderPromptMs{ 0 };
    QString remoteSessionId;
    /// Windows made with Compare Quantity share this non-empty group.
    QString m_comparisonGroup;
    bool m_comparisonLinked{ false };
    bool m_comparisonRepresentationLinked{ true };
    bool m_applyingComparisonUpdate{ false };
    LinkedCameraSync::Tracker m_comparisonCameraTracker;
    unsigned long m_comparisonCameraObserverTag{ 0 };
    BackendPointSeriesResult m_snapshotSeries;
    QFutureWatcher<BackendPointSeriesResult> m_snapshotSeriesWatcher;
    bool m_snapshotSwitching{ false };
    bool m_snapshotPlaying{ false };
    std::vector<Catalogue3DEntry> entries;
    /// Rows with no distance (issue #265), never in entries: not drawn.
    std::vector<Catalogue3DEntry> unplacedEntries;
    Catalogue3DSchema schema;
    QStringList rawHeaders;
    std::vector<int> numericRawFieldIndices;
    // Column indices of vx/vy/vz when the catalogue carries velocity components
    // (-1 if absent) → enables the "Speed |v|" colour mapping (peculiar-velocity
    // magnitude), the cosmic-flow quantity. Recomputed from rawHeaders whenever
    // the column set changes (BEFORE any buildScene/updateColorMapping).
    int m_velocityColIdx[3]{ -1, -1, -1 };
    void detectVelocityColumns();
    std::vector<std::array<double, 3>> mappedPositions;
    // Ordered list of unique morphology names (determines LUT index).
    QStringList morphologyNames;
    // Name → LUT index
    QMap<QString, int> morphologyIndexOf;
    // Default glyph radius derived from bounding box of all sources.
    double defaultGlyphRadius{ 1.0 };
    double maxDistanceMpc{ 1.0 };
    double sizeNormalizationValue{ 1.0 };
    GeometryMode geometryMode{ GeometryMode::Ellipsoid };
    SizeMode sizeMode{ SizeMode::Fixed };
    double fixedFallbackDistanceMpc{ 300.0 };

    // ── VTK rendering ───────────────────────────────────────────────────────
    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkNew<vtkRenderer> renderer;

    // Main source cloud — GPU-instanced glyphs (vtkGlyph3DMapper) so large
    // catalogues render fast without materialising per-point geometry on the CPU.
    vtkNew<vtkPoints> sourcePoints;
    // One scalar per source feeding the active colour LUT. Float (not int) so
    // continuous colour modes (raw columns, "Speed |v|") aren't truncated;
    // morphology stores integer class indices, which float holds exactly.
    vtkNew<vtkFloatArray> morphScalars;
    vtkNew<vtkFloatArray> glyphScaleVectors;
    vtkNew<vtkFloatArray> velocityVectors;   // vx/vy/vz per source (named "VelocityVector")
    vtkNew<vtkPolyData> sourcesPolyData;
    vtkNew<vtkSphereSource> sphereSource;
    vtkNew<vtkArrowSource> arrowSource;       // glyph source for the Velocity mode
    vtkNew<vtkCubeSource> pointCubeSource;
    vtkNew<vtkAppendPolyData> crossSource;
    // Render LOD: when the catalogue exceeds m_renderBudget points, the mappers
    // are fed a deterministic stride subsample of about this many points (full
    // data is kept for picking / stats / camera framing). 0 = unlimited.
    vtkNew<vtkMaskPoints> renderDecimator;
    int m_renderBudget{ 1000000 };
    bool m_renderDecimated{ false };
    void applyRenderBudget();
    /// Status text when the GPU point budget is thinning the cloud (empty when
    /// the full cloud is drawn). Reassures that picking/stats use every source.
    QString renderBudgetStatusText() const;

    vtkNew<vtkGlyph3DMapper> glyphMapper;
    vtkNew<vtkActor> glyphActor;
    vtkNew<vtkLookupTable> morphologyLut;
    vtkNew<vtkLookupTable> valueLut;
    vtkNew<vtkPolyDataMapper> pointsMapper;
    vtkNew<vtkActor> pointsActor;
    // Gaussian splats (GeometryMode::Gaussian), sized and faded as the former
    // VBT point viewer did; an actor of their own.
    vtkNew<vtkPointGaussianMapper> gaussianMapper;
    vtkNew<vtkPiecewiseFunction> gaussianOpacity;
    vtkNew<vtkActor> gaussianActor;
    void updateGaussianSplats();
    /// Point size of the Point shape, and the splats' size step (1..12).
    int pointSizeFromScale() const;
    double splatSizeFromScale() const;   ///< the splats' size step (former VBT viewer's)
    /// Bounds and count of the points with no filter: the splats are sized on
    /// them, so that a filter removes points without restyling the others.
    double m_splatBounds[6]{ 0, 0, 0, 0, 0, 0 };
    vtkIdType m_splatCount{ 0 };

    // Hover highlight (wireframe, yellow)
    vtkNew<vtkSphereSource> hoverSphere;
    vtkNew<vtkPolyDataMapper> hoverMapper;
    vtkNew<vtkActor> hoverActor;

    // Selection highlight (wireframe, red-white)
    vtkNew<vtkSphereSource> selectSphere;
    vtkNew<vtkPolyDataMapper> selectMapper;
    vtkNew<vtkActor> selectActor;

    // Billboard text labels (world-space, camera-facing)
    std::vector<vtkSmartPointer<vtkBillboardTextActor3D>> labelActors;
    bool m_labelsSkippedDueToScale{ false };  // per-point labels not built (too many sources)
    bool m_sceneBuilding{ false };            // a large buildScene() is in progress
    bool m_cosmologyFinishDeferred{ false };  // a cosmology result arrived mid-build → apply after
    /// During a large buildScene(), pump the event loop + show progress every
    /// few hundred-k points so the UI stays responsive (no OS "not responding").
    void pumpSceneBuild(int processed);

    // Orientation axes corner widget
    vtkNew<vtkOrientationMarkerWidget> axesWidget;
    vtkNew<vtkCubeAxesActor> cubeAxesActor;
    std::vector<vtkSmartPointer<vtkActor>> shellActors;
    QPointer<QDockWidget> catalogueDock;
    QPointer<QTableView> catalogueTableView;
    QPointer<Catalogue3DTableModel> catalogueTableModel;
    QPointer<QTextEdit> catalogueDetailsView;
    QPointer<QComboBox> comboMapX;
    QPointer<QComboBox> comboMapY;
    QPointer<QComboBox> comboMapZ;
    QPointer<QComboBox> comboMapSize;
    QPointer<QComboBox> comboMapColor;
    QPointer<QComboBox> comboColorScale;       // Linear / Log / Sqrt / Square / Power
    QPointer<QComboBox> comboColormap;         // the generated ramp, or a named map
    QPointer<QDoubleSpinBox> spinColorGamma;   // exponent for the Power mode
    QPointer<QComboBox> comboGeometry;
    QPointer<QComboBox> comboRenderBudget;     // GPU point budget (render LOD)
    QPointer<QComboBox> comboSizeMode;
    QPointer<QFrame> m_comparisonCard;
    QPointer<QLabel> m_comparisonSummary;
    QPointer<QPushButton> m_comparisonLinkToggle;
    QPointer<QCheckBox> m_comparisonRepresentationToggle;
    QPointer<QFrame> m_timelineCard;
    QPointer<QLabel> m_timelineSummary;
    QPointer<QSlider> m_timelineSlider;
    QPointer<QPushButton> m_timelinePrevious;
    QPointer<QPushButton> m_timelineNext;
    QPointer<QPushButton> m_timelinePlay;
    QPointer<QTimer> m_timelineTimer;
    QPointer<QComboBox> comboFrame;
    QPointer<QComboBox> comboCosmology;
    QPointer<QComboBox> comboDistanceMode;     // R-14: real-space vs redshift-space
    QPointer<QCheckBox> chkShowAxes;
    QPointer<QCheckBox> chkShowBoundingBox;
    QPointer<QCheckBox> chkShowShells;
    // Multi-filter UI
    QPointer<QListWidget> filterList;
    QPointer<QPushButton> btnFilterAdd;
    QPointer<QPushButton> btnFilterAddSpatial;
    QPointer<QPushButton> btnFilterRemove;
    QPointer<QPushButton> btnFilterClearAll;
    QPointer<QPushButton> btnFilterApply;
    QPointer<QLabel> filterStatusLabel;
    /// The filter list as edited; Apply copies it into m_appliedFilters.
    std::vector<BackendTabularFilter> activeFilters;
    /// What the rows on screen, the next page, the Remote view and Selection
    /// Diagnostics are filtered by.
    visivo::selection::AppliedFilters m_appliedFilters;
    /// Bumped by every framing request: a backend camera that arrives after a
    /// newer one was asked for is dropped.
    quint64 m_frameRequest{ 0 };
    bool filterBusy{ false };
    QFutureWatcher<Catalogue3DFilterTaskResult> filterWatcher;
    // ── Pagination ────────────────────────────────────────────────────────────
    int m_currentOffset{ 0 };
    // Every catalogue loads a stable randomized sample spanning all rows.
    // Each "Load more" adds a disjoint phase, until every row is loaded.
    int m_sampleStep{ 1 };
    int m_samplePhase{ 0 };
    int m_sampleLimit{ 100000 };   ///< the page size the step was taken for
    /// Rows left to load: phases of the sample, or pages.
    bool hasMoreRows() const
    {
        return this->m_sampleStep > 1 ? this->m_samplePhase < this->m_sampleStep
                                      : this->m_currentOffset < this->m_totalCount;
    }
    int m_pageSize{ 50000 };
    int m_totalCount{ -1 };
    bool m_loadingMore{ false };
    QPointer<QPushButton> btnLoadMore;
    QPointer<QPushButton> btnLoadAll;
    QPointer<QWidget> m_loadIndicator;
    QPointer<QLabel> m_loadMessage;
    QFutureWatcher<Catalogue3DFilterTaskResult> m_loadMoreWatcher;
    void loadMoreEntries();
    void loadAllEntries();   // load every remaining row in one query (no paging)
    // ── Session-expiry auto-recovery ──────────────────────────────────────────
    QFutureWatcher<BackendCatalogueInfo> m_reopenWatcher;
    void autoReopenThenLoadMore();
    QString m_cosmologyModel{ QStringLiteral("Planck18") };
    /// The model the loaded rows were last placed with (the one the backend is
    /// told): a request that fails leaves it, and the selector goes back to it.
    QString m_appliedCosmologyModel{ QStringLiteral("Planck18") };
    /// Rows replaced (a filter, Load more) arrive with the local Planck18
    /// distances: another model is computed for them again.
    void reapplyCosmologyAfterReload();
    bool m_cosmologyBusy{ false };
    QFutureWatcher<Catalogue3DCosmologyTaskResult> m_cosmologyWatcher;
    bool syncingTableSelection{ false };
    /// Set while a click in the table drives the selected source: the
    /// table's (multi-row) selection is then left as the user made it.
    bool tableDrivenSelection{ false };
    bool updatingMappingControls{ false };
    QString mapXKey{ QStringLiteral("computed:x") };
    QString mapYKey{ QStringLiteral("computed:y") };
    QString mapZKey{ QStringLiteral("computed:z") };
    QString mapSizeKey{ QStringLiteral("builtin:fixed") };
    QString mapColorKey{ QStringLiteral("builtin:morphology") };

    // ── SAMP row linking ──────────────────────────────────────────────────────
    /// Original-table row ↔ entry; rebuilt when the entries change.
    SampRowMap m_sampRowMap;
    bool m_sampRowMapDirty{ true };
    std::size_t m_sampRowMapSize{ 0 };
    void ensureSampRowMap();
    /// False while the streamed (backend) or ray-traced view is on screen.
    bool sampShowsLocalView() const;
    /// Appended to a SAMP status line when the view on screen is not the
    /// local one: how to get back to it, which depends on what replaced it.
    QString sampLocalViewNote() const;
    /// The status line a selection last wrote (SAMP's or a click's): a newer
    /// selection replaces it, any other status is left alone.
    QString m_selectionStatus;
    void showSelectionStatus(const QString &text);
    /// Set while a SAMP message drives the selection, so it is not sent back.
    bool m_applyingSampSelection{ false };
    /// The last table.select.rowList, as original rows: re-marked after a
    /// filter or another page of rows changes what is loaded.
    QList<qint64> m_sampSubsetRows;
    QString m_sampSubsetFrom;
    vtkNew<vtkPoints> m_sampSubsetPoints;
    vtkNew<vtkFloatArray> m_sampSubsetScales;
    vtkNew<vtkPolyData> m_sampSubsetData;
    vtkNew<vtkSphereSource> m_sampSubsetSphere;
    vtkNew<vtkGlyph3DMapper> m_sampSubsetMapper;
    vtkNew<vtkActor> m_sampSubsetActor;
    /// Rebuild the marks of m_sampSubsetRows on the current positions;
    /// @p report: also write the count in the status line.
    void updateSampSubset(bool report);
    /// Radius of a highlight on source @p idx (the selection sphere's).
    double highlightRadius(int idx, double radiusFactor);

    // ── SAMP actions (issue #130, vtkWindowCatalogue3D_Samp.cpp) ─────────────
    /// The SAMP menu of this window, and the table's context menu.
    void setupSampMenu();
    /// Send this catalogue, a selection of it, or a source's position.
    /// @p selectionOnly: the table's context menu (no whole-table items).
    void fillSampMenu(QMenu *menu, MainWindow *host, bool selectionOnly);
    /// Original rows of the rows selected in the table, ascending; the
    /// selected source's when the table has no selection.
    QList<qint64> sampSelectedRows();
    /// Original rows of every source loaded here (a filter, a sample or a
    /// partial load make that fewer than the table's).
    QList<qint64> sampShownRows();
    /// How many rows sampSelectedRows() would list, without listing them.
    qsizetype sampSelectedCount() const;
    bool sampShowsFewerRows() const;
    /// coord.pointAt.sky of the selected source; @p quiet: from a click,
    /// so nothing is said when it cannot be sent.
    void sendSelectedSourcePosition(const QString &recipientId, bool quiet);
    /// Send coord.pointAt.sky for every source the user clicks (off by default).
    bool m_sampBroadcastClicks{ false };
    /// The positions are RA/Dec columns (not X/Y/Z, not l/b).
    bool m_skyCoordinates{ false };

    // Interaction state
    int hoveredIndex{ -1 };
    int selectedIndex{ -1 };
    QElapsedTimer m_hoverThrottle;   // rate-limits hover picking during motion
    // Left-button drag tracking (to distinguish click from camera rotate)
    bool leftButtonDown{ false };
    bool leftClickCandidate{ false };
    int pressPickIndex{ -1 };
    int pressX{ 0 };
    int pressY{ 0 };
    static constexpr int clickDragThresholdPx = 5;

    static constexpr double hoverThresholdPx = 14.0;

    // ── Drawn on the backend (issue #83, vtkWindowCatalogue3D_Remote.cpp) ─────
    // A VBT point table with more rows than this window loaded, on a remote
    // backend that renders, is drawn whole there and streamed; the loaded rows
    // stay here for picking, the table and statistics. With the local backend
    // the view is always local.
    enum class RenderMode { Auto, Local, Remote };
    RenderMode m_renderMode{ RenderMode::Auto };
    bool m_serverSideRendering{ false };   ///< from the open: a VBT, a backend that renders
    int m_tableRows{ 0 };                  ///< rows of the whole table
    QPointer<QStackedWidget> m_renderStack;
    QPointer<RemoteRenderWidget> m_remoteWidget;
    QPointer<RemotePointView> m_remoteView;
    QPointer<QAction> m_actRenderRemote;
    QPointer<QLabel> m_renderBadge;
    void setupRemoteRendering();
    bool wantsRemote() const;
    int rowsToShow() const;

    // ── A table of points (issue #83): a VBT, cartesian ──────────────────────
    // The same window as a catalogue, without what only a sky catalogue has:
    // no name/type/RA/Dec/distance columns, no frame, no cosmology, no shells;
    // it opens drawn as points coloured by its first quantity.
    bool m_pointTable{ false };   ///< a simulation's particles: a VBT of points, a snapshot's particle type
    QString m_tableFormat;        ///< the format the backend read ("vbt", "gadget", "csv", ...)
    bool m_defaultScaleChosen{ false };   ///< the opening Log/Linear choice, made once
    bool m_cartesian{ false };    ///< positions are X/Y/Z columns
    /// A table of points without classes: colour means a column, or one
    /// colour for all (no morphology checkbox).
    bool m_noMorphology{ false };
    QPointer<QWidget> m_cosmoCard;
    QFormLayout *m_sceneForm{ nullptr };
    QFormLayout *m_encForm{ nullptr };
    void applyPointTableLayout();
    void choosePointTableDefaults();

    // ── Colour bar and LUT editor, as in the cube viewer's 3-D view ───────────
    QPointer<VolumeColorbar> m_colorbar;
    QPointer<LUTCustomizerDialog> m_lutEditor;
    vtkSmartPointer<vtkImageData> m_lutEditorData;   ///< the colour column, for the histogram
    /// Min..Max set in the editor; empty (lo >= hi): the column's range.
    double m_colorUserRange[2]{ 0.0, 0.0 };
    /// Drawn on the backend: the colour column's range over every row drawn
    /// there, as it reports it (the rows loaded here are only a page).
    double m_colorRemoteRange[2]{ 0.0, 0.0 };
    void refreshColorbar();
    void showLutEditor();
    void applyRenderMode(RenderMode mode);
    void updateRenderBadge();
    /// The view's state as commands for the backend (sent when changed).
    void pushRemoteState();
    /// The view's state as the backend's point commands: positions, filters,
    /// colours, style. ``textFiltersSkipped``: set when a text filter could not
    /// be sent (the backend filters on numbers).
    QList<QVariantMap> pointViewCommands(bool *textFiltersSkipped = nullptr) const;
    /// The view's state in a ray-traced render request's terms.
    QJsonObject raytraceViewState() const;
    /// The camera of the view on screen (asks the backend's view for its own).
    /// Without one, the string is the line for the status bar.
    void viewCamera(std::function<void(const QJsonObject &, const QString &)> done);
    QStringList numericColumns() const;
    // Ray-traced light: the view on the GPU, its settings in the sidebar. A
    // way of drawing, not a shape: its own check under the Shape, as in the
    // cube viewer; the shape is kept for when it is turned off.
    void setRaytraced(bool on);
    /// The view cannot be drawn (no camera, or a backend without the routes):
    /// turned off, with `why` in the status bar.
    void leaveRaytraced(const QString &why);
    QPointer<QCheckBox> m_rtLightCheck;
    /// The camera request the ray-traced view waits for: an answer to an
    /// earlier one (the check cleared and ticked again meanwhile) is dropped.
    quint64 m_rtCameraRequest{ 0 };
    /// The stack's page for a render mode; the ray-traced view while it is on.
    void showRenderPage(int index);
    int m_renderPage{ 0 };
    QPointer<RaytraceViewport> m_rtView;
    QPointer<RaytraceSettingsPanel> m_rtPanel;
    QPointer<RaytraceOutput> m_rtOutput;
    QPointer<QFrame> m_rtCard;
    QPointer<QVBoxLayout> m_vizPageLayout;
    QPointer<QFrame> m_reprCard;
    // What only the drawn points use, disabled while the view is ray-traced.
    QPointer<QFrame> m_sceneCard;
    void enablePointOnlyControls(bool enabled);
};

#endif // vtkWindowCatalogue3D_h

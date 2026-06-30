#ifndef vtkWindowCatalogue3D_h
#define vtkWindowCatalogue3D_h

#include "Catalogue3DParser.h"

#include "app/BackendClient.h"

#include <vtkNew.h>
#include <vtkSmartPointer.h>

#include <QDockWidget>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <array>

#include <vector>

// VTK forward declarations
class vtkActor;
class vtkBillboardTextActor3D;
class vtkCubeAxesActor;
class vtkCubeSource;
class vtkGenericOpenGLRenderWindow;
class vtkFloatArray;
class vtkGlyph3DMapper;
class vtkIntArray;
class vtkLookupTable;
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
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QPushButton;
class QLabel;
class QTextEdit;
class QTableView;
class SidebarPanel;
class Catalogue3DTableModel;
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
    QString statusMsg;
    int totalRows{ 0 };
};

// Result struct for the background cosmology recalculation task
struct Catalogue3DCosmologyTaskResult
{
    bool valid{ false };
    QString error;
    QString model;
    // Computed comoving distances in Mpc, same order as the entries vector at
    // submission time.  Empty on error.
    std::vector<double> distancesMpc;
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
    explicit vtkWindowCatalogue3D(const QString &filepath, const QString &backendUrl,
                                  const QString &backendToken,
                                  const QStringList &paths = QStringList(),
                                  QWidget *parent = nullptr);
    ~vtkWindowCatalogue3D() override;

private slots:
    void toggleLabels(bool checked);
    void toggleMorphColors(bool checked);
    void scaleChanged(int value);
    void resetCamera();
    void updateGeometryMode();
    void updateSizeMode();
    void toggleSceneAxes(bool checked);
    void toggleBoundingBox(bool checked);
    void toggleShells(bool checked);
    void syncTableSelection(int index);

signals:
    void sourceSelectionChanged(int index);
    /// Emitted when a session-expiry error is detected and the user requests
    /// that the dataset be reopened.  Connect to MainWindow::doOpenCataloguePath.
    void reopenRequested(const QString &filepath, const QStringList &paths = QStringList());

private:
    enum class GeometryMode
    {
        Ellipsoid,
        Sphere,
        Point,
        Cross,
        Velocity,   // arrows oriented by vx/vy/vz, scaled by speed
    };

    enum class SizeMode
    {
        Fixed,
        MajorAxis,
        Lls,
        Flux,
    };

    // ── Sidebar ───────────────────────────────────────────────────────────────
    SidebarPanel *m_sidebar{ nullptr };
    QPointer<QAction> actionShowControlsSidebar;
    void setupSidebar();

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
    void setHoveredSource(int idx);
    void setSelectedSource(int idx);
    void centerOnSource(int idx, double zoomFactor = 3.0);

    // ── UI helpers ──────────────────────────────────────────────────────────
    void updateInfoPanel();
    // Reposition and resize a highlight sphere to sit on source[idx].
    void updateHighlightSphere(vtkSphereSource *sphere, vtkActor *actor, int idx,
                               double radiusFactor);

    // ── Coordinate frame ────────────────────────────────────────────────────
    enum class CoordFrame { FK5_J2000, Galactic };
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

    // ── Data ────────────────────────────────────────────────────────────────
    Ui::vtkWindowCatalogue3D *ui;
    QString filepath;
    QStringList m_cataloguePaths;   // multi-file open (>1 → concatenated); else empty/single
    QString backendUrl;
    QString backendToken;
    QString remoteDatasetId;
    QString remoteSessionId;
    std::vector<Catalogue3DEntry> entries;
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
    vtkNew<vtkGlyph3DMapper> glyphMapper;
    vtkNew<vtkActor> glyphActor;
    vtkNew<vtkLookupTable> morphologyLut;
    vtkNew<vtkLookupTable> valueLut;
    vtkNew<vtkPolyDataMapper> pointsMapper;
    vtkNew<vtkActor> pointsActor;

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
    QPointer<QDoubleSpinBox> spinColorGamma;   // exponent for the Power mode
    QPointer<QComboBox> comboGeometry;
    QPointer<QComboBox> comboSizeMode;
    QPointer<QComboBox> comboFrame;
    QPointer<QComboBox> comboCosmology;
    QPointer<QCheckBox> chkShowAxes;
    QPointer<QCheckBox> chkShowBoundingBox;
    QPointer<QCheckBox> chkShowShells;
    // Multi-filter UI
    QPointer<QListWidget> filterList;
    QPointer<QPushButton> btnFilterAdd;
    QPointer<QPushButton> btnFilterRemove;
    QPointer<QPushButton> btnFilterClearAll;
    QPointer<QPushButton> btnFilterApply;
    QPointer<QLabel> filterStatusLabel;
    std::vector<BackendTabularFilter> activeFilters;
    bool filterBusy{ false };
    QFutureWatcher<Catalogue3DFilterTaskResult> filterWatcher;
    // ── Pagination ────────────────────────────────────────────────────────────
    int m_currentOffset{ 0 };
    int m_pageSize{ 50000 };
    int m_totalCount{ -1 };
    bool m_loadingMore{ false };
    QPointer<QPushButton> btnLoadMore;
    QPointer<QPushButton> btnLoadAll;
    QFutureWatcher<Catalogue3DFilterTaskResult> m_loadMoreWatcher;
    void loadMoreEntries();
    void loadAllEntries();   // load every remaining row in one query (no paging)
    // ── Session-expiry auto-recovery ──────────────────────────────────────────
    QFutureWatcher<BackendCatalogueInfo> m_reopenWatcher;
    void autoReopenThenLoadMore();
    QString m_cosmologyModel{ QStringLiteral("Planck18") };
    bool m_cosmologyBusy{ false };
    QFutureWatcher<Catalogue3DCosmologyTaskResult> m_cosmologyWatcher;
    bool syncingTableSelection{ false };
    bool updatingMappingControls{ false };
    QString mapXKey{ QStringLiteral("computed:x") };
    QString mapYKey{ QStringLiteral("computed:y") };
    QString mapZKey{ QStringLiteral("computed:z") };
    QString mapSizeKey{ QStringLiteral("builtin:fixed") };
    QString mapColorKey{ QStringLiteral("builtin:morphology") };

    // Interaction state
    int hoveredIndex{ -1 };
    int selectedIndex{ -1 };
    // Left-button drag tracking (to distinguish click from camera rotate)
    bool leftButtonDown{ false };
    bool leftClickCandidate{ false };
    int pressPickIndex{ -1 };
    int pressX{ 0 };
    int pressY{ 0 };
    static constexpr int clickDragThresholdPx = 5;

    static constexpr double hoverThresholdPx = 14.0;
};

#endif // vtkWindowCatalogue3D_h

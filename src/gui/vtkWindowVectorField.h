#ifndef vtkWindowVectorField_h
#define vtkWindowVectorField_h

#include "app/BackendClient.h"
#include "Catalogue3DParser.h"

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <QFutureWatcher>
#include <QMainWindow>
#include <QPointer>
#include <QString>

#include <vector>

/// Result of the async galaxy-overlay fetch (built off the GUI thread): the
/// parsed catalogue positions + schema. The VTK actor is built from this on the
/// GUI thread in onCatalogueOverlayReady().
struct CatalogueOverlayData
{
    bool ok = false;
    QString error;
    QString name;                            // catalogue basename (for status)
    int totalRows = 0;                       // backend total (for the "capped" note)
    bool cartesian = false;                  // schema.isCartesian() after XYZ forcing
    Catalogue3DSchema schema;
    std::vector<Catalogue3DEntry> entries;
};

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QVTKOpenGLNativeWidget;
class vtkActor;
class vtkArrowSource;
class vtkBoxWidget2;
class vtkGlyph3DMapper;
class vtkImageData;
class vtkLookupTable;
class vtkOrientationMarkerWidget;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkScalarBarActor;

/// 3-D viewer for a cosmic-flow velocity field (F1): glyph arrows + streamlines
/// over a 3-component vector grid, coloured by speed. The display grid is a
/// backend LOD (<=128³); the user can box a region and pull it at FULL
/// resolution (ROI on demand). Rendering is local VTK so seeding + box scale
/// stay interactive.
class vtkWindowVectorField : public QMainWindow
{
    Q_OBJECT

public:
    explicit vtkWindowVectorField(const QString &path,
                                  BackendVelocityGridResult grid,
                                  const QString &datasetId,
                                  const QString &backendUrl,
                                  const QString &backendToken,
                                  const QString &sessionId,
                                  QWidget *parent = nullptr);
    ~vtkWindowVectorField() override = default;

private:
    void setupUi();
    void setupRenderer();
    void buildImageData(const BackendVelocityGridResult &grid,
                        vtkSmartPointer<vtkImageData> &out, double sx, double sy, double sz,
                        double ox, double oy, double oz);   // grid bytes -> vtkImageData(vectors+speed)
    // Build glyph / streamline actors for a given grid; returns a new actor.
    vtkSmartPointer<vtkActor> makeGlyphActor(vtkImageData *img, double cell);
    vtkSmartPointer<vtkActor> makeStreamActor(vtkImageData *img);
    void rebuildLodActors();
    void applyBoxSize(double boxMpc);   // rescale display spacing/origin in place
    void applyColorMap(const QString &name);
    void resetCamera();
    void toggleRoiWidget(bool on);
    void loadFullResRoi();
    void showBasins(bool on);   // F1 phase-2: gravitational-basin (watershed) regions
    void placeBasins();         // (re)build the basin actor from m_basins at the current scale

    // Density from the velocity divergence (δ ∝ −∇·v, linear theory): converging
    // flow (∇·v < 0) marks overdensities. An isosurface of the overdense cores.
    void showFieldDensity(bool on);
    void buildFieldDensity();          // ∇·v on the grid → cached field (both signs)
    bool buildFieldDensityFromBackend();  // higher-res ∇·v from the backend (LOD fields)
    void updateFieldDensityContour();  // (re)contour the convergent (overdense) cores
    void showFieldVoid(bool on);       // divergent side (∇·v > 0): voids / repellers
    void updateFieldVoidContour();

    // CosmicFlows combo: overlay a galaxy catalogue's point cloud in the same
    // Cartesian frame as the velocity field ("where galaxies sit in the flow").
    void overlayCatalogue();          // pick a catalogue, launch the async fetch
    void onCatalogueOverlayReady();   // build the point actor from the fetched data
    void setCatalogueVisible(bool on);
    void recolourCatalogue(const QString &key);   // colour galaxies by a catalogue attribute
    void setCatalogueShape(const QString &mode);  // Points vs Velocity arrows
    void buildCatalogueDensity();                 // bin galaxy points → smoothed density grid
    void updateCatalogueDensityContour();         // (re)contour the grid at the current level
    void showCatalogueDensity(bool on);           // overdensity isosurface on/off
    void clearCatalogueOverlay();                 // remove the overlay + reset its controls
    void showBulkFlowProfile();                   // bulk flow |⟨v⟩| vs radius (galaxies vs field)
    void showFieldBulkFlow();                     // the field's own bulk flow vs radius (no catalogue)

    // Per-axis full-resolution cell size + centred origin (point-centred model:
    // box>0 → box/(n-1) and -box/2; else unit spacing, -(n-1)/2). The display
    // (LOD) cell is fullCell * downsampleFactor.
    void fullCells(double &sx, double &sy, double &sz) const;
    void originVec(double &ox, double &oy, double &oz) const;

    QString m_path;
    BackendVelocityGridResult m_grid;    // the LOD display grid
    QString m_datasetId;
    QString m_backendUrl;
    QString m_backendToken;
    QString m_sessionId;
    double m_boxMpc = 0.0;               // 0 -> grid units

    QPointer<QVTKOpenGLNativeWidget> vtkWidget;
    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkNew<vtkRenderer> renderer;
    vtkSmartPointer<vtkOrientationMarkerWidget> axesWidget;
    vtkSmartPointer<vtkScalarBarActor> scalarBar;
    vtkSmartPointer<vtkLookupTable> lut;
    vtkSmartPointer<vtkBoxWidget2> roiWidget;

    vtkSmartPointer<vtkImageData> imageData;     // LOD display grid
    vtkSmartPointer<vtkActor> glyphActor;
    vtkSmartPointer<vtkActor> streamActor;
    vtkSmartPointer<vtkImageData> roiImageData;  // full-res ROI grid
    vtkSmartPointer<vtkActor> roiGlyphActor;
    vtkSmartPointer<vtkActor> roiStreamActor;
    vtkSmartPointer<vtkActor> basinActor;        // gravitational-basin surfaces
    vtkSmartPointer<vtkLookupTable> basinLut;    // categorical basin colours
    BackendVelocityBasinsResult m_basins;        // cached labels (re-placed on scale change)

    vtkSmartPointer<vtkImageData> divergenceGrid;  // cached ∇·v field (scalars); overdense = ∇·v<0
    vtkSmartPointer<vtkActor> divergenceActor;     // overdensity (convergent-core) isosurface
    vtkSmartPointer<vtkActor> voidActor;           // void (divergent-core) isosurface
    double m_divMin = 0.0;                          // most-negative ∇·v (peak overdensity)
    double m_divMax = 0.0;                          // most-positive ∇·v (peak void/divergence)

    vtkSmartPointer<vtkActor> catalogueActor;    // overlaid galaxy point cloud
    vtkSmartPointer<vtkPolyData> cataloguePoly;  // the galaxy points + attribute arrays
    vtkSmartPointer<vtkPolyDataMapper> cataloguePointsMapper;  // Points shape
    vtkSmartPointer<vtkGlyph3DMapper> catalogueGlyphMapper;    // Velocity arrows shape
    vtkSmartPointer<vtkArrowSource> catalogueArrow;
    vtkSmartPointer<vtkLookupTable> catalogueLut; // attribute colour ramp (lazy)
    vtkSmartPointer<vtkScalarBarActor> catalogueScalarBar;  // galaxy-attribute legend (left)
    bool m_catalogueHasVelocity = false;         // vx/vy/vz present → arrows available
    vtkSmartPointer<vtkImageData> catalogueDensityGrid;   // smoothed galaxy-density field (lazy)
    vtkSmartPointer<vtkActor> catalogueDensityActor;      // overdensity isosurface
    double m_catalogueDensityMax = 0.0;          // peak density → contour level = frac·max
    QString m_catalogueName;                     // basename, for the status label
    int m_catalogueCount = 0;                    // points drawn in the overlay
    QString m_summaryBase;                        // summary text before any overlay suffix
    QFutureWatcher<CatalogueOverlayData> m_catalogueWatcher;  // async overlay fetch

    // Sidebar controls
    QPointer<QCheckBox> chkGlyphs;
    QPointer<QCheckBox> chkStreamlines;
    QPointer<QSpinBox> spinSeeds;
    QPointer<QSpinBox> spinMaxArrows;
    QPointer<QDoubleSpinBox> spinGlyphScale;
    QPointer<QDoubleSpinBox> spinBoxSize;
    QPointer<QComboBox> comboLut;
    QPointer<QCheckBox> chkRoi;
    QPointer<QPushButton> btnLoadRoi;
    QPointer<QCheckBox> chkBasins;
    QPointer<QComboBox> comboBasinDir;   // Attraction (superclusters) / Repulsion (voids)
    QPointer<QCheckBox> chkFieldDensity;      // overdensity isosurface from ∇·v
    QPointer<QCheckBox> chkFieldVoid;         // void isosurface from ∇·v
    QPointer<QSlider> sliderFieldDensityLevel;
    QPointer<QPushButton> btnFieldBulkFlow;   // reconstruction bulk flow vs radius
    QPointer<QPushButton> btnOverlayCatalogue;
    QPointer<QPushButton> btnRemoveCatalogue;
    QPointer<QPushButton> btnBulkFlow;
    QPointer<QCheckBox> chkShowCatalogue;
    QPointer<QDoubleSpinBox> spinCataloguePointSize;
    QPointer<QComboBox> comboGalaxyColor;   // Uniform / numeric column / Speed |v|
    QPointer<QComboBox> comboGalaxyShape;   // Points / Velocity arrows
    QPointer<QCheckBox> chkGalaxyDensity;   // overdensity isosurface toggle
    QPointer<QSlider> sliderDensityLevel;   // isosurface level (% of peak)
    QPointer<QLabel> summaryLabel;
};

#endif

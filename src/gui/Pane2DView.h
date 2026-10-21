#ifndef Pane2DView_h
#define Pane2DView_h

// Pane2DView — a DISPLAY-ONLY 2-D image view (one moment / product map) suitable
// for mounting into a workspace pane. Modelled on vtkWindowCube::setupMomentRenderer()
// and its WCS-overlay machinery, but stripped of ALL interactive overlays (probe,
// region, PV, catalogue, beam, contours, noise preview) and fully decoupled from
// vtkWindowCube: the WCS axis/label math is supplied by the caller via closures.
//
// Geometry (gutter/fit/colorbar) reuses the shared CubeGeom helpers; the overlay
// actors are configured with the shared CubeWcs helpers, so this view lays out and
// labels its data area identically to the moment pane.

#include <QObject>
#include <QPointer>
#include <QString>

#include <array>
#include <functional>
#include <limits>
#include <vector>

#include <vtkSmartPointer.h>

#include "LutScale.h"

class QWidget;
class QVTKOpenGLNativeWidget;
class vtkActor;
class vtkCellArray;
class vtkPoints;
class vtkPolyData;

class vtkImageData;
class vtkTextProperty;
class vtkGenericOpenGLRenderWindow;
class vtkRenderer;
class vtkTrivialProducer;
class vtkImageMapToColors;
class vtkLookupTable;
class vtkImageSliceMapper;
class vtkImageSlice;
class vtkScalarBarActor;
class vtkAxisActor2D;
class vtkTextActor;
class vtkCallbackCommand;

class Pane2DView : public QObject
{
    Q_OBJECT
public:
    // Everything the WCS axis/label math needs, captured as closures so the class is
    // fully decoupled from vtkWindowCube. All are dataset-level (channel-independent).
    struct WcsContext
    {
        std::function<double(int axis, double voxelIndex, bool *ok)> voxelToWcs;
        std::function<bool()> hasCelestial;
        std::function<QString(int axis)> axisTitle;
        // axisSpan (in the axis' WCS units, e.g. degrees) lets the formatter pick enough
        // precision that adjacent ticks never print the SAME label on a tiny field ((b)).
        std::function<QString(int axis, double value, double axisSpan)> formatCoord;
        // Pixel (x, y) → coordinates in the DISPLAY frame, both at once. A frame
        // change (FK5 ↔ Galactic, …) mixes the two axes, so the per-axis
        // voxelToWcs alone gives native values under a frame's title. When set,
        // ticks use this; when unset, the native per-axis values.
        std::function<bool(double vx, double vy, double &fx, double &fy)> voxelToFrame;
    };

    explicit Pane2DView(QObject *parent = nullptr);
    ~Pane2DView() override;

    QVTKOpenGLNativeWidget *widget() const;                 // the mountable Qt widget
    vtkRenderer *renderer() const { return m_renderer; }
    // `destScale` is the mode the pane is ABOUT to be put in, when the caller
    // follows this with setColorMap(). It matters because the range is installed
    // here and read back by rebuildLut(): installed under the pane's OLD mode, a
    // negative minimum would be floored away before the new (linear) product
    // ever saw it. Omit it when only the pixels change.
    void setImageData(vtkImageData *img, double lo, double hi,
                      std::optional<LutScale::Mode> destScale = std::nullopt);
    void setColorMap(const QString &name, LutScale::Mode mode, double gamma,
                     bool clampOutliersTransparent);
    void setWcsContext(WcsContext ctx);
    void setWcsAxesVisible(bool on);
    /// Keep @p logicalPx free below the axes (a channel bar sits there).
    void setBottomReserve(int logicalPx);
    void fit(bool force = true);                            // gutter→fit→colorbar→overlay→render
    // §5.1: the interactor is LIVE only on the active pane, flipped in the activation
    // slot (not per-event) so no VTK observer ever sees a stray hover on an idle pane.
    void setInteractive(bool on);

    // ── §5.2/§5.3: per-pane LUT state the DISPLAY dock drives ──────────────────
    vtkLookupTable *lookupTable() const { return m_lut; }  // for the LUT customizer (Edit)
    vtkImageData *imageData() const;                        // the pane's current image
    QString colorMapName() const { return m_colorMapName; }
    LutScale::Mode scale() const { return m_scale; }
    double gamma() const { return m_gamma; }
    bool invert() const { return m_invert; }
    bool clampOutliers() const { return m_clampOutliers; }

    /// A polyline drawn over the image, in its pixel coordinates (the PV path, shown
    /// on every map of the same field). Empty = hidden. Renders.
    void setOverlayPath(const std::vector<std::array<double, 2>> &pixels);
    void render();
    double rangeMin() const;
    double rangeMax() const;
    bool lutInitialized() const { return m_lutInitialized; }  // §5.3: don't re-seed on recompute

    void setColorMapName(const QString &name);             // rebuild m_lut at the current range
    void setScale(LutScale::Mode mode);
    // §5.2: adopt state AFTER an external editor (the LUT customizer) mutated m_lut in
    // place — store name/scale/gamma so getters + Linked propagation aren't stale. No
    // rebuild (the dialog already built the table); just re-fit.
    void adoptLutState(const QString &name, LutScale::Mode scale, double gamma);
    // §5.5: apply a full colormap state (name+scale+gamma+invert) in ONE rebuild — used
    // by Linked propagation so a Power-stretch gamma follows the peers too.
    void applyLut(const QString &name, LutScale::Mode scale, double gamma, bool invert);
    void setInvert(bool on);
    void setRange(double lo, double hi);                   // authoritative TableRange → colorbar

    // Linked 2D: read/write the pan+zoom of the parallel camera (world focal x/y + parallel
    // scale). setCameraState applies without re-emitting cameraChanged (echo guard).
    void cameraState(double focalXY[2], double &parallelScale) const;
    void setCameraState(const double focalXY[2], double parallelScale);

    // STEP6-P3: gutter-INDEPENDENT view state — the world point shown at the DATA-AREA centre
    // and the world height that fills the data area. Unlike the raw focal/scale above, this
    // cancels each pane's own colorbar gutter, so copying it between panes with different LUT
    // ranges (hence different gutters) frames the SAME feature at the SAME zoom. Returns false
    // if the geometry isn't ready (no image / zero data area). setCameraStateNormalized is
    // echo-guarded like setCameraState.
    bool cameraStateNormalized(double dataCenterXY[2], double &worldHeight) const;
    void setCameraStateNormalized(const double dataCenterXY[2], double worldHeight);

signals:
    void rangeChanged(double lo, double hi);

    /// Emitted instead of switching when a log stretch was asked for on an image
    /// with no positive values. The pane keeps its current scale; the window
    /// decides how to tell the user.
    void logScaleUnavailable();   // pane's range changed (drag or dock) → dock syncs
    void lutChanged();                          // colormap / scale / invert changed → strip + Linked
    void cameraChanged();                       // pan/zoom via interaction → Linked-2D propagate
    void fitted();                              // fit() re-letterboxed → Linked-2D may re-assert

private:
    void updateWcsOverlay();                                // EndEvent target; uses m_wcs
    void rebuildLut();                                      // §5.2: apply name+scale+invert at range
    void ensureOverlayTickActors();
    void setOverlayVisible(bool visible);
    double dpr() const;

    // QPointer so it auto-nulls if the mounting pane deletes the widget before us.
    QPointer<QVTKOpenGLNativeWidget> m_widget;              // owned by Qt parent once mounted
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> m_window;
    vtkSmartPointer<vtkRenderer> m_renderer;

    vtkSmartPointer<vtkTrivialProducer> m_source;
    vtkSmartPointer<vtkImageMapToColors> m_colors;
    vtkSmartPointer<vtkLookupTable> m_lut;
    vtkSmartPointer<vtkImageSliceMapper> m_mapper;
    vtkSmartPointer<vtkImageSlice> m_actor;
    vtkSmartPointer<vtkScalarBarActor> m_colorbar;
    vtkSmartPointer<vtkPoints> m_pathPoints;
    vtkSmartPointer<vtkCellArray> m_pathCells;
    vtkSmartPointer<vtkPolyData> m_pathData;
    vtkSmartPointer<vtkActor> m_pathActor;
    std::vector<std::array<double, 2>> m_pathShown;   ///< skip re-renders of the same path

    // WCS overlay actors (x/y axis lines, x/y titles, per-tick label vectors).
    vtkSmartPointer<vtkAxisActor2D> m_overlayXAxis;
    vtkSmartPointer<vtkAxisActor2D> m_overlayYAxis;
    vtkSmartPointer<vtkTextActor> m_overlayXTitle;
    vtkSmartPointer<vtkTextActor> m_overlayYTitle;
    std::vector<vtkSmartPointer<vtkTextActor>> m_overlayXTicks;
    std::vector<vtkSmartPointer<vtkTextActor>> m_overlayYTicks;

    vtkSmartPointer<vtkCallbackCommand> m_endObserver;
    vtkSmartPointer<vtkCallbackCommand> m_cameraObserver;   // interactor Interaction/EndInteraction
    unsigned long m_camTagInteract{ 0 };                    // observer tags (removed in dtor)
    unsigned long m_camTagEnd{ 0 };
    bool m_applyingCamera{ false };                          // Linked-2D echo guard

    WcsContext m_wcs;
    bool m_showWcsAxes{ true };
    bool m_overlayInitialized{ false };

    // §5.2/§5.3: per-pane LUT state (authoritative on the pane, not re-read from the
    // shared product cache on every mount — so two panes on the same product can differ).
    QString m_colorMapName;
    /// Anchors the log floor from the data before a switch to Mode::Log; false
    /// when the image has no positive values, in which case the caller must
    /// leave the scale as it is. See the definition for why every path that
    /// assigns m_scale goes through it.
    bool anchorLogFloor(LutScale::Mode mode, bool force = false);

    /// The sampled log floor for the image currently in the pipeline; false when
    /// the image has no positive values and a log stretch is meaningless.
    bool logFloor(double &floorOut) const;

    LutScale::Mode m_scale{ LutScale::Mode::Linear };
    double m_gamma{ 2.0 };
    bool m_clampOutliers{ false };
    bool m_invert{ false };
    bool m_lutInitialized{ false };

    // fit cache (see vtkWindowCube::layout2dPaneFor)
    std::array<int, 2> m_fitSize{ -1, -1 };
    double m_fitRight{ -1. };
    double m_fitBottom{ -1. };
    int m_bottomReserve{ 0 };   ///< logical px kept free at the bottom (channel bar)

    // overlay cache (see vtkWindowCube::updateMomentWcsOverlay)
    std::array<double, 4> m_overlayVisibleBounds{ std::numeric_limits<double>::quiet_NaN(),
                                                  std::numeric_limits<double>::quiet_NaN(),
                                                  std::numeric_limits<double>::quiet_NaN(),
                                                  std::numeric_limits<double>::quiet_NaN() };
    std::array<int, 2> m_overlayViewportSize{ -1, -1 };
    unsigned long m_overlayCameraMTime{ 0 };
};

#endif  // Pane2DView_h

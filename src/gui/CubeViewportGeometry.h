#ifndef CubeViewportGeometry_h
#define CubeViewportGeometry_h

// Shared viewport-geometry helpers extracted verbatim from vtkWindowCube.cpp.
// Free functions (no class state) used by multiple overlays — catalogue,
// slice/moment WCS overlays, isosurface clipping — so they live in one place.

#include <array>

class vtkImageData;
class vtkRenderWindow;
class vtkRenderer;
class vtkScalarBarActor;

namespace CubeGeom {

struct VisibleImageBounds2D
{
    bool valid{ false };
    double xmin{ 0. };
    double xmax{ 0. };
    double ymin{ 0. };
    double ymax{ 0. };
};

// Distance between two integer pixel coordinates, and the click tolerance used
// to decide a polygon ROI has been closed. Shared by the probe + region code.
inline constexpr double polygonClosureTolerance = 3.0;
double distance2d(const std::array<int, 2> &a, const std::array<int, 2> &b);

bool validBounds(const double bounds[6]);
bool displayPointToWorld(vtkRenderer *renderer, double x, double y, double z, double world[3]);
bool intersectLineWithBounds(const double p0[3], const double p1[3], const double bounds[6],
                             double entry[3], double exit[3]);
bool computeViewportIntersectionBounds(vtkRenderer *renderer, vtkRenderWindow *window,
                                       const double cubeBounds[6], double bounds[6]);
VisibleImageBounds2D computeVisibleImageBounds2D(vtkRenderer *renderer, vtkImageData *imageData);
bool pointInVisibleBounds(const std::array<double, 2> &point, const VisibleImageBounds2D &visible);

// Colorbar geometry, in LOGICAL pixels (the caller multiplies by the device pixel
// ratio — the gutter lives in render-window / device px, §3.0b).
inline constexpr int    kColorbarLabelFontSize = 9;
inline constexpr double kColorbarBarWidth      = 19.;  // colour swatch
inline constexpr double kColorbarGap           = 14.;  // data area → bar
inline constexpr double kColorbarTextPad       =  6.;  // bar → label
inline constexpr double kColorbarMargin        =  8.;  // label → pane edge

// Pixel gutter reserved around the 2-D data area for WCS ticks, axis titles and
// the colorbar. `left` is the x of the y-axis line (tick labels sit to its left);
// `right` holds the colorbar + its value labels. All in DEVICE pixels once
// imageGutter() has scaled the authored (logical) numbers by the dpr.
struct ImageGutter
{
    double left{ 136. };
    double right{ 116. };   // overwritten with the measured colorbar column
    double bottom{ 58. };
    double top{ 28. };
};

/// Where the fitted image landed, in display pixels — the DATA AREA, which is
/// the gutter's inner box minus the letterbox. Everything that annotates the
/// data (colorbar, WCS axes, ticks) aligns to this, never to the inner box.
struct ImageRect
{
    bool valid{ false };
    double x{ 0. };
    double y{ 0. };
    double width{ 0. };
    double height{ 0. };
};

/// The gutter in force for the 2-D panes. Collapses to a thin inset when the WCS
/// overlay is off, but always keeps room for the colorbar. All fields are in DEVICE
/// pixels: @p dpr scales the authored (logical) margins, and @p rightGutter is the
/// caller-measured colorbar-column width (already device px), clamped so a
/// pathological unit (Moment 0 ~1e4) cannot eat the pane (§3.0b).
ImageGutter imageGutter(bool wcsAxesVisible, double rightGutter = 0., double dpr = 1.);

/// Where an aspect-preserved image lands inside the gutter, for a GIVEN viewport
/// size. Pure — no renderer state, no caching. Both the camera fit and the WCS
/// overlays call this, so they cannot disagree about where the data is (§2.7).
ImageRect dataAreaFor(const int size[2], vtkImageData *image, const ImageGutter &g);

/// Aspect-preserving parallel-projection fit of `image` into the gutter's inner
/// box. Sets ParallelScale + focal point; leaves the view direction alone. When
/// @p dataArea is non-null it reports where the image actually landed (data area).
bool fitImageInGutter(vtkRenderer *renderer, vtkImageData *image, const ImageGutter &g,
                      ImageRect *dataArea = nullptr);

/// Parks a vertical scalar bar in the right gutter, aligned to the data area (so
/// its top/bottom edges are level with the image, not the inner box).
void layoutColorbarInGutter(vtkScalarBarActor *bar, vtkRenderer *renderer,
                            const ImageGutter &g, const ImageRect &dataArea, double dpr = 1.);

/// STEP6-P3: gutter-INDEPENDENT 2-D parallel camera, so a pan/zoom copied between panes with
/// different gutters (different colorbar widths) frames the SAME feature at the SAME zoom.
/// readNormalizedCamera reads the renderer's camera as the world point at the DATA-AREA centre
/// (@p dataCenterXY) plus the world height that fills the data area (@p worldHeight).
/// normalizedToFocalScale is the pure inverse: given a target pane's viewport @p size + its own
/// @p dataArea, it produces the focal x/y + parallel scale to reproduce that view. Both return
/// false on degenerate geometry (invalid data area, zero height, non-positive scale/size).
bool readNormalizedCamera(vtkRenderer *renderer, const ImageRect &dataArea,
                          double dataCenterXY[2], double &worldHeight);
bool normalizedToFocalScale(const int size[2], const ImageRect &dataArea,
                            const double dataCenterXY[2], double worldHeight,
                            double focalXY[2], double &parallelScale);

}  // namespace CubeGeom

#endif  // CubeViewportGeometry_h

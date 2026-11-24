#pragma once

// Save View As… (issue #528): the picture of a 3-D view a user keeps.
//
// The scene is read back from the view's own render window (tiled when the
// picture is larger than the view, with or without a transparent background),
// and the colour bar, the axes box and the title are drawn over it with
// QPainter. A PNG gets them as pixels; a PDF or an SVG gets them as vectors
// over the render embedded as an image. (vtkGL2PSExporter would give vectors
// for the actors, but it cannot draw a ray-cast volume, and not every VTK build
// the application runs on has the GL2PS modules, so the render stays a raster.)
//
// No widgets here: the dialog and the windows are in src/gui.

#include "gui/RenderExportPlan.h"

#include <QImage>
#include <QPainter>
#include <QSize>
#include <QString>

#include <array>
#include <functional>
#include <vector>

class vtkCubeAxesActor;
class vtkOrientationMarkerWidget;
class vtkProp;
class vtkRenderer;
class vtkRenderWindow;

namespace visivo::renderexport {

/// The axes box of a view: its bounds, and per axis the title and the range
/// the labels read (bounds[2i]..bounds[2i+1] stand for lo..hi).
struct AxesBox
{
    bool valid = false;
    std::array<double, 6> bounds{};
    std::array<QString, 3> title;
    std::array<double, 3> lo{};
    std::array<double, 3> hi{};
};

/// The box of `actor`, invalid when it is null or hidden.
AxesBox axesBoxOf(vtkCubeAxesActor *actor);

/// A `hideAnnotations` for renderScene: hides `props` and the orientation
/// `marker` (either may be empty or null) and puts back what was visible.
std::function<void(bool)> sceneHider(std::vector<vtkProp *> props,
                                     vtkOrientationMarkerWidget *marker = nullptr);

/// What is drawn over the render.
struct Annotations
{
    QString title;
    /// The axes box, projected through `renderer`; drawn when valid.
    AxesBox box;
    vtkRenderer *renderer = nullptr;
    /// The view in the coordinates the colour bar is laid out in (the window's
    /// logical pixels) and the same in device pixels, which is the render
    /// window's size: the projection of the box is made in the second and drawn
    /// in the first.
    QSize logicalSize;
    QSize deviceSize;
    /// Paints the colour bar for a view of that logical size; empty for none.
    std::function<void(QPainter &, const QSize &)> colourbar;
};

/// Read the scene of `window` (its `renderer` is the one that draws the data)
/// back at `out` device pixels. `hideAnnotations(true)` is called before the
/// read-back and `(false)` after it, so the actors and widgets drawn in the
/// scene (axes, scalar bar, orientation marker) can be left out. The window is
/// rendered again with its own state at the end. Returns a null image and an
/// error text when the size is not allowed or the read-back is empty.
///
/// `transparent`: the background has alpha 0 (the result is ARGB32, not
/// premultiplied); else the result is opaque.
QImage renderScene(vtkRenderWindow *window, vtkRenderer *renderer, const QSize &out,
                   bool transparent, const std::function<void(bool)> &hideAnnotations,
                   QString *error);

/// Draw `annotations` on `painter`, which is scaled so that one unit is one
/// logical pixel of the view.
void paintAnnotations(QPainter &painter, const Annotations &annotations);

/// Write the picture: a PNG with the annotations in its pixels, or a PDF or an
/// SVG (page = the view's logical size) with `scene` embedded and the
/// annotations as vectors. `scene` may be at any size over the view's shape.
bool writePicture(const QString &path, Format format, const QImage &scene,
                  const Annotations &annotations, QString *error);

} // namespace visivo::renderexport

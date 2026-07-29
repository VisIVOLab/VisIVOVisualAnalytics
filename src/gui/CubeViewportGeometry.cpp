#include "CubeViewportGeometry.h"

#include <vtkCamera.h>
#include <vtkCoordinate.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkTextProperty.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace CubeGeom {

double distance2d(const std::array<int, 2> &a, const std::array<int, 2> &b)
{
    const double dx = static_cast<double>(a[0] - b[0]);
    const double dy = static_cast<double>(a[1] - b[1]);
    return std::sqrt(dx * dx + dy * dy);
}

bool validBounds(const double bounds[6])
{
    return std::isfinite(bounds[0]) && std::isfinite(bounds[1]) && std::isfinite(bounds[2])
            && std::isfinite(bounds[3]) && std::isfinite(bounds[4]) && std::isfinite(bounds[5])
            && bounds[0] <= bounds[1] && bounds[2] <= bounds[3] && bounds[4] <= bounds[5];
}

bool displayPointToWorld(vtkRenderer *renderer, double x, double y, double z, double world[3])
{
    if (!renderer) {
        return false;
    }

    renderer->SetDisplayPoint(x, y, z);
    renderer->DisplayToWorld();
    const double *worldPoint = renderer->GetWorldPoint();
    if (!worldPoint || std::fabs(worldPoint[3]) <= 1e-9) {
        return false;
    }

    world[0] = worldPoint[0] / worldPoint[3];
    world[1] = worldPoint[1] / worldPoint[3];
    world[2] = worldPoint[2] / worldPoint[3];
    return std::isfinite(world[0]) && std::isfinite(world[1]) && std::isfinite(world[2]);
}

bool intersectLineWithBounds(const double p0[3], const double p1[3], const double bounds[6],
                             double entry[3], double exit[3])
{
    double direction[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    double tMin = 0.0;
    double tMax = 1.0;

    for (int axis = 0; axis < 3; ++axis) {
        const int i0 = axis * 2;
        const int i1 = i0 + 1;
        if (std::fabs(direction[axis]) <= 1e-12) {
            if (p0[axis] < bounds[i0] || p0[axis] > bounds[i1]) {
                return false;
            }
            continue;
        }

        const double invDir = 1.0 / direction[axis];
        double t0 = (bounds[i0] - p0[axis]) * invDir;
        double t1 = (bounds[i1] - p0[axis]) * invDir;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }

    for (int axis = 0; axis < 3; ++axis) {
        entry[axis] = p0[axis] + tMin * direction[axis];
        exit[axis] = p0[axis] + tMax * direction[axis];
    }
    return true;
}

bool computeViewportIntersectionBounds(vtkRenderer *renderer, vtkRenderWindow *window,
                                       const double cubeBounds[6], double bounds[6])
{
    if (!renderer || !window || !validBounds(cubeBounds)) {
        return false;
    }

    const int *size = window->GetSize();
    if (!size || size[0] <= 1 || size[1] <= 1) {
        return false;
    }

    bounds[0] = bounds[2] = bounds[4] = std::numeric_limits<double>::infinity();
    bounds[1] = bounds[3] = bounds[5] = -std::numeric_limits<double>::infinity();
    const std::array<std::pair<double, double>, 9> samples = {{
            { 0.0, 0.0 },
            { static_cast<double>(size[0] - 1), 0.0 },
            { 0.0, static_cast<double>(size[1] - 1) },
            { static_cast<double>(size[0] - 1), static_cast<double>(size[1] - 1) },
            { 0.5 * (size[0] - 1), 0.0 },
            { 0.5 * (size[0] - 1), static_cast<double>(size[1] - 1) },
            { 0.0, 0.5 * (size[1] - 1) },
            { static_cast<double>(size[0] - 1), 0.5 * (size[1] - 1) },
            { 0.5 * (size[0] - 1), 0.5 * (size[1] - 1) },
    }};

    bool hasIntersection = false;
    for (const auto &[x, y] : samples) {
        double nearWorld[3];
        double farWorld[3];
        if (!displayPointToWorld(renderer, x, y, 0.0, nearWorld)
            || !displayPointToWorld(renderer, x, y, 1.0, farWorld)) {
            continue;
        }

        double entry[3];
        double exit[3];
        if (!intersectLineWithBounds(nearWorld, farWorld, cubeBounds, entry, exit)) {
            continue;
        }

        hasIntersection = true;
        for (int axis = 0; axis < 3; ++axis) {
            const int i0 = axis * 2;
            const int i1 = i0 + 1;
            bounds[i0] = std::min(bounds[i0], std::min(entry[axis], exit[axis]));
            bounds[i1] = std::max(bounds[i1], std::max(entry[axis], exit[axis]));
        }
    }

    return hasIntersection && validBounds(bounds);
}

VisibleImageBounds2D computeVisibleImageBounds2D(vtkRenderer *renderer, vtkImageData *imageData)
{
    VisibleImageBounds2D result;
    if (!renderer || !imageData || !renderer->GetActiveCamera()) {
        return result;
    }

    const int *size = renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0) {
        return result;
    }

    double bounds[6];
    imageData->GetBounds(bounds);
    auto *camera = renderer->GetActiveCamera();
    const double *focalPoint = camera->GetFocalPoint();
    const double halfHeight = camera->GetParallelScale();
    const double halfWidth = halfHeight * static_cast<double>(size[0]) / static_cast<double>(size[1]);

    result.xmin = std::max(bounds[0], focalPoint[0] - halfWidth);
    result.xmax = std::min(bounds[1], focalPoint[0] + halfWidth);
    result.ymin = std::max(bounds[2], focalPoint[1] - halfHeight);
    result.ymax = std::min(bounds[3], focalPoint[1] + halfHeight);
    result.valid = result.xmin <= result.xmax && result.ymin <= result.ymax;
    return result;
}

bool pointInVisibleBounds(const std::array<double, 2> &point, const VisibleImageBounds2D &visible)
{
    return visible.valid && point[0] >= visible.xmin && point[0] <= visible.xmax && point[1] >= visible.ymin
            && point[1] <= visible.ymax;
}

ImageGutter imageGutter(bool wcsAxesVisible, double rightGutter, double dpr)
{
    ImageGutter g;
    // Every field is in DEVICE pixels; the authored numbers are logical, hence ×dpr.
    if (!wcsAxesVisible) {
        g.left = g.bottom = g.top = 12. * dpr;
    } else {
        g.left *= dpr;
        g.bottom *= dpr;
        g.top *= dpr;
    }
    // `right` is the measured colorbar column (already device px); clamp in device px.
    g.right = std::clamp(rightGutter, 96. * dpr, 200. * dpr);
    return g;   // `right` (the colorbar's) survives the axes being hidden
}

ImageRect dataAreaFor(const int size[2], vtkImageData *image, const ImageGutter &g)
{
    ImageRect r;
    if (!size || size[0] <= 0 || size[1] <= 0 || !image) {
        return r;
    }
    double b[6];
    image->GetBounds(b);
    if (!validBounds(b)) {
        return r;
    }
    const double innerW = static_cast<double>(size[0]) - g.left - g.right;
    const double innerH = static_cast<double>(size[1]) - g.bottom - g.top;
    if (innerW < 16. || innerH < 16.) {
        return r;
    }
    // World units per screen pixel, aspect preserved: the BINDING axis is whichever
    // of the two is scarcer (width in a wide pane, height in a tall one).
    const double w = std::max(b[1] - b[0], 1e-9);
    const double h = std::max(b[3] - b[2], 1e-9);
    const double perPixel = std::max(w / (innerW - 2.), h / (innerH - 2.));
    r.valid = true;
    r.width = w / perPixel;
    r.height = h / perPixel;
    r.x = g.left + (innerW - r.width) / 2.;
    r.y = g.bottom + (innerH - r.height) / 2.;
    return r;
}

bool fitImageInGutter(vtkRenderer *renderer, vtkImageData *image, const ImageGutter &g,
                      ImageRect *dataArea)
{
    if (!renderer || !image) {
        return false;
    }
    const int *size = renderer->GetSize();
    const ImageRect r = dataAreaFor(size, image, g);   // one formula, shared with the overlays
    if (!r.valid) {
        return false;   // pane too small / bad bounds — leave the camera
    }
    double b[6];
    image->GetBounds(b);
    const double w = std::max(b[1] - b[0], 1e-9);
    const double perPixel = w / r.width;   // recovered from the shared rect

    auto *cam = renderer->GetActiveCamera();
    cam->ParallelProjectionOn();
    cam->SetParallelScale(perPixel * static_cast<double>(size[1]) / 2.);

    // Offset the focal point so the image centre lands at the DATA AREA centre
    // rather than the viewport centre — this is what puts the letterbox bands
    // around the whole group instead of between the image and its own axis.
    const double dxPx = (r.x + r.width / 2.) - static_cast<double>(size[0]) / 2.;
    const double dyPx = (r.y + r.height / 2.) - static_cast<double>(size[1]) / 2.;
    const double cx = (b[0] + b[1]) / 2. - dxPx * perPixel;
    const double cy = (b[2] + b[3]) / 2. - dyPx * perPixel;
    const double cz = (b[4] + b[5]) / 2.;

    double distance = cam->GetDistance();
    if (!std::isfinite(distance) || distance <= 0.) {
        distance = 1.;
    }
    cam->SetFocalPoint(cx, cy, cz);
    cam->SetPosition(cx, cy, cz + distance);
    cam->SetViewUp(0., 1., 0.);
    renderer->ResetCameraClippingRange();

    if (dataArea) {
        *dataArea = r;
    }
    return true;
}

void layoutColorbarInGutter(vtkScalarBarActor *bar, vtkRenderer *renderer,
                            const ImageGutter &g, const ImageRect &dataArea, double dpr)
{
    if (!bar || !renderer) {
        return;
    }
    const int *size = renderer->GetSize();
    if (!size || size[0] <= 0 || size[1] <= 0) {
        return;
    }
    const double W = static_cast<double>(size[0]);
    const double H = static_cast<double>(size[1]);
    // All device px (renderer->GetSize() is device px); authored numbers × dpr.
    const double gap = kColorbarGap * dpr;      // data area → bar
    const double barW = kColorbarBarWidth * dpr;
    const double pad = kColorbarTextPad * dpr;
    const double boxW = std::max(g.right - gap - kColorbarMargin * dpr, barW + pad + 8. * dpr);
    // Level with the image (data area), not the inner box — otherwise the bar's
    // top/bottom fifths map values onto the letterbox bands.
    const double barY = dataArea.valid ? dataArea.y : g.bottom;
    const double boxH = std::max(dataArea.valid ? dataArea.height : H - g.bottom - g.top, 1.);

    bar->SetOrientationToVertical();
    bar->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    bar->SetPosition((W - g.right + gap) / W, barY / H);
    bar->SetWidth(boxW / W);
    bar->SetHeight(boxH / H);
    bar->SetMaximumWidthInPixels(static_cast<int>(boxW));
    bar->SetMaximumHeightInPixels(static_cast<int>(boxH));
    bar->SetNumberOfLabels(5);
    bar->SetLabelFormat("%.3g");   // 0.0386 → "0.0386"; exact extrema live in STATISTICS
    bar->SetTitle("");
    bar->UnconstrainedFontSizeOn();
    // Pin the swatch to a fixed width instead of inheriting the default BarRatio
    // (0.375): the label column is then exactly what gutter2dFor() measured, so both
    // sides of the gap are sized by the same code and the value can't clip (§3.0b).
    bar->SetBarRatio(std::clamp(barW / boxW, 0.05, 0.9));
    bar->SetTextPad(static_cast<int>(pad));
    if (auto *tp = bar->GetLabelTextProperty()) {
        tp->SetFontSize(kColorbarLabelFontSize);   // VTK scales by DPI itself
        tp->SetColor(0.85, 0.90, 0.95);
        tp->BoldOff();
        tp->ShadowOff();
        tp->SetJustificationToLeft();
    }
}

}  // namespace CubeGeom

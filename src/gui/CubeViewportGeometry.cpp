#include "CubeViewportGeometry.h"

#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace CubeGeom {

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

}  // namespace CubeGeom

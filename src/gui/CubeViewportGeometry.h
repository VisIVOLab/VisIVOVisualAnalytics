#ifndef CubeViewportGeometry_h
#define CubeViewportGeometry_h

// Shared viewport-geometry helpers extracted verbatim from vtkWindowCube.cpp.
// Free functions (no class state) used by multiple overlays — catalogue,
// slice/moment WCS overlays, isosurface clipping — so they live in one place.

#include <array>

class vtkImageData;
class vtkRenderWindow;
class vtkRenderer;

namespace CubeGeom {

struct VisibleImageBounds2D
{
    bool valid{ false };
    double xmin{ 0. };
    double xmax{ 0. };
    double ymin{ 0. };
    double ymax{ 0. };
};

bool validBounds(const double bounds[6]);
bool displayPointToWorld(vtkRenderer *renderer, double x, double y, double z, double world[3]);
bool intersectLineWithBounds(const double p0[3], const double p1[3], const double bounds[6],
                             double entry[3], double exit[3]);
bool computeViewportIntersectionBounds(vtkRenderer *renderer, vtkRenderWindow *window,
                                       const double cubeBounds[6], double bounds[6]);
VisibleImageBounds2D computeVisibleImageBounds2D(vtkRenderer *renderer, vtkImageData *imageData);
bool pointInVisibleBounds(const std::array<double, 2> &point, const VisibleImageBounds2D &visible);

}  // namespace CubeGeom

#endif  // CubeViewportGeometry_h

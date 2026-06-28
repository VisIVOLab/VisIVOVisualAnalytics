#ifndef CubeRegionGeom_h
#define CubeRegionGeom_h

// Shared ROI hit-testing + annulus-fill helpers, identical between the cube and
// image viewers (deduplicated). Free functions, no class state / no RegionMode.

#include <array>
#include <vector>

class vtkCellArray;
class vtkPoints;
class vtkPolyData;

namespace CubeRegionGeom {

bool pointInBox(const std::array<int, 2> &anchor, const std::array<int, 2> &current, int x, int y);
bool pointInCircle(const std::array<int, 2> &anchor, const std::array<int, 2> &current, int x, int y);
bool pointInPolygon(const std::vector<std::array<int, 2>> &vertices, int x, int y);
bool pointInAnnulus(const std::array<int, 2> &center, const std::array<int, 2> &outerPoint,
                    double innerRadius, int x, int y);
void buildAnnulusFill(vtkPoints *points, vtkCellArray *cells, vtkPolyData *polyData,
                      const std::array<int, 2> &center, double innerRadius, double outerRadius,
                      int sides);

}  // namespace CubeRegionGeom

#endif

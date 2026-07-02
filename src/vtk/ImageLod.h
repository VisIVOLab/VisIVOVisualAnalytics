#ifndef ImageLod_h
#define ImageLod_h

#include <QVector>
#include <QPair>

/// Pure level-of-detail math for the 2-D image tile pyramid — the "brain" of
/// viewport-driven tile streaming, kept free of VTK/Qt-GUI so it is unit
/// testable. The viewer converts camera state into a visible source-pixel
/// rectangle + viewport size and asks this module which pyramid level to show
/// and which tiles cover the view. See docs/backend-api.md (`/v1/image/tile`).
namespace ImageLod {

/// Geometry of the pyramid for one image (mirrors the backend tile contract:
/// level 0 = full res, level L decimates by 2^L; tiles are ``tileSize`` px).
struct TileGrid
{
    int fullWidth{ 0 };
    int fullHeight{ 0 };
    int tileSize{ 256 };
    int numLevels{ 1 };
};

/// Pick the pyramid level so one displayed pixel ≈ one screen pixel:
/// level = clamp(round(log2(sourcePerScreen)), 0, numLevels-1), where
/// sourcePerScreen = visibleSourceHeightPx / viewportHeightPx. Zoomed in
/// (few source px per screen px) → finer level (→0); zoomed out → coarser.
int chooseLevel(double visibleSourceHeightPx, double viewportHeightPx, int numLevels);

/// Number of tiles per axis at ``level`` (matches the backend's tiling of the
/// source): cols = ceil(fullWidth / (tileSize·2^level)), likewise rows.
QPair<int, int> tileCounts(int level, const TileGrid &grid);

/// Tiles (tx, ty) at ``level`` covering the source-pixel rectangle
/// [x0,x1]×[y0,y1] (any corner order), expanded by ``margin`` tiles and
/// clamped to the grid. Returned in row-major order, unique.
QVector<QPair<int, int>> visibleTiles(double x0, double y0, double x1, double y1,
                                      int level, const TileGrid &grid, int margin = 1);

}  // namespace ImageLod

#endif  // ImageLod_h

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

/// Coarsest-necessary level to assemble the WHOLE image within a pixel budget:
/// the smallest level L whose decimated dims ceil(w/2^L)·ceil(h/2^L) ≤
/// maxPixels. Level 0 (full res) when the image already fits. Used so a huge
/// mosaic loads at a bounded resolution instead of materialising every full-res
/// tile (the display is downsampled anyway). Clamped to [0, numLevels-1].
int chooseAssemblyLevel(int fullWidth, int fullHeight, int numLevels, long long maxPixels);

/// Tiles (tx, ty) at ``level`` covering the source-pixel rectangle
/// [x0,x1]×[y0,y1] (any corner order), expanded by ``margin`` tiles and
/// clamped to the grid. Returned in row-major order, unique.
QVector<QPair<int, int>> visibleTiles(double x0, double y0, double x1, double y1,
                                      int level, const TileGrid &grid, int margin = 1);

/// Everything the viewer needs to assemble ONE sub-image covering the visible
/// source rectangle at the level-of-detail chosen for the current zoom. All
/// pixel coordinates are FULL-RES source pixels unless noted. Feeds the VTK
/// image: SetExtent(0,width-1, 0,height-1), SetOrigin(originX,originY,0),
/// SetSpacing(spacing,spacing,1) — so a decimated voxel v maps to full-res
/// pixel originX + v*spacing, keeping every coordinate readout / overlay
/// correct (same convention the whole-image assembly uses).
struct ViewportAssembly
{
    int level{ 0 };
    int txMin{ 0 }, tyMin{ 0 }, txMax{ 0 }, tyMax{ 0 };  ///< inclusive tile range at level
    int tileOriginX{ 0 }, tileOriginY{ 0 };  ///< level-pixel offset of the union (txMin*tileSize)
    int width{ 0 }, height{ 0 };             ///< assembled sub-image dims, in level pixels
    double originX{ 0.0 }, originY{ 0.0 };   ///< full-res pixel offset of the union
    double spacing{ 1.0 };                   ///< full-res px per level px == 2^level
    bool valid{ false };
};

/// The affine mapping a decimated display grid -> FULL-RES source pixels:
///   source = origin + displayedIndex * scale
/// Every overlay, coordinate readout and WCS label in the image viewer reads the
/// displayed image's own VTK origin/spacing, so getting these two numbers right
/// at load time is what makes all of them right. They must mirror how the
/// BACKEND sampled, which is why they live here next to the tile contract
/// rather than being re-derived at each call site (they were, and drifted).
struct SourceMapping
{
    double scaleX{ 1.0 }, scaleY{ 1.0 };
    double originX{ 0.0 }, originY{ 0.0 };
};

/// Whole-image assembly from pyramid tiles at ``level``. A SAMPLED tile is the
/// backend reading ``data[y0:y_stop:step, x0:x_stop:step]`` with
/// ``step = 2^level`` and ``x0 = tileX*tileSize*step``, so displayed pixel i IS
/// source pixel i*step. An AVERAGED tile (the image's pyramid, issue #85) holds
/// the mean of source pixels i*step .. (i+1)*step - 1, so its centre is
/// averagedOffset(level) further on. Exact - no interpolation, no endpoint
/// stretching.
SourceMapping assemblyMapping(int level, bool averaged = false);

/// Where an averaged pixel of ``level`` sits relative to a sampled one, in
/// source pixels: the centre of its 2^level cell, (2^level - 1) / 2.
double averagedOffset(int level);

/// Backend preview (``_build_preview``): displayed pixel i holds source pixel
/// ``floor((i + 0.5) * full / preview)``. The cell is ``full/preview`` source
/// pixels wide and the first cell's CENTRE sits at ``(full/preview - 1)/2``, so
/// that is the affine mapping. (The old endpoint-aligned
/// ``(full-1)/(preview-1)`` with origin 0 pinned the first and last displayed
/// pixels to source 0 and full-1, which is where the sampler never put them:
/// it offset every coordinate by about half a preview cell.)
SourceMapping previewMapping(int fullWidth, int fullHeight, int previewWidth, int previewHeight);

/// Compute the ViewportAssembly for the visible source rectangle [x0,x1]×[y0,y1]
/// (full-res px, any corner order) given the viewport height in screen px. The
/// level is chosen so one displayed pixel ≈ one screen pixel; the tile set is
/// the visible tiles + ``margin`` ring, clamped to the image. Returns {valid=false}
/// if the grid is degenerate or the rectangle does not intersect the image.
ViewportAssembly viewportAssembly(double x0, double y0, double x1, double y1,
                                  double viewportHeightPx, const TileGrid &grid, int margin = 1);

}  // namespace ImageLod

#endif  // ImageLod_h

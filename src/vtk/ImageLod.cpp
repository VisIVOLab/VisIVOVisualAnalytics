#include "ImageLod.h"

#include <algorithm>
#include <cmath>

namespace ImageLod {

int chooseLevel(double visibleSourceHeightPx, double viewportHeightPx, int numLevels)
{
    const int maxLevel = std::max(0, numLevels - 1);
    if (viewportHeightPx <= 0.0 || visibleSourceHeightPx <= 0.0) {
        return 0;
    }
    const double sourcePerScreen = visibleSourceHeightPx / viewportHeightPx;
    if (sourcePerScreen <= 1.0) {
        return 0;  // one source pixel per screen pixel (or finer) → full res
    }
    const int level = static_cast<int>(std::lround(std::log2(sourcePerScreen)));
    return std::clamp(level, 0, maxLevel);
}

QPair<int, int> tileCounts(int level, const TileGrid &grid)
{
    const int lvl = std::clamp(level, 0, std::max(0, grid.numLevels - 1));
    const int ts = std::max(1, grid.tileSize);
    const long long span = static_cast<long long>(ts) << lvl;  // source px per tile
    const auto ceilDiv = [](long long a, long long b) -> int {
        return static_cast<int>((a + b - 1) / b);
    };
    const int cols = grid.fullWidth > 0 ? ceilDiv(grid.fullWidth, span) : 0;
    const int rows = grid.fullHeight > 0 ? ceilDiv(grid.fullHeight, span) : 0;
    return { cols, rows };
}

int chooseAssemblyLevel(int fullWidth, int fullHeight, int numLevels, long long maxPixels)
{
    const int maxLevel = std::max(0, numLevels - 1);
    if (fullWidth <= 0 || fullHeight <= 0 || maxPixels <= 0) {
        return maxLevel;
    }
    for (int level = 0; level <= maxLevel; ++level) {
        const long long step = 1LL << level;
        const long long dw = (static_cast<long long>(fullWidth) + step - 1) / step;
        const long long dh = (static_cast<long long>(fullHeight) + step - 1) / step;
        if (dw * dh <= maxPixels) {
            return level;
        }
    }
    return maxLevel;
}

QVector<QPair<int, int>> visibleTiles(double x0, double y0, double x1, double y1,
                                      int level, const TileGrid &grid, int margin)
{
    QVector<QPair<int, int>> out;
    const auto counts = tileCounts(level, grid);
    const int cols = counts.first;
    const int rows = counts.second;
    if (cols <= 0 || rows <= 0) {
        return out;
    }
    const int lvl = std::clamp(level, 0, std::max(0, grid.numLevels - 1));
    const int ts = std::max(1, grid.tileSize);
    const double span = static_cast<double>(static_cast<long long>(ts) << lvl);
    const int m = std::max(0, margin);

    // Clamp the requested rectangle to the image, tolerant of corner order.
    double lox = std::min(x0, x1);
    double hix = std::max(x0, x1);
    double loy = std::min(y0, y1);
    double hiy = std::max(y0, y1);
    lox = std::clamp(lox, 0.0, static_cast<double>(grid.fullWidth - 1));
    hix = std::clamp(hix, 0.0, static_cast<double>(grid.fullWidth - 1));
    loy = std::clamp(loy, 0.0, static_cast<double>(grid.fullHeight - 1));
    hiy = std::clamp(hiy, 0.0, static_cast<double>(grid.fullHeight - 1));

    const int txMin = std::clamp(static_cast<int>(std::floor(lox / span)) - m, 0, cols - 1);
    const int txMax = std::clamp(static_cast<int>(std::floor(hix / span)) + m, 0, cols - 1);
    const int tyMin = std::clamp(static_cast<int>(std::floor(loy / span)) - m, 0, rows - 1);
    const int tyMax = std::clamp(static_cast<int>(std::floor(hiy / span)) + m, 0, rows - 1);

    out.reserve((txMax - txMin + 1) * (tyMax - tyMin + 1));
    for (int ty = tyMin; ty <= tyMax; ++ty) {
        for (int tx = txMin; tx <= txMax; ++tx) {
            out.append({ tx, ty });
        }
    }
    return out;
}

ViewportAssembly viewportAssembly(double x0, double y0, double x1, double y1,
                                  double viewportHeightPx, const TileGrid &grid, int margin)
{
    ViewportAssembly va;
    const int ts = std::max(1, grid.tileSize);
    if (grid.fullWidth <= 0 || grid.fullHeight <= 0) {
        return va;  // valid == false
    }

    const double visibleSourceHeightPx = std::abs(y1 - y0);
    const int lvl = std::clamp(chooseLevel(visibleSourceHeightPx, viewportHeightPx, grid.numLevels),
                               0, std::max(0, grid.numLevels - 1));
    const long long step = 1LL << lvl;
    const long long span = static_cast<long long>(ts) << lvl;  // full-res px per tile

    const auto counts = tileCounts(lvl, grid);
    const int cols = counts.first;
    const int rows = counts.second;
    if (cols <= 0 || rows <= 0) {
        return va;
    }

    // Reject a rectangle that does not intersect the image at all (fully panned
    // away): the caller should keep its current view rather than fetch an edge
    // tile. Corner-order tolerant.
    const double rminx = std::min(x0, x1), rmaxx = std::max(x0, x1);
    const double rminy = std::min(y0, y1), rmaxy = std::max(y0, y1);
    if (rmaxx < 0.0 || rminx > grid.fullWidth - 1.0
        || rmaxy < 0.0 || rminy > grid.fullHeight - 1.0) {
        return va;  // valid == false
    }

    // Clamp the requested rectangle to the image (tolerant of corner order).
    const double lox = std::clamp(std::min(x0, x1), 0.0, static_cast<double>(grid.fullWidth - 1));
    const double hix = std::clamp(std::max(x0, x1), 0.0, static_cast<double>(grid.fullWidth - 1));
    const double loy = std::clamp(std::min(y0, y1), 0.0, static_cast<double>(grid.fullHeight - 1));
    const double hiy = std::clamp(std::max(y0, y1), 0.0, static_cast<double>(grid.fullHeight - 1));

    const int m = std::max(0, margin);
    const double spanD = static_cast<double>(span);
    va.txMin = std::clamp(static_cast<int>(std::floor(lox / spanD)) - m, 0, cols - 1);
    va.txMax = std::clamp(static_cast<int>(std::floor(hix / spanD)) + m, 0, cols - 1);
    va.tyMin = std::clamp(static_cast<int>(std::floor(loy / spanD)) - m, 0, rows - 1);
    va.tyMax = std::clamp(static_cast<int>(std::floor(hiy / spanD)) + m, 0, rows - 1);

    // Whole-image dims at this level (so the last tile is clipped, not padded).
    const int downW = static_cast<int>((static_cast<long long>(grid.fullWidth) + step - 1) / step);
    const int downH = static_cast<int>((static_cast<long long>(grid.fullHeight) + step - 1) / step);

    va.level = lvl;
    va.tileOriginX = va.txMin * ts;
    va.tileOriginY = va.tyMin * ts;
    va.width  = std::min((va.txMax + 1) * ts, downW) - va.tileOriginX;
    va.height = std::min((va.tyMax + 1) * ts, downH) - va.tileOriginY;
    va.originX = static_cast<double>(va.txMin) * spanD;   // == tileOriginX * step
    va.originY = static_cast<double>(va.tyMin) * spanD;
    va.spacing = static_cast<double>(step);
    va.valid = (va.width > 0 && va.height > 0);
    return va;
}

double averagedOffset(int level)
{
    return (static_cast<double>(1 << std::max(0, level)) - 1.0) / 2.0;
}

SourceMapping assemblyMapping(int level, bool averaged)
{
    const double step = static_cast<double>(1 << std::max(0, level));
    const double origin = averaged ? averagedOffset(level) : 0.0;
    return { step, step, origin, origin };
}

SourceMapping previewMapping(int fullWidth, int fullHeight, int previewWidth, int previewHeight)
{
    SourceMapping m;
    if (previewWidth > 0 && fullWidth > 0) {
        m.scaleX = static_cast<double>(fullWidth) / static_cast<double>(previewWidth);
        m.originX = (m.scaleX - 1.0) / 2.0;
    }
    if (previewHeight > 0 && fullHeight > 0) {
        m.scaleY = static_cast<double>(fullHeight) / static_cast<double>(previewHeight);
        m.originY = (m.scaleY - 1.0) / 2.0;
    }
    return m;
}

}  // namespace ImageLod

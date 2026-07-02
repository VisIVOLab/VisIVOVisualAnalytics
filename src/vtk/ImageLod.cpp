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

}  // namespace ImageLod

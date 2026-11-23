#pragma once
// The samples a remote channel is shown with (issue #381).
//
// A wide channel (7233 x 7233 float32 is 209 MB) is asked for every `stride`
// pixels so neither side exceeds a budget; the backend picks the stride the
// same way (app/compute/cube.py, plane_view). When the 2-D view is zoomed in
// far enough that the part on screen, at a finer stride, still fits the
// budget, that part is asked for again and drawn over the coarse channel.
// GUI/VTK-free so the rules are tested on their own.

#include <QRect>

#include <algorithm>
#include <cmath>
#include <optional>

namespace visivo {

/// Longest side of the samples a remote channel is asked with: a few MB on the
/// wire, about the pixels of a large 2-D pane.
inline constexpr int kRemoteSliceLongestSide = 2048;

/// The smallest integer stride that keeps both sides of a w x h box within
/// @p maxLongestSide (0: every pixel).
inline int sliceStrideFor(int width, int height, int maxLongestSide)
{
    const int longest = std::max(width, height);
    if (maxLongestSide <= 0 || longest <= 0)
        return 1;
    return std::max(1, (longest + maxLongestSide - 1) / maxLongestSide);
}

/// A detail request: the inclusive plane pixels @p region read every @p stride.
struct SliceDetailWindow
{
    QRect region;
    int stride{ 1 };
};

/// The plane pixels of the world rectangle [wx0, wx1] x [wy0, wy1] (the world
/// frame is plane pixels), clamped to a fullWidth x fullHeight plane; empty
/// when the rectangle misses the plane.
inline QRect visiblePlanePixels(double wx0, double wx1, double wy0, double wy1, int fullWidth,
                                int fullHeight)
{
    if (!(std::isfinite(wx0) && std::isfinite(wx1) && std::isfinite(wy0) && std::isfinite(wy1))
        || fullWidth <= 0 || fullHeight <= 0)
        return {};
    const double lox = std::min(wx0, wx1), hix = std::max(wx0, wx1);
    const double loy = std::min(wy0, wy1), hiy = std::max(wy0, wy1);
    const int x0 = std::max(0, static_cast<int>(std::floor(std::max(lox, -1.0))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::max(loy, -1.0))));
    const int x1 = std::min(fullWidth - 1,
                            static_cast<int>(std::ceil(std::min(hix, double(fullWidth)))));
    const int y1 = std::min(fullHeight - 1,
                            static_cast<int>(std::ceil(std::min(hiy, double(fullHeight)))));
    if (x0 > x1 || y0 > y1)
        return {};
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

/// The detail to ask for when @p visible (plane pixels) is on screen over a
/// channel shown every @p coarseStride pixels: the visible part grown by
/// @p margin of its size on every side (a little panning needs no new fetch),
/// at the stride that fits @p budget. Nothing when that stride is no finer
/// than the coarse one.
inline std::optional<SliceDetailWindow> sliceDetailFor(const QRect &visible, int fullWidth,
                                                       int fullHeight, int coarseStride,
                                                       int budget = kRemoteSliceLongestSide,
                                                       double margin = 0.25)
{
    if (!visible.isValid() || coarseStride <= 1 || fullWidth <= 0 || fullHeight <= 0)
        return std::nullopt;
    const int mx = static_cast<int>(std::ceil(visible.width() * margin));
    const int my = static_cast<int>(std::ceil(visible.height() * margin));
    const QRect grown = visible.adjusted(-mx, -my, mx, my)
                                .intersected(QRect(0, 0, fullWidth, fullHeight));
    if (!grown.isValid())
        return std::nullopt;
    const int stride = sliceStrideFor(grown.width(), grown.height(), budget);
    if (stride >= coarseStride)
        return std::nullopt;
    return SliceDetailWindow{ grown, stride };
}

/// True when a detail @p have already shows @p visible at least as finely as
/// @p want would.
inline bool sliceDetailCovers(const SliceDetailWindow &have, const QRect &visible,
                              const SliceDetailWindow &want)
{
    return have.region.contains(visible) && have.stride <= want.stride;
}

}  // namespace visivo

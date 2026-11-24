#pragma once

// ChannelMapsLayout.h
// How big the panels of the Channel Maps window are (issue #502).
//
// The window used to give every panel a 160 px minimum and let the grid
// stretch them: eight columns need 1280 px, so in a 1200 px window the grid
// scrolled sideways, "8 columns" showed 7 and the eighth was cut off, and a
// panel took whatever shape its cell had. Here the image area of a panel is
// sized from the room there is, so the requested number of columns always
// fits, and it keeps the aspect of the image (a sky pixel is square). The axis
// margins are the same for every panel of a column, so the panels of a row show
// the image at one scale.
//
// Header-only and free of Qt Widgets, so it is tested without a window
// (TestChannelMapsLayout).

#include <algorithm>
#include <cmath>

namespace visivo::cmlayout {

constexpr int kGridMargin = 8;     ///< around the grid, inside the scroll area
constexpr int kGridSpacing = 4;    ///< between two panels
constexpr int kLeftAxis = 40;      ///< room for the y tick labels, first column only
constexpr int kInnerPad = 4;       ///< image to panel edge where no axis is drawn
constexpr int kBottomAxis = 20;    ///< room for the x tick labels, last row only
constexpr int kMinImageWidth = 48; ///< below this the panels scroll rather than shrink
constexpr int kWantedImageWidth = 140;   ///< what a new window is sized for

/// Width of the image area of one panel such that @p cols columns fit in a
/// viewport @p viewportWidth px wide.
inline int imageWidth(int viewportWidth, int cols)
{
    cols = std::max(1, cols);
    const int spent = 2 * kGridMargin + kGridSpacing * (cols - 1) + kLeftAxis
            + (2 * cols - 1) * kInnerPad;
    return std::max(kMinImageWidth, (viewportWidth - spent) / cols);
}

/// Height of the image area for an image of @p w × @p h pixels shown
/// @p imageW px wide: the image's own aspect.
inline int imageHeight(int imageW, int w, int h)
{
    if (w <= 0 || h <= 0)
        return imageW;
    return std::max(1, int(std::lround(double(imageW) * double(h) / double(w))));
}

/// The viewport width at which every one of @p cols columns gets an image area
/// of @p wantedImageW px.
inline int viewportWidthFor(int cols, int wantedImageW = kWantedImageWidth)
{
    cols = std::max(1, cols);
    return 2 * kGridMargin + kGridSpacing * (cols - 1) + kLeftAxis + (2 * cols - 1) * kInnerPad
            + cols * wantedImageW;
}

/// The whole panel (image, its axis margins, the title above) for the panel in
/// column @p col of row @p row of @p rows. @p titleHeight is the height of the
/// title row.
struct PanelSize
{
    int width{ 0 };
    int height{ 0 };
    int marginLeft{ 0 };
    int marginRight{ 0 };
    int marginBottom{ 0 };
};

inline PanelSize panelSize(int col, int row, int rows, int imageW, int imageH, int titleHeight)
{
    PanelSize p;
    p.marginLeft = col == 0 ? kLeftAxis : kInnerPad;
    p.marginRight = kInnerPad;
    p.marginBottom = row == rows - 1 ? kBottomAxis : kInnerPad;
    p.width = imageW + p.marginLeft + p.marginRight;
    p.height = imageH + p.marginBottom + titleHeight;
    return p;
}

}   // namespace visivo::cmlayout

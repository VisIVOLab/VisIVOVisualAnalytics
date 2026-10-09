#ifndef LayerCardGeometry_h
#define LayerCardGeometry_h

// LayerCardGeometry.h
// Where the pieces of a layer card sit inside its row.
//
// Separated from the delegate that draws them, and written in terms of QRect
// alone, so the arithmetic can be checked without a window: the two things that
// go wrong here are a click target that does not cover what it looks like it
// covers, and a gradient strip that turns inside-out when the dock is dragged
// narrow — neither of which is visible in a screenshot of the default width.

#include <QRect>

#include <algorithm>

namespace visivo {

/// Fixed metrics of one card, in device-independent pixels.
struct LayerCardMetrics
{
    int height{ 52 };
    int paddingX{ 10 };
    int paddingY{ 7 };
    int boxSide{ 14 };
    int stripHeight{ 9 };
    int gap{ 8 };
    /// The strip never shrinks below this, however narrow the dock gets: a
    /// gradient two pixels wide says nothing, and a negative width is a
    /// QPainter warning per repaint.
    int minimumStripWidth{ 24 };
};

/// The visibility box at the left of the row.
inline QRect layerCheckboxRect(const QRect &row, const LayerCardMetrics &m = {})
{
    return QRect(row.left() + m.paddingX, row.top() + m.paddingY + 1, m.boxSide, m.boxSide);
}

/// What a click has to land in to toggle visibility — the whole left gutter,
/// not just the 14 px box, which is a small target for a pointer.
inline QRect layerVisibilityHitRect(const QRect &row, const LayerCardMetrics &m = {})
{
    QRect hit = layerCheckboxRect(row, m);
    hit.adjust(-m.paddingX, -m.paddingY, m.gap / 2, m.paddingY);
    return hit;
}

/// The gradient strip, given the room left by the detail text to its right.
inline QRect layerStripRect(const QRect &card, int detailWidth, const LayerCardMetrics &m = {})
{
    const QRect box = layerCheckboxRect(card, m);
    const int left = box.right() + m.gap;
    const int right = card.right() - m.paddingX;
    const int width = std::max(m.minimumStripWidth, right - left - detailWidth);
    return QRect(left, box.bottom() + m.gap, width, m.stripHeight);
}

}   // namespace visivo

#endif   // LayerCardGeometry_h

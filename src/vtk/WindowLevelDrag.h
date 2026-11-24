#ifndef WindowLevelDrag_h
#define WindowLevelDrag_h

#include <algorithm>
#include <array>
#include <cmath>

// The arithmetic of a window/level drag on a colour range, with no VTK in it so
// the unit tests and every viewer that drags a range use the same numbers.
namespace WindowLevelDrag {

/// The colour range after a left-drag of (dx, dy) pixels in a view of
/// width x height pixels, starting from [lo0, hi0]. Horizontal changes the
/// window (width of the range, exponentially, so it stays positive and a drag
/// back undoes a drag forward); vertical moves the level (centre of the range)
/// by a fraction of the window. A start range that is empty, inverted or not
/// finite is taken as unit width, so a drag never produces a NaN range.
inline std::array<double, 2> rangeAfterDrag(double lo0, double hi0, double dx, double dy,
                                            double width, double height)
{
    double window = hi0 - lo0;
    if (!std::isfinite(window) || !(window > 0.))
        window = 1.;
    double mid = 0.5 * (hi0 + lo0);
    if (!std::isfinite(mid))
        mid = 0.;
    const double w = std::max(1., width);
    const double h = std::max(1., height);
    const double newWindow = window * std::exp(2.0 * dx / w);
    const double newLevel = mid - (dy / h) * window;
    return { newLevel - 0.5 * newWindow, newLevel + 0.5 * newWindow };
}

} // namespace WindowLevelDrag

#endif

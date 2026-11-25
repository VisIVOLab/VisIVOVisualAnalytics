#pragma once
// FontSize.h
// Scaling a font's size without ever asking Qt for a size it rejects.
//
// QFont::pointSizeF() is -1 when the font was given in pixels (a style sheet's
// font-size: 12px does that): -1 * 0.82 handed to setPointSizeF logged
// "Point size <= 0 (-0.82)" each time the session tree drew a pane badge.

#include <algorithm>

namespace visivo::fontsize {

/// @p size times @p factor, never below @p minimum. A size that is not
/// positive (a font set in the other unit) comes back as the minimum, never a
/// negative number.
inline double scaled(double size, double factor, double minimum)
{
    return size > 0.0 ? std::max(minimum, size * factor) : minimum;
}

}   // namespace visivo::fontsize

#ifndef SCALARCOLOURRANGE_H
#define SCALARCOLOURRANGE_H

// The colour range the ray-traced GPU view maps a particle's colour value
// through (ParticleGLView, LightScalar rows; issue #447). Widget-free, so the
// rule can be unit-tested; the vertex shader applies the same rule in float.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

namespace visivo {

struct ScalarColourRange
{
    double lo = 0.0;     ///< the low end, its log10 on a log scale
    double span = 0.0;   ///< the width, in the same units; 0: a flat range
    bool log = false;
};

/// Worked out in double, so a range of cgs densities (1e-35 to 1e-25) keeps
/// its ends: the shader once took the log of max(x, 1e-30) and divided by
/// max(hi - lo, 1e-30), which put such a table in its lowest colour. A log
/// scale needs a positive low end, as the stills' (particle_render.py,
/// _colours): a range reaching zero is drawn on a linear one.
inline ScalarColourRange scalarColourRange(double lo, double hi, bool log)
{
    ScalarColourRange r;
    r.log = log && lo > 0.0 && hi > 0.0;
    const double a = r.log ? std::log10(lo) : lo;
    const double b = r.log ? std::log10(hi) : hi;
    if (std::isfinite(a) && std::isfinite(b) && b > a) {
        r.lo = a;
        r.span = b - a;
    } else {
        r.lo = std::isfinite(a) ? a : 0.0;
    }
    return r;
}

/// The shader's NaN test (ParticleGLView, nanValue), by the float's bits: a
/// GPU compiler's fast maths may take isnan() to be always false. Any NaN,
/// quiet or signalling, of either sign; not an infinity.
inline bool nanBits(float x)
{
    std::uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    return (u & 0x7fffffffu) > 0x7f800000u;
}

/// Where ``value`` falls in the colour table, 0 to 1, as the shader places
/// it; no position (NaN) means the table's NaN colour. On a log scale a value
/// at or below zero takes the lowest colour.
inline std::optional<double> colourPosition(double value, const ScalarColourRange &r)
{
    if (std::isnan(value))
        return std::nullopt;
    double t = 0.0;
    if (r.span > 0.0) {
        if (r.log)
            t = value > 0.0 ? (std::log10(value) - r.lo) / r.span : 0.0;
        else
            t = (value - r.lo) / r.span;
    }
    return t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
}

} // namespace visivo

#endif // SCALARCOLOURRANGE_H

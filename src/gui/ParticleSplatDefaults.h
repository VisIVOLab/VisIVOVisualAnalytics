#ifndef ParticleSplatDefaults_h
#define ParticleSplatDefaults_h

#include <algorithm>
#include <cmath>

namespace ParticleSplatDefaults {

/// Gaussian size in units of the mean 3-D particle spacing. The UI maps a
/// slider value of 100 to size 3, so 25 is a 0.75-spacing initial footprint.
inline constexpr double kInitialSize = 0.75;

inline int initialSliderValue()
{
    return static_cast<int>(std::lround(kInitialSize * 100.0 / 3.0));
}

/// Per-splat opacity for an estimated number of particles along the longest
/// sightline. Their accumulated brightness stays around 1.5 instead of the
/// former 7, with a small floor for very deep clouds and a cap for sparse ones.
inline double opacityForDepth(double depth)
{
    if (!(std::isfinite(depth) && depth > 0.0))
        return 0.25;
    return std::clamp(1.5 / depth, 0.005, 0.25);
}

} // namespace ParticleSplatDefaults

#endif

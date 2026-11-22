#ifndef ParticleSplatDefaults_h
#define ParticleSplatDefaults_h

#include <algorithm>
#include <cmath>

namespace ParticleSplatDefaults {

/// Gaussian size in units of the mean 3-D particle spacing. The UI maps a
/// slider value of 100 to size 3, so 25 is a 0.75-spacing initial footprint.
inline constexpr double kInitialSize = 0.75;

/// The size slider's own default, which every other shape starts from.
inline constexpr int kDefaultSliderValue = 100;

inline int initialSliderValue()
{
    return static_cast<int>(std::lround(kInitialSize * 100.0 / 3.0));
}

/// The size slider after the shape changes, while it still holds a value the
/// viewer chose itself. The Gaussian default (initialSliderValue) is a quarter
/// of the slider's own, and carried over to Point it drew one-pixel points.
/// So leaving splats gives the slider its own default back, and coming back
/// to splats takes the Gaussian one again. The caller does not ask once the
/// user has moved the slider.
inline int automaticSliderValue(bool toSplats, int value)
{
    if (toSplats && value == kDefaultSliderValue)
        return initialSliderValue();
    if (!toSplats && value == initialSliderValue())
        return kDefaultSliderValue;
    return value;
}

/// The per-splat opacity counts in steps of an 8-bit channel (issue #447).
/// VTK keeps a splat's opacity as the 8-bit alpha of its colour and adds the
/// splats into an 8-bit framebuffer: an opacity of 1/255 was stored 22 % low,
/// one step more or less moved the brightness by a quarter, and the dark end
/// of a colour map or a kernel's wings added nothing at all. So the opacity is
/// a whole number of steps, at least 8. A cloud deeper than about 48 splats
/// along its longest sightline then comes out brighter than the nominal 1.5,
/// rather than losing its faint colours (about 2.1× the nominal brightness at
/// a million rows, 3.2× at 3.66 M, 4.5× at 10 M). The backend's point session
/// and the VR page's 8-bit targets use the same steps (backend/app/
/// point_splat.py, splat_opacity); the page's float targets, which have no
/// 8-bit store, keep 1.5 / depth unquantised.
inline constexpr int kMinAlphaSteps = 8;
inline constexpr int kMaxAlphaSteps = 64;   ///< about 0.25

/// ``intensity``: the point session's and the page's brightness control, 35
/// its standard (the desktop has none).
inline int alphaStepsForDepth(double depth, double intensity = 35.0)
{
    if (!(std::isfinite(depth) && depth > 0.0))
        return kMaxAlphaSteps;
    // Half up, in the same operations as the backend's and the page's copies,
    // so that the three agree to the bit on a step's boundary.
    const double steps = std::floor(1.5 * intensity / (35.0 * depth) * 255.0 + 0.5);
    return static_cast<int>(std::clamp(steps, double(kMinAlphaSteps), double(kMaxAlphaSteps)));
}

/// Per-splat opacity for an estimated number of particles along the longest
/// sightline: their accumulated brightness stays around 1.5 (the former
/// viewer's 7 saturated), in whole 8-bit steps from kMinAlphaSteps to
/// kMaxAlphaSteps.
inline double opacityForDepth(double depth)
{
    return alphaStepsForDepth(depth) / 255.0;
}

/// What to give VTK's opacity function for ``opacity`` so that the 8-bit
/// alpha it stores is the intended step: VTK truncates, and k / 255 × 255 can
/// come out a hair below k; a quarter step above lands on k whether VTK
/// truncates or rounds.
inline double vtkOpacityValue(double opacity)
{
    return (std::floor(opacity * 255.0 + 0.5) + 0.25) / 255.0;
}

/// The 8-bit alpha a truncating store keeps of an opacity-function value.
inline int storedAlphaSteps(double value)
{
    return static_cast<int>(value * 255.0);
}

} // namespace ParticleSplatDefaults

#endif

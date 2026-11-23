#ifndef ParticleSplatDefaults_h
#define ParticleSplatDefaults_h

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

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

/// The size slider's range, as the window's form sets it
/// (ui/vtkWindowCatalogue3D.ui).
inline constexpr int kSliderMinimum = 1;
inline constexpr int kSliderMaximum = 500;

/// A size from the slider's @p value, linear between @p knots (slider value,
/// size), in increasing order of both, and held at the end knots outside them.
template <std::size_t N>
double sizeThroughKnots(int value, const std::array<std::array<double, 2>, N> &knots)
{
    const double v = static_cast<double>(value);
    if (v <= knots.front()[0])
        return knots.front()[1];
    for (std::size_t i = 1; i < N; ++i) {
        if (v <= knots[i][0]) {
            const double t = (v - knots[i - 1][0]) / (knots[i][0] - knots[i - 1][0]);
            return knots[i - 1][1] + t * (knots[i][1] - knots[i - 1][1]);
        }
    }
    return knots.back()[1];
}

/// The splats' size for the size slider's @p value, in the former VBT
/// viewer's size step. The whole slider changes it (issue #456): it ran as
/// 3 × value / 100 clamped to 0.25..12, so the right fifth of the slider and
/// its first steps did nothing, and a drag there looked broken. The ends stay
/// 0.25 and 12, the range the backend's point session accepts
/// (render_session/points.py, set_point_style), and the splat default (25,
/// size 0.75) and the slider's own default (100, size 3) keep their sizes, as
/// does everything between them, so views opened at a default do not change;
/// a view saved at another value draws its splats a little differently.
inline double splatSizeForSlider(int value)
{
    static constexpr std::array<std::array<double, 2>, 4> knots{ {
            { double(kSliderMinimum), 0.25 },
            { kInitialSize * 100.0 / 3.0, kInitialSize },
            { double(kDefaultSliderValue), 3.0 },
            { double(kSliderMaximum), 12.0 } } };
    return sizeThroughKnots(value, knots);
}

/// A local splat's radius for the mean particle @p spacing and the size
/// slider's @p value: spacing × size / 8, as the backend's point session
/// (app/render_session/points.py, gaussian_splat) sizes its own.
inline double splatRadiusForSlider(double spacing, int value)
{
    return spacing * splatSizeForSlider(value) / 8.0;
}

/// The points' size in pixels for the size slider's @p value. It was
/// 4 × value / 100 rounded and clamped to 3..12 pixels, flat below 75 and
/// above 300 (issue #456); that middle stretch is kept as it was, and the
/// ends now reach 1 and 16 pixels. Not rounded: VTK takes a fractional point
/// size, and the backend's physical pixels are rounded after the pixel ratio.
inline double pointSizeForSlider(int value)
{
    static constexpr std::array<std::array<double, 2>, 4> knots{ {
            { double(kSliderMinimum), 1.0 },
            { 75.0, 3.0 },
            { 300.0, 12.0 },
            { double(kSliderMaximum), 16.0 } } };
    return sizeThroughKnots(value, knots);
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

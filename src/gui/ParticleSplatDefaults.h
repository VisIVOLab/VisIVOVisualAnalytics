#ifndef ParticleSplatDefaults_h
#define ParticleSplatDefaults_h

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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

/// The VR page's 8-bit splat rule (``v2-8bit``, issue #447): an 8-bit WebGL
/// target rounds every addition, so there a splat's opacity is a whole number
/// of steps of its channel, at least 8, and a cloud deeper than about 48
/// splats along its longest sightline comes out brighter than the nominal 1.5
/// (about 2.1x at a million rows, 3.2x at 3.66 M, 4.5x at 10 M). The desktop
/// adds its splats into a float target and does not use it; it is kept here,
/// pinned by the tests to the same steps as backend/app/point_splat.py
/// (splat_opacity) and the page (points.js), as the reference for that rule.
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

/// The desktop's intensity: the point session's standard.
inline constexpr double kIntensity = 35.0;
/// The peak opacity of a splat in a float target never exceeds this
/// (point_splat.FLOAT_OPACITY_MAX): it dims only clouds a few splats deep.
inline constexpr double kMaxOpacity = 0.25;

/// Peak opacity of a splat for an estimated number of particles along the
/// longest sightline (issue #457): 1.5 / depth at the standard intensity,
/// unquantised and with no floor, so N splats add up to the same light for
/// any N. The exposure is the intensity alone, never taken from the frame, so
/// two comparison panes stay comparable and real density differences show.
/// The same rule as the backend point session's float target (issue #457:
/// point_splat.splat_opacity(depth, intensity, steps=False), which until
/// then kept a floor of 0.005).
inline double opacityForDepth(double depth, double intensity = kIntensity)
{
    if (!(std::isfinite(depth) && depth > 0.0))
        return kMaxOpacity;
    return std::min(1.5 * intensity / (35.0 * depth), kMaxOpacity);
}

/// The fragment code (vtkPointGaussianMapper::SetSplatShaderCode) of a splat
/// of peak opacity ``gain``, for a mapper whose opacity function is 1 and
/// whose renderer adds into a float target: VTK's own code reads the opacity
/// from the colour's 8-bit alpha, which cannot hold 1.5 / depth. Character
/// for character point_splat.vtk_splat_shader (issue #457), so the desktop
/// and the point session draw the same splat; their levels of detail differ
/// (the point session raises the gain by the stride, the desktop grows the
/// radius), for the same light. A gain that is not a finite number at least 0
/// gives an empty string.
inline std::string splatShaderCode(double gain)
{
    if (!(std::isfinite(gain) && gain >= 0.0))
        return {};
    char literal[32];
    std::snprintf(literal, sizeof literal, "%.9e", gain);
    return std::string("//VTK::Color::Impl\n"
                       "  float dist2 = dot(offsetVCVSOutput.xy, offsetVCVSOutput.xy);\n"
                       "  opacity = opacity * exp(-0.5 * dist2) * ")
            + literal + ";\n";
}

/// The stride of the render budget's level of detail: every stride-th row of
/// ``rows`` is drawn, so that at most ``budget`` are (0: every row).
inline long long renderStride(long long rows, long long budget)
{
    if (budget <= 0 || rows <= budget)
        return 1;
    return (rows + budget - 1) / budget;
}

/// How a cloud of splats is drawn: radius in the points' units, the
/// sightline depth in splats, and the peak opacity.
struct SplatStyle {
    double radius = 0.0;
    double depth = 1.0;
    double gain = kMaxOpacity;
};

/// Splats for ``rows`` points filling ``bounds`` (xmin, xmax, ymin, ymax,
/// zmin, zmax) at size step ``size``, as the former VBT viewer and the point
/// session (app/render_session/points.py gaussian_splat) size them: spacing
/// ∛(volume / rows) of the box, a flat axis counted as a thousandth of the
/// longest; radius = spacing × size / 8; depth = longest side / spacing (∛N
/// for a cube, unchanged by a filter that cuts a slab). ``rows`` may be
/// fractional: a level of detail's every stride-th row of a table is
/// table / stride rows over the same box, and its larger, brighter splats
/// carry the light of the whole table.
inline SplatStyle splatStyle(const double bounds[6], double rows, double size,
                             double intensity = kIntensity)
{
    const double dims[3] = { bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4] };
    const double longest = std::max({ dims[0], dims[1], dims[2], 1.0e-6 });
    double volume = 1.0;
    for (const double d : dims)
        volume *= std::max(d, longest * 1.0e-3);
    const double spacing = std::cbrt(volume / std::max(1.0, rows));
    SplatStyle style;
    style.radius = spacing * size / 8.0;
    style.depth = std::max(longest / spacing, 1.0);
    style.gain = opacityForDepth(style.depth, intensity);
    return style;
}

} // namespace ParticleSplatDefaults

#endif

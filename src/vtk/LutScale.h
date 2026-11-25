#ifndef LutScale_h
#define LutScale_h

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

class vtkLookupTable;
class vtkColorTransferFunction;
class vtkImageData;

// ---------------------------------------------------------------------------
// LutScale — shared colour-scale stretches for every VisIVO viewer.
//
// VTK's vtkLookupTable only natively supports LINEAR and LOG10 scale modes.
// To offer the DS9/CARTA-style Square-Root / Squared / Power stretches we keep
// the table in LINEAR mode and resample its colour entries through the stretch
// curve  f(t) = t^exp  (t the normalised data position in [0,1]).  A value at
// normalised position t then receives the colour the colormap holds at f(t),
// which is exactly what a non-linear stretch means.
//
// The same helper handles a vtkColorTransferFunction (volume rendering) by
// redistributing its node positions through the same curve.
// ---------------------------------------------------------------------------
namespace LutScale {

enum class Mode {
    Linear = 0,
    Log = 1,
    Sqrt = 2,
    Square = 3,
    Power = 4,
};

// Combo/segmented-control labels; index matches the Mode enum value.
std::array<const char *, 5> modeLabels();

// Short tool-tip for each mode (index matches the Mode enum value).
std::array<const char *, 5> modeToolTips();

// The pure arithmetic of the stretches is inline, with no VTK in it, so the
// viewers that do not draw through a vtkLookupTable (the PV diagram) and the
// unit tests use the same numbers as the tables built below.

// The exponent applied to the normalised position. Sqrt -> 0.5, Square -> 2.0,
// Power -> the caller-supplied gamma, Linear/Log -> 1.0 (unused).
inline double exponent(Mode mode, double gamma)
{
    switch (mode) {
    case Mode::Sqrt:
        return 0.5;
    case Mode::Square:
        return 2.0;
    case Mode::Power:
        // Clamp to a sane, strictly-positive range so the curve stays monotone.
        return std::clamp(gamma, 0.1, 10.0);
    case Mode::Linear:
    case Mode::Log:
    default:
        return 1.0;
    }
}

// True for the stretches implemented by colour-entry resampling (everything
// except Linear and the native Log).
inline bool isResampled(Mode mode)
{
    return mode == Mode::Sqrt || mode == Mode::Square || mode == Mode::Power;
}

// Smallest strictly-positive value usable as a log floor for `maxValue` when
// nothing better is known: six decades below it.
inline double positiveFloor(double maxValue)
{
    if (!(maxValue > 0.0) || !std::isfinite(maxValue))
        return 1e-6;
    return maxValue * 1e-6;
}

// The log floor sampledPositiveFloor() picks from the positive samples it
// collected: their 1st percentile, low enough to keep the faint end, high
// enough that a single hot-adjacent pixel or a residual near zero does not set
// the floor. 0.0 for an empty sample.
inline double percentileFloor(std::vector<double> positives)
{
    if (positives.empty())
        return 0.0;
    std::sort(positives.begin(), positives.end());
    const std::size_t idx = static_cast<std::size_t>(
            std::clamp<double>(0.01 * static_cast<double>(positives.size() - 1), 0.0,
                               static_cast<double>(positives.size() - 1)));
    // No absolute floor: physical densities are 1e-24 g/cm3 and below, and a
    // fixed 1e-12 here put the whole map under it (a black slice whose colour
    // bar read "1e-12" throughout). Every sample is already > 0.
    return positives[idx];
}

// Apply a scale mode to a vtkLookupTable ALREADY freshly filled with a linear
// colormap.  NOTE: this resamples the current entries, so it is NOT idempotent —
// callers that may re-apply on an already-stretched table must use build()
// instead.
//   * Linear -> native SetScaleToLinear
//   * Log    -> native SetScaleToLog10 (non-positive endpoints floored)
//   * Sqrt/Square/Power -> linear scale + colour-entry resample through f.
// Sets the table range and rebuilds. `gamma` is only used by Power.
void apply(vtkLookupTable *lut, Mode mode, double min, double max, double gamma = 2.0);

// Idempotent entry point: forces a fresh linear fill of `colorMapName` (defeating
// ColorMaps' same-name fast-path) and then applies the stretch. Safe to call on
// an already-stretched table — this is what every viewer should use.
void build(vtkLookupTable *lut, const std::string &colorMapName, Mode mode, double min,
           double max, double gamma = 2.0);

// Set ONLY the table range, honouring the current mode's constraints, without
// touching the colour entries.
//
// For everything but Log this is a plain SetTableRange. For Log it applies the
// same positive floor apply()/build() use — which is the whole point: VTK
// refuses a log range whose endpoints are not positive, and silently
// substitutes [1, 10]. Radio data is negative wherever there is only noise, so
// a viewer that pushed a fresh scalar range straight at a log-scaled table got
// a colour mapping with no relation to the data and one line on stderr:
//   vtkLookupTable 'Inferno': Bad table range for log scale: [-0.0078, 0.0387],
//   adjusting to [1, 10]
// Use this wherever new data arrives and only the range changes; use build()
// when the colormap or the mode itself changes.
void setRange(vtkLookupTable *lut, Mode mode, double min, double max);

// The range setRange()/apply()/build() would actually install for `mode` — the
// input unchanged except under Log, where non-positive endpoints are floored.
// Callers that compare against the current table range, or that echo the range
// onward (linked panes, colorbars), need the installed values rather than the
// requested ones: on a log stretch of signed data those differ, and treating
// them as equal makes a "did it change?" guard never fire.
inline std::array<double, 2> rangeFor(Mode mode, double min, double max)
{
    if (mode != Mode::Log)
        return { min, max };
    const double flooredMax = (max > 0.0 && std::isfinite(max)) ? max : positiveFloor(max);
    const double flooredMin = (min > 0.0 && std::isfinite(min)) ? min : positiveFloor(flooredMax);
    return { flooredMin, flooredMax };
}

// The data value a colour bar shows at `position` (0 = bottom, 1 = top) when
// its colours run evenly: log-spaced under Log (over the range rangeFor()
// installs), and under Sqrt/Square/Power the inverse of the stretch, since a
// value at normalised t takes the colour the map holds at t^exponent.
inline double valueAt(Mode mode, double position, double min, double max, double gamma = 2.0)
{
    const double u = std::clamp(position, 0.0, 1.0);
    if (mode == Mode::Log) {
        const auto r = rangeFor(Mode::Log, min, max);
        return r[1] > r[0] ? r[0] * std::pow(r[1] / r[0], u) : min;
    }
    return min + (max - min) * std::pow(u, 1.0 / exponent(mode, gamma));
}

// A log-stretch floor taken FROM THE DATA: the 1st percentile of the positive
// finite samples, or 0.0 when the image has no positive values at all.
//
// The percentile is a display heuristic, not a noise threshold — for zero-mean
// Gaussian noise it lands near 0.013 sigma, so some colour still goes to
// sub-noise values. It is the rule the image viewer already uses, kept here so
// the two agree.
//
// Worth the sampling pass, because the fallback the functions above use when a
// caller supplies nothing better — max x 1e-6, six fixed decades — is a shape,
// not a measurement. On a radio map whose real dynamic range is two or three
// decades it spends most of the colour ramp on values that are not there: with
// a six-decade floor, everything in the brightest decade is squeezed into the
// top sixth of the colours. Anchoring the bottom near the noise instead gives
// the ramp back to the range the data occupies.
//
// A return of 0.0 means "log is not meaningful for this image" and the caller
// should say so rather than stretch anyway; it is not a usable floor. That
// verdict is taken from the image's own scalar maximum, not from the sample
// coming back empty — a strided sample can step over a compact source.
double sampledPositiveFloor(vtkImageData *image, int targetSamples = 20000);

// Idempotent variant for procedurally-generated LUTs (HSV ramps via
// SetHueRange/SetSaturationRange/…) that are NOT filled from a named colormap:
// regenerates the base table with ForceBuild() before applying the stretch.
void applyGenerated(vtkLookupTable *lut, Mode mode, double min, double max, double gamma = 2.0);

// Redistribute a vtkColorTransferFunction's nodes through the stretch curve so
// volume rendering matches the slice LUT. The function snapshots its own current
// colours across [min,max], so it stretches in place.
void applyToColorTransferFunction(vtkColorTransferFunction *ctf, Mode mode,
                                  double min, double max, double gamma = 2.0,
                                  int sampleCount = 256);

}  // namespace LutScale

#endif

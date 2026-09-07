#ifndef LutScale_h
#define LutScale_h

#include <array>
#include <string>

class vtkLookupTable;
class vtkColorTransferFunction;

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

// The exponent applied to the normalised position. Sqrt -> 0.5, Square -> 2.0,
// Power -> the caller-supplied gamma, Linear/Log -> 1.0 (unused).
double exponent(Mode mode, double gamma);

// True for the stretches implemented by colour-entry resampling (everything
// except Linear and the native Log).
bool isResampled(Mode mode);

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
std::array<double, 2> rangeFor(Mode mode, double min, double max);

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

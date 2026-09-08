#include "LutScale.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <vtkColorTransferFunction.h>
#include <vtkImageData.h>
#include <vtkLookupTable.h>

#include "ColorMaps.h"

namespace LutScale {

std::array<const char *, 5> modeLabels()
{
    return { "Linear", "Log", "Sqrt", "Square", "Power" };
}

std::array<const char *, 5> modeToolTips()
{
    return {
        "Linear mapping of scalar value → colour.",
        "Logarithmic mapping (range minimum must be strictly positive).",
        "Square-root stretch (√t) — emphasises faint structure.",
        "Squared stretch (t²) — emphasises bright structure.",
        "Power stretch t^γ with adjustable exponent γ.",
    };
}

double exponent(Mode mode, double gamma)
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

bool isResampled(Mode mode)
{
    return mode == Mode::Sqrt || mode == Mode::Square || mode == Mode::Power;
}

namespace {

// Smallest strictly-positive value usable as a log/zero floor for `maxValue`.
double positiveFloor(double maxValue)
{
    if (!(maxValue > 0.0) || !std::isfinite(maxValue)) {
        return 1e-6;
    }
    return maxValue * 1e-6;
}

// Re-index a freshly-built colour table in place so entry i shows the colour the
// base table holds at f(i/(n-1)) = (i/(n-1))^exp.
void resampleEntries(vtkLookupTable *lut, double exp)
{
    const vtkIdType n = lut->GetNumberOfTableValues();
    if (n <= 1) {
        return;
    }
    std::vector<std::array<double, 4>> source(static_cast<std::size_t>(n));
    for (vtkIdType i = 0; i < n; ++i) {
        lut->GetTableValue(i, source[static_cast<std::size_t>(i)].data());
    }
    for (vtkIdType i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const double ft = std::pow(t, exp);
        vtkIdType j = static_cast<vtkIdType>(std::lround(ft * static_cast<double>(n - 1)));
        j = std::clamp<vtkIdType>(j, 0, n - 1);
        lut->SetTableValue(i, source[static_cast<std::size_t>(j)].data());
    }
}

}  // namespace

void apply(vtkLookupTable *lut, Mode mode, double min, double max, double gamma)
{
    if (!lut) {
        return;
    }

    if (mode == Mode::Log) {
        // Only floor non-positive / non-finite endpoints; a caller that already
        // supplied a dataset-sampled positive floor must not be overridden.
        const double flooredMax = (max > 0.0 && std::isfinite(max)) ? max : positiveFloor(max);
        const double flooredMin =
                (min > 0.0 && std::isfinite(min)) ? min : positiveFloor(flooredMax);
        lut->SetTableRange(flooredMin, flooredMax);   // before the scale — see setRange()
        lut->SetScaleToLog10();
        lut->Build();
        return;
    }

    // Linear / Sqrt / Square / Power all use a native LINEAR value->index map;
    // the non-linearity (when any) is baked into the colour entries below.
    lut->SetScaleToLinear();
    lut->SetTableRange(min, max);
    lut->Build();

    if (isResampled(mode)) {
        resampleEntries(lut, exponent(mode, gamma));
    }
}

double sampledPositiveFloor(vtkImageData *image, int targetSamples)
{
    if (!image) {
        return 0.0;
    }
    int extent[6] = { 0, -1, 0, -1, 0, -1 };
    image->GetExtent(extent);
    const long long nx = static_cast<long long>(extent[1]) - extent[0] + 1;
    const long long ny = static_cast<long long>(extent[3]) - extent[2] + 1;
    const long long nz = static_cast<long long>(extent[5]) - extent[4] + 1;
    if (nx <= 0 || ny <= 0 || nz <= 0) {
        return 0.0;
    }
    const long long voxels = nx * ny * nz;
    const int target = std::max(1, targetSamples);

    // One stride for every axis, but the root taken over the number of axes that
    // actually have extent. A cube root on a 2-D image under-strides: on a
    // 2048x2048 map it asks for 20000 samples and takes 116000, measurably the
    // slowest case in the set. The square root there gives what was asked for.
    const int dims = (nx > 1 ? 1 : 0) + (ny > 1 ? 1 : 0) + (nz > 1 ? 1 : 0);
    const double ratio = std::max(1.0, static_cast<double>(voxels) / static_cast<double>(target));
    const int stride = (dims <= 0)
            ? 1
            : std::max(1, static_cast<int>(std::ceil(std::pow(ratio, 1.0 / dims))));

    std::vector<double> positives;
    positives.reserve(static_cast<std::size_t>(target));
    for (int z = extent[4]; z <= extent[5]; z += stride) {
        for (int y = extent[2]; y <= extent[3]; y += stride) {
            for (int x = extent[0]; x <= extent[1]; x += stride) {
                const double v = image->GetScalarComponentAsDouble(x, y, z, 0);
                if (std::isfinite(v) && v > 0.0) {
                    positives.push_back(v);
                }
            }
        }
    }
    if (positives.empty()) {
        // An empty sample is not proof that the image has none: the lattice
        // steps over pixels, so a compact positive source in a mostly-blank or
        // masked product can fall entirely between sampled coordinates. Ask the
        // image for its actual maximum — VTK caches it, so this is not a second
        // pass — and only then say there is nothing to stretch.
        double range[2] = { 0., 0. };
        image->GetScalarRange(range);
        if (!(range[1] > 0.0) || !std::isfinite(range[1])) {
            return 0.0;   // genuinely nothing positive
        }
        // Positives exist but the sample missed them. Six decades below the peak
        // is a poor floor, which is the whole reason this function exists — but
        // it is a working one, and better than refusing a stretch of an image
        // that does have signal in it.
        return std::max(1e-12, range[1] * 1e-6);
    }
    std::sort(positives.begin(), positives.end());
    // 1st percentile: low enough to keep the faint end, high enough that a
    // single hot-adjacent pixel or a residual near zero does not set the floor.
    const std::size_t idx = static_cast<std::size_t>(
            std::clamp<double>(0.01 * static_cast<double>(positives.size() - 1), 0.0,
                               static_cast<double>(positives.size() - 1)));
    return std::max(1e-12, positives[idx]);
}

std::array<double, 2> rangeFor(Mode mode, double min, double max)
{
    if (mode != Mode::Log) {
        return { min, max };
    }
    const double flooredMax = (max > 0.0 && std::isfinite(max)) ? max : positiveFloor(max);
    const double flooredMin = (min > 0.0 && std::isfinite(min)) ? min : positiveFloor(flooredMax);
    return { flooredMin, flooredMax };
}

void setRange(vtkLookupTable *lut, Mode mode, double min, double max)
{
    if (!lut) {
        return;
    }
    // The native scale is set as well as the range. SetTableRange validates
    // against whatever scale the table is in RIGHT NOW, so a caller switching
    // to Linear and then pushing a range would still be rejected while the
    // table remained in log from the previous mode. Setting both together
    // means the two cannot be out of step, whichever order the caller works in.
    if (mode == Mode::Log) {
        const std::array<double, 2> r = rangeFor(mode, min, max);
        const double flooredMin = r[0], flooredMax = r[1];
        // Range BEFORE scale on the way into log. SetScaleToLog10() validates the
        // range the table is already holding, so switching a linear table that
        // holds a signed range produces the same complaint the floor exists to
        // prevent — the mapping ends up right, but VTK has already adjusted and
        // logged. Installing the floored range first leaves nothing to object to.
        lut->SetTableRange(flooredMin, flooredMax);
        lut->SetScaleToLog10();
        return;
    }
    // Scale BEFORE range on the way out: a signed range cannot be installed
    // while the table is still logarithmic. Sqrt/Square/Power keep a linear
    // value->index map, the curve living in the colour entries, which this
    // function deliberately does not touch.
    lut->SetScaleToLinear();
    lut->SetTableRange(min, max);
}

void applyGenerated(vtkLookupTable *lut, Mode mode, double min, double max, double gamma)
{
    if (!lut) {
        return;
    }
    // For procedurally-generated LUTs (HSV ramps etc.) ForceBuild() regenerates
    // the base colours unconditionally — so this is idempotent even after a prior
    // resample (unlike apply(), which assumes a caller-supplied fresh fill).
    if (mode == Mode::Log) {
        const double flooredMax = (max > 0.0 && std::isfinite(max)) ? max : positiveFloor(max);
        const double flooredMin =
                (min > 0.0 && std::isfinite(min)) ? min : positiveFloor(flooredMax);
        lut->SetTableRange(flooredMin, flooredMax);   // before the scale — see setRange()
        lut->SetScaleToLog10();
        lut->ForceBuild();
        return;
    }
    lut->SetScaleToLinear();
    lut->SetTableRange(min, max);
    lut->ForceBuild();
    if (isResampled(mode)) {
        resampleEntries(lut, exponent(mode, gamma));
    }
}

void build(vtkLookupTable *lut, const std::string &colorMapName, Mode mode, double min,
           double max, double gamma)
{
    if (!lut) {
        return;
    }
    // Force a fresh LINEAR colormap fill before (re)applying the stretch.
    // ColorMaps::SetColorMap early-returns when the name is unchanged, which
    // would leave an already-stretched table in place and compound the stretch
    // on the next apply — clearing the object name defeats that fast-path.
    lut->SetObjectName("");
    ColorMaps::SetColorMap(lut, colorMapName);
    apply(lut, mode, min, max, gamma);
}

void applyToColorTransferFunction(vtkColorTransferFunction *ctf, Mode mode,
                                  double min, double max, double gamma, int sampleCount)
{
    if (!ctf || !(max > min) || sampleCount < 2) {
        return;
    }

    const double span = max - min;
    const double exp = exponent(mode, gamma);
    const bool log = (mode == Mode::Log);

    // Snapshot the current colours as a function of value across [min,max].
    std::vector<std::array<double, 3>> base(static_cast<std::size_t>(sampleCount));
    for (int s = 0; s < sampleCount; ++s) {
        const double t = static_cast<double>(s) / static_cast<double>(sampleCount - 1);
        double rgb[3];
        ctf->GetColor(min + t * span, rgb);
        base[static_cast<std::size_t>(s)] = { rgb[0], rgb[1], rgb[2] };
    }
    const auto colourAtNorm = [&](double p) -> const std::array<double, 3> & {
        const int idx = std::clamp(static_cast<int>(std::lround(p * (sampleCount - 1))),
                                   0, sampleCount - 1);
        return base[static_cast<std::size_t>(idx)];
    };

    const double floorVal = positiveFloor(max);
    const double logMin = log ? std::log10(std::max(min, floorVal)) : 0.0;
    const double logMax = log ? std::log10(std::max(max, floorVal)) : 0.0;

    // Rebuild nodes: the node at value (min + t·span) shows the base colour at
    // the stretched source position so the displayed colour matches the slice
    // LUT's stretch.
    ctf->RemoveAllPoints();
    for (int s = 0; s < sampleCount; ++s) {
        const double t = static_cast<double>(s) / static_cast<double>(sampleCount - 1);
        const double value = min + t * span;
        double srcNorm;
        if (log) {
            // The node at linear `value` shows the base colour at that value's
            // LOG-normalised position.
            srcNorm = (logMax > logMin)
                    ? std::clamp((std::log10(std::max(value, floorVal)) - logMin)
                                         / (logMax - logMin),
                                 0.0, 1.0)
                    : t;
        } else {
            srcNorm = std::pow(t, exp);
        }
        const auto &c = colourAtNorm(srcNorm);
        ctf->AddRGBPoint(value, c[0], c[1], c[2]);
    }
}

}  // namespace LutScale

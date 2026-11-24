#pragma once
// FieldOverlay.h
// The arithmetic of showing several fields of one simulation in one view
// (issue #525): how many fields at most, when a field is drawn on a log
// scale, where a 3-D field's opacity ramp starts, how the fields' colours
// combine on the 2-D slice, and how a split view takes its halves from two
// fields. Standard C++ only (no Qt, no VTK), so it is unit-tested; the window
// feeds it the colours its lookup tables produce.
//
// 2-D layouts, on the slice's own grid (the fields of one run share it):
//   Layered   each visible field drawn over the one below it, weighted by its
//             opacity: the usual alpha "over".
//   Additive  the colours summed per pixel and clipped at 1 (LayerBlend.h:
//             the image viewer's Additive blend, so one rule for both).
//   Split     left of a seam field A, right of it field B.
//   Mirrored  for an axisymmetric run whose grid holds one side of the axis:
//             field A reflected to the left of the axis, field B as it is to
//             the right.
//
// 3-D: the fields are separate volumes of ONE ray cast (vtkMultiVolume), each
// with its own colour and opacity functions; the cast SUMS their emission and
// absorption along the ray, as the headset's Overlay does in Sum mode.

#include "LayerBlend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace visivo::fields {

/// The fields in one view, the first included: the headset's Overlay takes
/// three variables at most (OVERLAY_MAX_FIELDS), and so does the desktop.
inline constexpr int kMaxFields = 3;

/// A positive field spanning three decades or more opens on a log scale: on a
/// linear one only its brightest voxels show. A radio cube, with noise around
/// zero, is not positive and stays linear.
inline bool prefersLog(double minValue, double maxValue)
{
    return minValue > 0.0 && std::isfinite(maxValue) && maxValue / minValue >= 1e3;
}

/// The value at quantile \a q (0..1) of \a sample, which is reordered; NaN and
/// infinite values are ignored. NaN for an empty sample.
inline double quantile(std::vector<float> &sample, double q)
{
    sample.erase(std::remove_if(sample.begin(), sample.end(),
                                [](float v) { return !std::isfinite(v); }),
                 sample.end());
    if (sample.empty())
        return std::nan("");
    const std::size_t k = static_cast<std::size_t>(
            std::clamp(q, 0.0, 1.0) * static_cast<double>(sample.size() - 1) + 0.5);
    std::nth_element(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(k),
                     sample.end());
    return sample[k];
}

/// Where a 3-D field's opacity starts when nothing was typed: the quantile of
/// its voxels (the headset's default threshold is the 99th percentile; a
/// tracer filling a few per cent of the box wants a little lower).
inline constexpr double kDefaultThresholdQuantile = 0.95;

// ── 2-D layouts ─────────────────────────────────────────────────────────────

enum class Layout { Layered = 0, Additive = 1, Split = 2, Mirrored = 3 };

inline const char *layoutLabel(Layout l)
{
    switch (l) {
    case Layout::Layered:  return "Layered";
    case Layout::Additive: return "Additive";
    case Layout::Split:    return "Split";
    case Layout::Mirrored: return "Mirrored";
    }
    return "Layered";
}

/// Split and Mirrored take two fields (the first two shown); the others any.
inline bool usesHalves(Layout l) { return l == Layout::Split || l == Layout::Mirrored; }

/// The running "over" composite of one pixel: colour premultiplied by alpha.
struct OverAccum
{
    float r{ 0 }, g{ 0 }, b{ 0 }, a{ 0 };
};

/// Draws a layer's colour (8-bit RGBA from its table) over \a acc, weighted by
/// its \a opacity and by the colour's own alpha (a blank pixel is alpha 0).
inline void accumulateOver(OverAccum &acc, const std::uint8_t rgba[4], double opacity)
{
    const float a = static_cast<float>(std::clamp(opacity, 0.0, 1.0) * rgba[3] / 255.0);
    const float k = a / 255.f;
    acc.r = rgba[0] * k + acc.r * (1.f - a);
    acc.g = rgba[1] * k + acc.g * (1.f - a);
    acc.b = rgba[2] * k + acc.b * (1.f - a);
    acc.a = a + acc.a * (1.f - a);
}

/// The pixel \a acc stands for, straight (not premultiplied) RGBA.
inline blend::Rgba8 finishOver(const OverAccum &acc)
{
    if (!(acc.a > 0.f))
        return {};
    const auto to8 = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
    };
    return { to8(acc.r / acc.a), to8(acc.g / acc.a), to8(acc.b / acc.a), to8(acc.a) };
}

/// One field's colours on the slice: \a rgba holds width x height pixels, 4
/// bytes each (the field's lookup table already applied; alpha 0 = no data).
struct ColourLayer
{
    const std::uint8_t *rgba{ nullptr };
    double opacity{ 1.0 };
};

/// Layered or Additive composite of \a layers (first = bottom) over \a pixels
/// pixels into \a out. A layer of null colours is skipped.
inline void compose(Layout layout, const std::vector<ColourLayer> &layers, std::size_t pixels,
                    blend::Rgba8 *out)
{
    for (std::size_t p = 0; p < pixels; ++p) {
        if (layout == Layout::Additive) {
            blend::Accum acc;
            for (const ColourLayer &l : layers) {
                // No data (alpha 0) must not make the pixel opaque: a field
                // that is blank here adds nothing and claims nothing.
                if (l.rgba && l.rgba[p * 4 + 3] != 0)
                    blend::accumulate(acc, &l.rgba[p * 4], l.opacity);
            }
            out[p] = blend::finish(acc);
        } else {
            OverAccum acc;
            for (const ColourLayer &l : layers) {
                if (l.rgba)
                    accumulateOver(acc, &l.rgba[p * 4], l.opacity);
            }
            out[p] = finishOver(acc);
        }
    }
}

// ── Split and mirrored views ────────────────────────────────────────────────

/// How a split view lays its two halves out on the slice's grid. Output column
/// k stands for slice column k + \a indexOffset (negative columns lie on the
/// far side of the axis when mirrored), so the right half keeps its place and
/// everything drawn in plane pixels still lands.
struct SplitLayout
{
    int outWidth{ 0 };       ///< columns of the composite
    int indexOffset{ 0 };    ///< slice column of output column 0
    int seam{ 0 };           ///< first output column of the right half
    bool mirrored{ false };
    int sourceWidth{ 0 };    ///< columns of one field's slice
    int axisColumn{ 0 };     ///< mirrored: the slice column on the axis
};

/// The layout for a \a width-column slice. Split: field A fills the columns
/// before \a seamColumn, field B the others (the seam clamped to the slice).
/// Mirrored: the axis is at \a seamColumn; the columns from it on show field B
/// as they are and field A reflected, so the composite is twice as wide as the
/// part of the slice on the axis' right.
inline SplitLayout splitLayout(int width, int seamColumn, bool mirrored)
{
    SplitLayout s;
    s.sourceWidth = std::max(0, width);
    s.mirrored = mirrored;
    const int seam = std::clamp(seamColumn, 0, s.sourceWidth);
    s.axisColumn = seam;
    if (!mirrored) {
        s.outWidth = s.sourceWidth;
        s.indexOffset = 0;
        s.seam = seam;
    } else {
        const int half = s.sourceWidth - seam;
        s.outWidth = 2 * half;
        s.indexOffset = seam - half;
        s.seam = half;
    }
    return s;
}

/// Which field an output column takes (0: A, 1: B) and the slice column it
/// reads; -1 when the column reads nothing.
struct SplitSource
{
    int field{ 0 };
    int column{ -1 };
};

inline SplitSource splitSource(const SplitLayout &s, int outColumn)
{
    if (outColumn < 0 || outColumn >= s.outWidth)
        return {};
    if (!s.mirrored)
        return { outColumn < s.seam ? 0 : 1, outColumn };
    // The left half runs away from the axis: output column 0 is the farthest.
    if (outColumn < s.seam)
        return { 0, s.axisColumn + (s.seam - 1 - outColumn) };
    return { 1, s.axisColumn + (outColumn - s.seam) };
}

/// The seam as a column of a slice drawn with pixel \a stride and origin
/// \a origin (the slice's own frame: column i sits at origin + i * stride),
/// from a position given as a pixel index of the full plane (pixel centres at
/// integers; a fraction when the coordinate lies between pixels). The column
/// where the right half starts: a position exactly between two columns puts
/// the seam between them.
inline int seamColumnFromIndex(double fullPlaneIndex, double origin, double stride)
{
    if (!(stride > 0.0) || !std::isfinite(fullPlaneIndex))
        return 0;
    return static_cast<int>(std::lround((fullPlaneIndex - origin) / stride));
}

/// The split composite: \a a and \a b are the two fields' colours (width x
/// height each, 4 bytes a pixel), \a out receives layout.outWidth x height
/// pixels. A field's own opacity weights its half.
inline void composeSplit(const SplitLayout &layout, const std::uint8_t *a, const std::uint8_t *b,
                         double opacityA, double opacityB, int height, blend::Rgba8 *out)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < layout.outWidth; ++x) {
            const SplitSource src = splitSource(layout, x);
            blend::Rgba8 &px = out[static_cast<std::size_t>(y) * layout.outWidth + x];
            px = {};
            const std::uint8_t *field = src.field == 0 ? a : b;
            if (!field || src.column < 0 || src.column >= layout.sourceWidth)
                continue;
            OverAccum acc;
            accumulateOver(acc,
                           &field[(static_cast<std::size_t>(y) * layout.sourceWidth + src.column) * 4],
                           src.field == 0 ? opacityA : opacityB);
            px = finishOver(acc);
        }
    }
}

// ── 3-D opacity ─────────────────────────────────────────────────────────────

/// A field's overall opacity (0..1) scales the ramp of its opacity function;
/// a ramp is a list of (value, opacity) nodes.
inline void scaleOpacityNodes(std::vector<std::pair<double, double>> &nodes, double opacity)
{
    const double k = std::clamp(opacity, 0.0, 1.0);
    for (auto &n : nodes)
        n.second = std::clamp(n.second * k, 0.0, 1.0);
}

}   // namespace visivo::fields

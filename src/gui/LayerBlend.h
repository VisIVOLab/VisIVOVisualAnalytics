#pragma once
// LayerBlend.h
// The arithmetic of the image viewer's Additive blend mode: how the colours of
// the visible layers add up, which single-hue colour map "Make RGB Composite"
// gives each layer, and how a layer is resampled onto the base image's grid.
// Standard C++ only (no Qt, no VTK), so it is unit-tested; the window feeds it
// the colours its layers' lookup tables produce.
//
// Normal blending draws each layer over the one below it, weighted by its
// opacity, so a red, a green and a blue layer wash each other out and an
// overlap never reaches white. Additive blending SUMS the layers' colours per
// pixel and clips at 1, which is how a three-colour composite is made: each
// layer is mapped black -> its hue, and the sum is the picture. A layer's
// opacity is its weight in the sum.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace visivo::blend {

struct Rgba8
{
    std::uint8_t r{ 0 }, g{ 0 }, b{ 0 }, a{ 0 };
};

/// The running sum for one pixel. \a any stays false while no layer had data
/// there, which is what keeps the picture transparent outside the inputs.
struct Accum
{
    float r{ 0 }, g{ 0 }, b{ 0 };
    bool any{ false };
};

/// Adds a layer's colour (the lookup table's 8-bit RGBA) to \a acc, scaled by
/// the layer's \a weight (its opacity) and by the colour's own alpha.
inline void accumulate(Accum &acc, const std::uint8_t rgba[4], double weight)
{
    acc.any = true;
    const float k = static_cast<float>(std::max(0.0, weight) * rgba[3] / (255.0 * 255.0));
    acc.r += k * rgba[0];
    acc.g += k * rgba[1];
    acc.b += k * rgba[2];
}

/// The pixel \a acc adds up to: each component clipped at 1. Opaque whenever a
/// layer had data (even if it sums to black), transparent otherwise.
inline Rgba8 finish(const Accum &acc)
{
    if (!acc.any)
        return {};
    const auto to8 = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
    };
    return { to8(acc.r), to8(acc.g), to8(acc.b), 255 };
}

/// The black -> hue colour map "Make RGB Composite" gives layer \a index of
/// \a count. Three layers are exactly red, green, blue; two are red and blue;
/// four or five are spread over the same red -> blue run (red, yellow, green,
/// cyan, blue: longest wavelength red, as in a false-colour picture); more than
/// five cycle through those hues and magenta, one per layer.
inline const char *hueMapName(int index, int count)
{
    static const char *const kMaps[] = { "PureRed",  "PureYellow", "PureGreen",
                                         "PureCyan", "PureBlue",   "PureMagenta" };
    if (count < 2)
        return kMaps[0];
    index = std::clamp(index, 0, count - 1);
    if (count > 5)
        return kMaps[index % 6];
    return kMaps[std::lround(4.0 * index / (count - 1))];
}

// ── Resampling onto the base image's grid ───────────────────────────────────

/// x' = a x + b y + e, y' = c x + d y + f: how pixel indices of one grid land
/// on another. A layer is stored with its own spacing, origin and rotation; the
/// blend needs it in the base image's pixel indices.
struct Affine2D
{
    double a{ 1 }, b{ 0 }, c{ 0 }, d{ 1 }, e{ 0 }, f{ 0 };

    std::array<double, 2> apply(double x, double y) const
    {
        return { a * x + b * y + e, c * x + d * y + f };
    }
    /// The inverse; \a ok is false for a singular map (spacing 0).
    Affine2D inverse(bool *ok = nullptr) const
    {
        const double det = a * d - b * c;
        const bool good = std::isfinite(det) && std::fabs(det) > 1e-300;
        if (ok)
            *ok = good;
        if (!good)
            return {};
        Affine2D r;
        r.a = d / det;
        r.b = -b / det;
        r.c = -c / det;
        r.d = a / det;
        r.e = -(r.a * e + r.b * f);
        r.f = -(r.c * e + r.d * f);
        return r;
    }
    /// The map that applies \a first, then this one.
    Affine2D after(const Affine2D &first) const
    {
        Affine2D r;
        r.a = a * first.a + b * first.c;
        r.b = a * first.b + b * first.d;
        r.c = c * first.a + d * first.c;
        r.d = c * first.b + d * first.d;
        r.e = a * first.e + b * first.f + e;
        r.f = c * first.e + d * first.f + f;
        return r;
    }
};

/// A read-only float image: \a data holds width * height values, row 0 first.
struct FloatImage
{
    const float *data{ nullptr };
    int width{ 0 };
    int height{ 0 };
};

/// The value of \a src at fractional pixel index (x, y): bilinear over the
/// FINITE neighbours (a blank neighbour does not darken the edge next to it),
/// NaN outside the image or when all four neighbours are blank. Pixel centres
/// are at integer indices, so a sample on a pixel returns that pixel exactly.
inline float sampleBilinear(const FloatImage &src, double x, double y)
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    if (!src.data || !(x > -0.5) || !(y > -0.5) || !(x < src.width - 0.5)
        || !(y < src.height - 0.5))
        return nan;
    const double fx = std::floor(x), fy = std::floor(y);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const double tx = x - fx, ty = y - fy;
    double acc = 0, wsum = 0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int xi = std::clamp(x0 + i, 0, src.width - 1);
            const int yi = std::clamp(y0 + j, 0, src.height - 1);
            const float v = src.data[static_cast<std::size_t>(yi) * static_cast<std::size_t>(src.width)
                                     + static_cast<std::size_t>(xi)];
            if (!std::isfinite(v))
                continue;
            const double w = (i ? tx : 1.0 - tx) * (j ? ty : 1.0 - ty);
            acc += w * v;
            wsum += w;
        }
    }
    return wsum > 0.0 ? static_cast<float>(acc / wsum) : nan;
}

/// Rows [row0, row1) of the output, which is \a src seen on the base grid
/// decimated by \a step: output pixel (ox, oy) stands for the step x step block
/// of base pixels centred at base index ((ox + 0.5) step - 0.5, ...).
/// \a srcToBase takes a source pixel index to a base pixel index. With step > 1
/// the block is averaged over a grid of at most 4 x 4 sub-samples, so a point
/// source does not fall between samples when the image is shown reduced.
/// \a out receives (row1 - row0) * outWidth floats, NaN where \a src has no data.
inline void resampleRows(const FloatImage &src, const Affine2D &srcToBase, int step, int outWidth,
                         int row0, int row1, float *out)
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    bool ok = false;
    const Affine2D baseToSrc = srcToBase.inverse(&ok);
    if (!ok || step < 1) {
        const std::size_t n = static_cast<std::size_t>(std::max(0, row1 - row0))
                * static_cast<std::size_t>(std::max(0, outWidth));
        std::fill(out, out + n, nan);
        return;
    }
    const int sub = std::min(step, 4);
    for (int oy = row0; oy < row1; ++oy) {
        float *row = out + static_cast<std::size_t>(oy - row0) * static_cast<std::size_t>(outWidth);
        for (int ox = 0; ox < outWidth; ++ox) {
            double acc = 0;
            int hits = 0;
            for (int sy = 0; sy < sub; ++sy) {
                for (int sx = 0; sx < sub; ++sx) {
                    const double bx = (ox + (sx + 0.5) / sub) * step - 0.5;
                    const double by = (oy + (sy + 0.5) / sub) * step - 0.5;
                    const auto p = baseToSrc.apply(bx, by);
                    const float v = sampleBilinear(src, p[0], p[1]);
                    if (std::isfinite(v)) {
                        acc += v;
                        ++hits;
                    }
                }
            }
            row[ox] = hits > 0 ? static_cast<float>(acc / hits) : nan;
        }
    }
}

/// The decimation step that brings a width x height base grid within
/// \a pixelBudget pixels for display: 1 when it already fits.
inline int displayStep(int width, int height, long long pixelBudget)
{
    if (width <= 0 || height <= 0 || pixelBudget <= 0)
        return 1;
    int step = 1;
    while (static_cast<long long>((width + step - 1) / step)
                   * static_cast<long long>((height + step - 1) / step)
           > pixelBudget)
        ++step;
    return step;
}

// ── Values to colours: the rule the image blend and the field composite share ──

/// \a src clamped to the table's range (either order), NaN kept. A saturated
/// pixel then takes the table's top colour whatever it does outside its range.
inline void clampToRange(const float *src, std::size_t n, double rangeA, double rangeB, float *out)
{
    const float lo = static_cast<float>(std::min(rangeA, rangeB));
    const float hi = static_cast<float>(std::max(rangeA, rangeB));
    for (std::size_t i = 0; i < n; ++i)
        out[i] = std::isnan(src[i]) ? src[i] : std::clamp(src[i], lo, hi);
}

/// The no-data rule: a NaN value is a transparent pixel, whatever colour the
/// table gives NaN. \a rgba holds 4 bytes a pixel.
inline void clearNoData(const float *values, std::size_t n, std::uint8_t *rgba)
{
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isnan(values[i]))
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = 0;
    }
}

}   // namespace visivo::blend

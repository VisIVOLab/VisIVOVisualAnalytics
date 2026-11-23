#pragma once

// SedPlotStyle.h
// How the SED window draws the photometry: which survey a band belongs to,
// the colour each survey gets in the light and dark themes, the axis ranges
// and the line joining the bands in the fit.
//
// No widgets and no QtGui, so it can be tested.

#include "SedBuilder.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace visivo::SedPlot {

struct Rgb
{
    int r{ 0 };
    int g{ 0 };
    int b{ 0 };
};

/// The plot sheet of each theme. They mirror VisivoTheme::kSurface(), which
/// needs QtWidgets; the window paints with the theme's own value, the tests
/// check contrast against these.
inline constexpr Rgb kLightSurface{ 0xFF, 0xFF, 0xFF };
inline constexpr Rgb kDarkSurface{ 0x09, 0x0F, 0x14 };

/// Opacity of a band left out of the fit: still read, but behind the ones in it.
inline constexpr double kUnusedAlpha = 0.55;

enum class Family { TwoMass, Wise, Msx, SpitzerIrac, SpitzerMips, Pacs, Spire, Other };

/// Legend order, short to long wavelength.
inline const std::vector<Family> &familyOrder()
{
    static const std::vector<Family> kOrder{ Family::TwoMass,     Family::Wise,
                                             Family::SpitzerIrac, Family::Msx,
                                             Family::SpitzerMips, Family::Pacs,
                                             Family::Spire,       Family::Other };
    return kOrder;
}

inline QString familyName(Family family)
{
    switch (family) {
    case Family::TwoMass: return QStringLiteral("2MASS");
    case Family::Wise: return QStringLiteral("WISE");
    case Family::Msx: return QStringLiteral("MSX");
    case Family::SpitzerIrac: return QStringLiteral("Spitzer IRAC");
    case Family::SpitzerMips: return QStringLiteral("Spitzer MIPS");
    case Family::Pacs: return QStringLiteral("Herschel PACS");
    case Family::Spire: return QStringLiteral("Herschel SPIRE");
    case Family::Other: break;
    }
    return QStringLiteral("Other");
}

/// The survey a band comes from. The catalogue's tag decides when it names the
/// survey ("2MASS Ks", "WISE W1", "MSX A", "21_c"); a bare number is matched
/// to the nearest survey band within 1.5 %, which keeps WISE 4.6 µm apart from
/// IRAC 4.5 µm and WISE 12 µm apart from MSX 12.13 µm. A 70 µm band is PACS:
/// the band-merged catalogues this window reads are Hi-GAL's.
inline Family bandFamily(const QString &tag, double wavelengthUm)
{
    const QString key = tag.trimmed().toUpper();
    if (key.startsWith(QStringLiteral("2MASS")))
        return Family::TwoMass;
    if (key.startsWith(QStringLiteral("WISE")))
        return Family::Wise;
    if (key.startsWith(QStringLiteral("MSX")) || key == QStringLiteral("21")
        || key.startsWith(QStringLiteral("21_")))
        return Family::Msx;
    if (key.startsWith(QStringLiteral("IRAC")))
        return Family::SpitzerIrac;
    if (key.startsWith(QStringLiteral("MIPS")))
        return Family::SpitzerMips;
    if (key.startsWith(QStringLiteral("PACS")))
        return Family::Pacs;
    if (key.startsWith(QStringLiteral("SPIRE")))
        return Family::Spire;

    bool numeric = false;
    const double tagged = key.toDouble(&numeric);
    const double lambda = (numeric && tagged > 0.0) ? tagged : wavelengthUm;
    if (!(lambda > 0.0) || !std::isfinite(lambda))
        return Family::Other;

    static const std::vector<std::pair<double, Family>> kBands{
        { 1.235, Family::TwoMass }, { 1.662, Family::TwoMass }, { 2.159, Family::TwoMass },
        { 3.4, Family::Wise },      { 4.6, Family::Wise },      { 12.0, Family::Wise },
        { 22.0, Family::Wise },     { 3.6, Family::SpitzerIrac }, { 4.5, Family::SpitzerIrac },
        { 5.8, Family::SpitzerIrac }, { 8.0, Family::SpitzerIrac }, { 8.28, Family::Msx },
        { 12.13, Family::Msx },     { 14.65, Family::Msx },     { 21.34, Family::Msx },
        { 24.0, Family::SpitzerMips }, { 70.0, Family::Pacs },  { 100.0, Family::Pacs },
        { 160.0, Family::Pacs },    { 250.0, Family::Spire },   { 350.0, Family::Spire },
        { 500.0, Family::Spire },
    };
    double best = 0.015;
    Family family = Family::Other;
    for (const auto &[band, owner] : kBands) {
        const double offset = std::abs(lambda - band) / band;
        if (offset < best) {
            best = offset;
            family = owner;
        }
    }
    return family;
}

/// One hue per survey, none of them the blue, orange or green of the fit
/// curves, each at least 3:1 against its theme's plot sheet.
inline Rgb familyColour(Family family, bool dark)
{
    switch (family) {
    case Family::TwoMass: return dark ? Rgb{ 200, 170, 160 } : Rgb{ 121, 85, 72 };
    case Family::Wise: return dark ? Rgb{ 244, 143, 177 } : Rgb{ 173, 20, 87 };
    case Family::Msx: return dark ? Rgb{ 224, 195, 65 } : Rgb{ 130, 100, 0 };
    case Family::SpitzerIrac: return dark ? Rgb{ 159, 168, 218 } : Rgb{ 57, 73, 171 };
    case Family::SpitzerMips: return dark ? Rgb{ 239, 120, 110 } : Rgb{ 198, 40, 40 };
    case Family::Pacs: return dark ? Rgb{ 186, 160, 230 } : Rgb{ 106, 63, 181 };
    case Family::Spire: return dark ? Rgb{ 77, 208, 225 } : Rgb{ 0, 125, 137 };
    case Family::Other: break;
    }
    return dark ? Rgb{ 205, 210, 215 } : Rgb{ 70, 82, 98 };
}

enum class Curve { Thin, Thick, Model };

inline Rgb curveColour(Curve curve, bool dark)
{
    switch (curve) {
    case Curve::Thin: return dark ? Rgb{ 90, 165, 255 } : Rgb{ 25, 95, 200 };
    case Curve::Thick: return dark ? Rgb{ 255, 160, 70 } : Rgb{ 190, 90, 0 };
    case Curve::Model: break;
    }
    return dark ? Rgb{ 120, 215, 135 } : Rgb{ 35, 125, 50 };
}

/// `colour` painted at `alpha` over `surface`.
inline Rgb blend(Rgb colour, double alpha, Rgb surface)
{
    const auto mix = [alpha](int c, int s) {
        return static_cast<int>(std::lround(alpha * c + (1.0 - alpha) * s));
    };
    return { mix(colour.r, surface.r), mix(colour.g, surface.g), mix(colour.b, surface.b) };
}

/// WCAG 2 contrast ratio, 1 (none) to 21 (black on white).
inline double contrastRatio(Rgb a, Rgb b)
{
    const auto channel = [](int c) {
        const double v = c / 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    const auto luminance = [&channel](Rgb c) {
        return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

struct AxisRange
{
    double lower{ 0.0 };
    double upper{ 0.0 };
    bool valid{ false };
};

namespace detail {

inline AxisRange padded(double lo, double hi, bool logScale, double logBelow, double logAbove,
                        double linearFraction)
{
    if (!(lo <= hi) || !std::isfinite(lo) || !std::isfinite(hi))
        return {};
    if (logScale)
        return { lo / logBelow, hi * logAbove, true };
    double span = hi - lo;
    if (span <= 0.0)
        span = lo != 0.0 ? std::abs(lo) * 5.0 * linearFraction : 1.0;
    return { lo - linearFraction * span, hi + linearFraction * span, true };
}

}   // namespace detail

/// The wavelength axis: every band, with a margin so no point sits on the frame.
inline AxisRange wavelengthRange(const std::vector<SedPoint> &points, bool logScale)
{
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (const SedPoint &point : points) {
        const double w = point.wavelengthUm;
        if (!std::isfinite(w) || (logScale && w <= 0.0))
            continue;
        lo = std::min(lo, w);
        hi = std::max(hi, w);
    }
    return detail::padded(lo, hi, logScale, 1.4, 1.4, 0.06);
}

/// The flux axis, from the photometry alone: every band in the fit or not,
/// upper limits included, and the top of each error bar. The model curves are
/// left out on purpose; a greybody falls to 1e-60 Jy at 5 µm, and following it
/// would crush the data into the top of the plot. On a log axis the margin is
/// a decade below the lowest flux, which also leaves room for the upper-limit
/// arrows and the lower error bars, and half a decade above the highest; an
/// error bar reaching further down than that is cut by the frame.
inline AxisRange fluxRange(const std::vector<SedPoint> &points, bool logScale)
{
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    const auto take = [&](double v) {
        if (!std::isfinite(v) || (logScale && v <= 0.0))
            return;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };
    for (const SedPoint &point : points) {
        take(point.fluxJy);
        if (point.upperLimit || !(point.errorJy > 0.0))
            continue;
        take(point.fluxJy + point.errorJy);
        if (!logScale)
            take(point.fluxJy - point.errorJy);
    }
    return detail::padded(lo, hi, logScale, 10.0, std::sqrt(10.0), 0.08);
}

/// The line through the bands in the fit, in wavelength order. A band with two
/// ticked counterparts has no single flux, so the line passes it by rather than
/// jumping up and down between them; the markers still show both.
inline std::vector<std::pair<double, double>> fitLine(const std::vector<SedPoint> &points)
{
    std::vector<std::pair<double, double>> used;
    for (const SedPoint &point : points) {
        if (point.use && !point.upperLimit && std::isfinite(point.wavelengthUm)
            && std::isfinite(point.fluxJy))
            used.emplace_back(point.wavelengthUm, point.fluxJy);
    }
    std::stable_sort(used.begin(), used.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    const auto sameBand = [](double a, double b) {
        return std::abs(a - b) <= 1e-3 * std::max(std::abs(a), std::abs(b));
    };
    std::vector<std::pair<double, double>> line;
    for (std::size_t i = 0; i < used.size();) {
        std::size_t j = i + 1;
        while (j < used.size() && sameBand(used[j].first, used[i].first))
            ++j;
        if (j - i == 1)
            line.push_back(used[i]);
        i = j;
    }
    return line;
}

}   // namespace visivo::SedPlot

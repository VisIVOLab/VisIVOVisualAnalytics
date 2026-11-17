#ifndef POWERSPECTRUMSLOPE_H
#define POWERSPECTRUMSLOPE_H
// PowerSpectrumSlope.h
// The slope of an angular power spectrum, P(ℓ) ∝ ℓ^n, fitted to the binned
// spectrum the backend returns (POST /v1/cosmology/angular_power). The backend
// gives the bins; it does not fit them, so the fit is the client's.
//
// A straight line through (log10 ℓ, log10 P), WEIGHTED by the per-bin errors
// the backend returns (cl_err, already corrected for the window's mode
// coupling): σ_log = cl_err / (P ln 10). The low-ℓ bins hold far fewer modes
// than the high-ℓ ones, so their errors differ by orders of magnitude, and an
// unweighted fit both scatters 2–3× more and reports an error ~2× too small
// (science review of issue #29: 30 Gaussian random fields per case through the
// backend's own worker; the weighted error matched the true scatter). A bin
// whose error is null (fewer than 3 modes) is left out. Without any errors (an
// older backend) the fit falls back to ordinary least squares with the error
// from the scatter about the line.
//
// The weighted fit also measures whether ONE power law describes the bins at
// all: χ² of the residuals in log P over the degrees of freedom. A real map
// often curves (issue #403: the Moment 0 of a cosmological box runs from
// k^-1.2 at large scales to steeper than k^-2.5 near Nyquist, χ²/dof = 47 over
// all bins).
// Because the high-ℓ bins hold thousands of modes their weights are ~1000×
// those of the low-ℓ ones, so the line then follows the high-ℓ bins and sits
// well above the low-ℓ ones. That line IS the fit; what was wrong was a slope
// error computed as if the model were right. When χ² rejects the power law
// (p < 0.001) the error is scaled by √(χ²/dof), as a fit to inconsistent data
// is conventionally reported, and the caller says so next to the slope.
//
// A bin is kept when its power is positive, its ℓ is inside the chosen range,
// and it holds at least `minModes` Fourier modes.
//
// QtCore only: pinned by tests/test_cosmology_tools.cpp.

#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace visivo {

struct PowerSlopeOptions
{
    double ellMin{ 0.0 };   ///< 0 = from the first bin
    double ellMax{ 0.0 };   ///< 0 = to the last bin
    int minModes{ 8 };      ///< drop bins with fewer Fourier modes than this
};

struct PowerSlopeFit
{
    bool valid{ false };
    bool weighted{ false };   ///< true: weighted by cl_err; false: OLS fallback
    double slope{ std::numeric_limits<double>::quiet_NaN() };       ///< n in P ∝ ℓ^n
    double slopeError{ std::numeric_limits<double>::quiet_NaN() };  ///< 1σ
    double intercept{ std::numeric_limits<double>::quiet_NaN() };   ///< log10 P at ℓ = 1
    /// χ² of the weighted residuals in log10 P over binsUsed − 2; NaN when
    /// unweighted (the scatter then IS the error, so χ² says nothing).
    double chi2PerDof{ std::numeric_limits<double>::quiet_NaN() };
    /// The weighted residuals reject a single power law (p < 0.001): the slope
    /// is dominated by the bins with the smallest errors and slopeError has
    /// been scaled by √(χ²/dof).
    bool powerLawRejected{ false };
    int binsUsed{ 0 };
    double ellLow{ std::numeric_limits<double>::quiet_NaN() };   ///< first ℓ actually used
    double ellHigh{ std::numeric_limits<double>::quiet_NaN() };  ///< last ℓ actually used
};

/// @p nModes and @p powerErr may be empty (an older backend): the mode test
/// then passes every bin, and the fit is unweighted.
inline PowerSlopeFit fitPowerSlope(const QVector<double> &ell, const QVector<double> &power,
                                   const QVector<int> &nModes,
                                   const PowerSlopeOptions &opt = {},
                                   const QVector<double> &powerErr = {})
{
    PowerSlopeFit fit;
    const int n = static_cast<int>(std::min(ell.size(), power.size()));
    const bool weighted = powerErr.size() >= n && n > 0;
    QVector<double> xs, ys, ws;
    xs.reserve(n);
    ys.reserve(n);
    ws.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double l = ell[i];
        const double p = power[i];
        if (!(std::isfinite(l) && l > 0.0 && std::isfinite(p) && p > 0.0))
            continue;
        if (opt.ellMin > 0.0 && l < opt.ellMin)
            continue;
        if (opt.ellMax > 0.0 && l > opt.ellMax)
            continue;
        if (i < nModes.size() && nModes[i] < opt.minModes)
            continue;
        double w = 1.0;
        if (weighted) {
            const double e = powerErr[i];
            if (!(std::isfinite(e) && e > 0.0))
                continue;   // no error estimable: the bin carries no weight
            const double sigmaLog = e / (p * std::log(10.0));
            w = 1.0 / (sigmaLog * sigmaLog);
        }
        xs.append(std::log10(l));
        ys.append(std::log10(p));
        ws.append(w);
        if (fit.binsUsed == 0)
            fit.ellLow = l;
        fit.ellHigh = l;
        ++fit.binsUsed;
    }
    fit.weighted = weighted;
    if (fit.binsUsed < 3)
        return fit;   // two points always fit exactly: no slope error to state

    double sw = 0.0, mx = 0.0, my = 0.0;
    for (int i = 0; i < fit.binsUsed; ++i) {
        sw += ws[i];
        mx += ws[i] * xs[i];
        my += ws[i] * ys[i];
    }
    mx /= sw;
    my /= sw;
    double sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < fit.binsUsed; ++i) {
        sxx += ws[i] * (xs[i] - mx) * (xs[i] - mx);
        sxy += ws[i] * (xs[i] - mx) * (ys[i] - my);
    }
    if (!(sxx > 0.0))
        return fit;
    fit.slope = sxy / sxx;
    fit.intercept = my - fit.slope * mx;
    if (weighted) {
        // The errors are absolute: the slope's variance is 1 / Σ w (x − x̄)².
        fit.slopeError = std::sqrt(1.0 / sxx);
        double chi2 = 0.0;
        for (int i = 0; i < fit.binsUsed; ++i) {
            const double r = ys[i] - (fit.intercept + fit.slope * xs[i]);
            chi2 += ws[i] * r * r;
        }
        const int dof = fit.binsUsed - 2;
        fit.chi2PerDof = chi2 / dof;
        // Wilson–Hilferty: (χ²/ν)^⅓ is close to normal with mean 1 − 2/(9ν)
        // and variance 2/(9ν), good to a few per cent in p from ν ≈ 1. z > 3.09
        // is p < 0.001, far enough out that a correct model with honest errors
        // almost never trips it, so the validated error stays untouched there.
        const double v = 2.0 / (9.0 * dof);
        const double z = (std::cbrt(fit.chi2PerDof) - (1.0 - v)) / std::sqrt(v);
        if (z > 3.09) {
            fit.powerLawRejected = true;
            fit.slopeError *= std::sqrt(fit.chi2PerDof);
        }
    } else {
        double rss = 0.0;
        for (int i = 0; i < fit.binsUsed; ++i) {
            const double r = ys[i] - (fit.intercept + fit.slope * xs[i]);
            rss += r * r;
        }
        fit.slopeError = std::sqrt(rss / (fit.binsUsed - 2) / sxx);
    }
    fit.valid = std::isfinite(fit.slope);
    return fit;
}

/// The fitted power at @p ell, in the same units as the bins it was fitted
/// to: what the plot draws, so a test can hold the drawn line to the data.
inline double fittedPower(const PowerSlopeFit &fit, double ell)
{
    if (!fit.valid || !(ell > 0.0))
        return std::numeric_limits<double>::quiet_NaN();
    return std::pow(10.0, fit.intercept + fit.slope * std::log10(ell));
}

/// The slope line under the plot, also stored as the run's "Slope" parameter:
/// "Slope n = -1.200 ± 0.034 (P ∝ kⁿ) over k 0.02118–0.1848, 26 bins, each
/// weighted by 1/σ² of log P (χ²/dof = 1.36)". When χ² rejects one power law it
/// says so, since the line then follows only the bins with the smallest errors.
inline QString powerSlopeSummary(const PowerSlopeFit &fit, bool ellIsAngular)
{
    const QString axis = ellIsAngular ? QStringLiteral("ℓ") : QStringLiteral("k");
    if (!fit.valid)
        return QStringLiteral("No slope: fewer than 3 bins left in the fit range.");
    QString weighting;
    if (!fit.weighted)
        weighting = QStringLiteral("unweighted (the server sent no errors)");
    else if (fit.powerLawRejected)
        weighting = QStringLiteral("each weighted by 1/σ² of log P. One power law does not fit "
                                   "these bins (χ²/dof = %1): the line follows the bins with the "
                                   "smallest errors, at high %2, and the error is scaled by "
                                   "√(χ²/dof). Narrow the fit range.")
                            .arg(fit.chi2PerDof, 0, 'f', 1)
                            .arg(axis);
    else
        weighting = QStringLiteral("each weighted by 1/σ² of log P (χ²/dof = %1)")
                            .arg(fit.chi2PerDof, 0, 'f', 2);
    return QStringLiteral("Slope n = %1 ± %2 (P ∝ %3ⁿ) over %3 %4–%5, %6 bins, %7")
            .arg(fit.slope, 0, 'f', 3)
            .arg(fit.slopeError, 0, 'f', 3)
            .arg(axis)
            .arg(fit.ellLow, 0, 'g', 4)
            .arg(fit.ellHigh, 0, 'g', 4)
            .arg(fit.binsUsed)
            .arg(weighting);
}

}   // namespace visivo

#endif   // POWERSPECTRUMSLOPE_H

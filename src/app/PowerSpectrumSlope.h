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
// A bin is kept when its power is positive, its ℓ is inside the chosen range,
// and it holds at least `minModes` Fourier modes.
//
// QtCore only: pinned by tests/test_cosmology_tools.cpp.

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

}   // namespace visivo

#endif   // POWERSPECTRUMSLOPE_H

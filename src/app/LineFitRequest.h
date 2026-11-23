#ifndef LineFitRequest_h
#define LineFitRequest_h

// LineFitRequest.h
// The Gaussian line fit's request and answer (POST /v1/spectral/linefit), and
// the rules around them that decide whether a fit is right: which part of the
// spectrum is fitted, and when the result has to be flagged.
//
// A fit over the whole spectrum locks onto whatever is strongest anywhere in
// it, an edge spike as readily as the line (issue #416). The profiler
// therefore fits what the user is looking at by default. Header-only and free
// of widgets so the unit tests can exercise it directly.

#include "BackendClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace visivo::linefit {

/// Which part of the spectrum the fit sees.
enum class RangeMode {
    Visible,   ///< the x-range currently shown on the plot
    Typed,     ///< a range the user typed
    Whole,     ///< every channel
};

/// The fit window sent to the backend. Inactive means "fit everything", and
/// then no x_min/x_max is sent at all.
struct FitWindow
{
    bool active{ false };
    double xMin{ 0.0 };
    double xMax{ 0.0 };
};

/// A window from a range on the x axis, in either order. It is active only
/// when it leaves some of the data out: a zoomed-out plot fits the whole
/// spectrum, and saying so with an explicit window would change nothing but
/// the request.
inline FitWindow windowFromRange(double lower, double upper, const QVector<double> &keys)
{
    FitWindow window;
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower == upper)
        return window;
    if (lower > upper)
        std::swap(lower, upper);
    bool leavesSomethingOut = false;
    for (double key : keys) {
        if (std::isfinite(key) && (key < lower || key > upper)) {
            leavesSomethingOut = true;
            break;
        }
    }
    if (!leavesSomethingOut)
        return window;
    window.active = true;
    window.xMin = lower;
    window.xMax = upper;
    return window;
}

/// Finite channels the fit would see through @p window.
inline int channelsInWindow(const QVector<double> &keys, const QVector<double> &values,
                            const FitWindow &window)
{
    int count = 0;
    const int n = static_cast<int>(std::min(keys.size(), values.size()));
    for (int i = 0; i < n; ++i) {
        const double key = keys.at(i);
        if (!std::isfinite(key) || !std::isfinite(values.at(i)))
            continue;
        if (window.active && (key < window.xMin || key > window.xMax))
            continue;
        ++count;
    }
    return count;
}

inline QJsonObject requestBody(const QVector<double> &values, const QVector<double> &axis,
                               bool withBaseline, const FitWindow &window)
{
    QJsonArray valueArray;
    QJsonArray axisArray;
    for (double value : values)
        valueArray.append(value);
    for (double key : axis)
        axisArray.append(key);
    QJsonObject body{
        { QStringLiteral("values"), valueArray },
        { QStringLiteral("axis"), axisArray },
        { QStringLiteral("with_baseline"), withBaseline },
    };
    if (window.active) {
        body.insert(QStringLiteral("x_min"), window.xMin);
        body.insert(QStringLiteral("x_max"), window.xMax);
    }
    return body;
}

inline BackendLineFitResult parseResponse(const QJsonObject &obj)
{
    BackendLineFitResult result;
    result.valid          = obj.value(QStringLiteral("valid")).toBool();
    result.error          = obj.value(QStringLiteral("error")).toString();
    result.peak           = obj.value(QStringLiteral("peak")).toDouble();
    result.center         = obj.value(QStringLiteral("center")).toDouble();
    result.sigma          = obj.value(QStringLiteral("sigma")).toDouble();
    result.fwhm           = obj.value(QStringLiteral("fwhm")).toDouble();
    result.integratedFlux = obj.value(QStringLiteral("integrated_flux")).toDouble();
    result.continuum      = obj.value(QStringLiteral("continuum")).toDouble();
    result.rms            = obj.value(QStringLiteral("rms")).toDouble();
    // Errors are null when the covariance was undefined; keep the -1 sentinel so
    // the caller can tell "no uncertainty available" from "uncertainty of zero".
    const auto err = [&obj](const char *key) -> double {
        const QJsonValue v = obj.value(QLatin1String(key));
        return v.isDouble() ? v.toDouble() : -1.0;
    };
    result.peakErr           = err("peak_err");
    result.centerErr         = err("center_err");
    result.sigmaErr          = err("sigma_err");
    result.fwhmErr           = err("fwhm_err");
    result.integratedFluxErr = err("integrated_flux_err");
    // 0 = the field is ABSENT (a backend older than this correction), which is not
    // the same as 1 = "measured, and the channels are independent".
    result.spectralCorrelationFactor =
            obj.value(QStringLiteral("spectral_correlation_factor")).toDouble(0.0);
    result.spectralCorrelationNote =
            obj.value(QStringLiteral("spectral_correlation_note")).toString();
    result.fitWarning = obj.value(QStringLiteral("fit_warning")).toString();
    const QJsonArray baseline = obj.value(QStringLiteral("baseline")).toArray();
    if (baseline.size() >= 2) {
        result.baselineIntercept = baseline.at(0).toDouble();
        result.baselineSlope     = baseline.at(1).toDouble();
    }
    return result;
}

/// An emission spectrum rises further above its median than it falls below
/// it. Only the channels inside @p window count: that is the spectrum fitted.
inline bool looksLikeEmission(const QVector<double> &keys, const QVector<double> &values,
                              const FitWindow &window)
{
    QVector<double> inside;
    const int n = static_cast<int>(std::min(keys.size(), values.size()));
    for (int i = 0; i < n; ++i) {
        const double key = keys.at(i);
        const double value = values.at(i);
        if (!std::isfinite(key) || !std::isfinite(value))
            continue;
        if (window.active && (key < window.xMin || key > window.xMax))
            continue;
        inside.append(value);
    }
    if (inside.size() < 3)
        return true;
    std::sort(inside.begin(), inside.end());
    const double median = inside.at(inside.size() / 2);
    return inside.last() - median >= median - inside.first();
}

/// What the user has to be told about a fit, or an empty string. The
/// backend's own warning comes first; a negative peak on an emission
/// spectrum is said whether or not the backend said anything, because that
/// fit has found a dip in the baseline or the noise, not the line.
inline QString warningText(const BackendLineFitResult &fit, bool emission)
{
    QStringList parts;
    if (!fit.fitWarning.trimmed().isEmpty())
        parts.append(fit.fitWarning.trimmed());
    if (emission && std::isfinite(fit.peak) && fit.peak < 0.0) {
        parts.append(QStringLiteral(
                "The fitted peak is negative on an emission spectrum: the fit found a dip "
                "in the baseline or the noise, not the line. Zoom on the line, or type a "
                "fit range around it, and fit again."));
    }
    return parts.join(QLatin1Char('\n'));
}

}   // namespace visivo::linefit

#endif   // LineFitRequest_h

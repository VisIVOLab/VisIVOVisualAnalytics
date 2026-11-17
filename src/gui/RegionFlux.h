#ifndef REGION_FLUX_H
#define REGION_FLUX_H
// RegionFlux.h
// Beam-aware flux of a region of a radio map (issue #401).
//
// A map in Jy/beam sums to a flux once divided by the beam area in pixels,
// Ω = π·BMAJ·BMIN / (4 ln 2) / |CDELT1·CDELT2|. A moment-0 map is in Jy/beam
// times the spectral unit, so its flux is in Jy times that unit: Jy m/s for a
// cube whose velocity axis is in m/s, which is given here in Jy km/s, the unit
// HI fluxes are quoted in. With a background region its mean is subtracted
// and its scatter σ gives the error σ·√(N/Ω): the N pixels hold N/Ω
// independent beams. The backend's `/v1/analysis/hi-products` region flux
// uses the same rules (backend/app/compute/hi.py).
//
// Pure QtCore and header-only so it is unit-tested without a window
// (tests/test_region_flux.cpp).

#include "Catalogue3DParser.h"   // the local Planck18 distance integrator
#include "RegionModel.h"

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace visivo::flux {

/// What the pixel sum ÷ beam area of a map in @p bunit is.
struct FluxUnit
{
    bool perBeam{ false };    ///< the brightness is Jy (or mJy, µJy) per beam
    double scale{ 1.0 };      ///< from the map's unit to `label` (mJy → Jy, m/s → km/s)
    bool velocity{ false };   ///< the map is integrated over a velocity: Jy km/s
    QString spectral;         ///< the spectral part as given ("m/s", "Hz"), empty for none
    QString label;            ///< "Jy", "Jy km/s", "Jy Hz"
    double frequencyToHz{ 0.0 };   ///< the spectral part is a frequency: its unit in Hz
    bool fromFrequency{ false };   ///< converted to km/s with the rest frequency
};

namespace detail {

/// Factor from a frequency unit to Hz; 0 when @p u is not one.
inline double frequencyToHz(QString u)
{
    u = u.remove(QLatin1Char(' '));
    // Case matters: MHz is mega, mHz milli (never a spectral axis).
    if (u.compare(QLatin1String("Hz"), Qt::CaseInsensitive) == 0)
        return 1.0;
    if (u == QLatin1String("kHz") || u == QLatin1String("KHZ") || u == QLatin1String("khz"))
        return 1e3;
    if (u == QLatin1String("MHz") || u == QLatin1String("MHZ"))
        return 1e6;
    if (u == QLatin1String("GHz") || u == QLatin1String("GHZ") || u == QLatin1String("ghz"))
        return 1e9;
    return 0.0;
}

/// Factor from a velocity unit (any FITS/astropy spelling) to km/s; 0 when
/// @p u is not a velocity.
inline double velocityToKms(QString u)
{
    u = u.toLower().remove(QLatin1Char(' ')).remove(QLatin1Char('.')).remove(QLatin1Char('*'));
    u.replace(QStringLiteral("sec"), QStringLiteral("s"));
    // "m/s", "ms-1", "ms^-1", "m.s**-1" (astropy, without CUNIT3) all land here.
    static const QRegularExpression re(QStringLiteral("^(k?)m(/s|s\\^?-1)$"));
    const auto m = re.match(u);
    if (!m.hasMatch())
        return 0.0;
    return m.captured(1).isEmpty() ? 1e-3 : 1.0;
}

} // namespace detail

/// Read a brightness unit: "Jy/beam", "JY/BEAM", "Jy beam-1", "mJy/beam",
/// and a moment-0 unit, the brightness and the spectral unit in any order:
/// "Jy/beam km/s", "JY/BEAM m.s**-1", "Jy/beam.km/s" (CASA), "Jy km s-1
/// beam-1", "Jy km/s/beam". The prefix is case-sensitive as in the FITS
/// standard: mJy is milli, MJy mega. Not per beam ("K", "Jy/pixel", "MJy/sr",
/// empty) gives perBeam false: a sum ÷ beam area of such a map is not a flux.
inline FluxUnit parseFluxUnit(const QString &bunit)
{
    FluxUnit out;
    // The brightness: Jy with an optional, case-sensitive prefix, first.
    static const QRegularExpression jy(QStringLiteral("^\\s*([muµμMk]?)[Jj][Yy](?![A-Za-z])"));
    const auto mj = jy.match(bunit);
    if (!mj.hasMatch())
        return out;
    // "per beam", anywhere after it: "/beam", "beam-1", "beam^-1", "beam**-1".
    static const QRegularExpression beam(
            QStringLiteral("(?:/\\s*beam(?:\\s*-\\s*1)?|beam\\s*(?:\\^|\\*\\*)?\\s*-\\s*1)(?![A-Za-z])"),
            QRegularExpression::CaseInsensitiveOption);
    QString rest = bunit.mid(mj.capturedEnd(0));
    const auto mb = beam.match(rest);
    if (!mb.hasMatch())
        return out;
    rest.remove(mb.capturedStart(0), mb.capturedLength(0));
    out.perBeam = true;
    const QString prefix = mj.captured(1);
    out.scale = prefix.isEmpty()                 ? 1.0
            : prefix == QLatin1String("m")      ? 1e-3
            : prefix == QLatin1String("M")      ? 1e6
            : prefix == QLatin1String("k")      ? 1e3
                                                : 1e-6;   // u, µ
    // The separators left around the spectral unit: "Jy/beam.km/s", "Jy km/s/beam".
    static const QRegularExpression edges(QStringLiteral("^[\\s.*/]+|[\\s.*/]+$"));
    rest.remove(edges);
    out.spectral = rest;
    if (rest.isEmpty()) {
        out.label = QStringLiteral("Jy");
        return out;
    }
    const double kms = detail::velocityToKms(rest);
    if (kms > 0.0) {
        out.velocity = true;
        out.scale *= kms;
        out.label = QStringLiteral("Jy km/s");
        return out;
    }
    // A frequency stays in its own unit (Jy Hz) unless withRestFrequency()
    // converts it; anything else too.
    out.frequencyToHz = detail::frequencyToHz(rest);
    out.label = QStringLiteral("Jy ") + rest;
    return out;
}

/// A frequency-integrated map (Jy/beam Hz) in Jy km/s through the rest
/// frequency: Δv = c·Δν/ν₀, as the backend's HI products convert it
/// (`frequency_width_to_kms`). Unchanged when the map is not integrated over
/// a frequency or @p restHz is not positive.
inline FluxUnit withRestFrequency(FluxUnit u, double restHz)
{
    if (!u.perBeam || !(u.frequencyToHz > 0.0) || !(restHz > 0.0) || !std::isfinite(restHz))
        return u;
    u.scale *= u.frequencyToHz * Catalogue3DParser::detail::kSpeedOfLightKms / restHz;
    u.velocity = true;
    u.fromFrequency = true;
    u.label = QStringLiteral("Jy km/s");
    return u;
}

/// Pixels in one Gaussian beam: π·BMAJ·BMIN / (4 ln 2) over the pixel's solid
/// angle |CDELT1·CDELT2| (all in degrees). 0 when any is missing.
inline double beamAreaPixels(double bmajDeg, double bminDeg, double cdelt1Deg, double cdelt2Deg)
{
    const double pix = std::abs(cdelt1Deg * cdelt2Deg);
    if (!(bmajDeg > 0.0) || !(bminDeg > 0.0) || !(pix > 0.0) || !std::isfinite(bmajDeg)
        || !std::isfinite(bminDeg) || !std::isfinite(pix))
        return 0.0;
    return regions::kPi * bmajDeg * bminDeg / (4.0 * std::log(2.0)) / pix;
}

/// The noise the flux error is taken from.
struct Noise
{
    double mean{ 0.0 };     ///< subtracted from every pixel when `subtract`
    double sigma{ std::numeric_limits<double>::quiet_NaN() };   ///< per pixel, in the map's unit
    bool subtract{ false }; ///< a background region: its mean is subtracted
    long count{ 0 };        ///< the background region's valid pixels (for its mean's error)
};

/// Does a region of a map look clipped, as a moment-0 map computed with a
/// threshold mask is (its masked voxels summed to exact zeros, or none left:
/// NaN)? More than a fifth of its pixels exactly zero or blank. Its σ is then
/// too small and a flux on it biased: faint emission below the threshold is lost (issue #401).
inline bool looksClipped(long valid, long exactZeros, long blanked)
{
    const long total = valid + blanked;
    return total > 0 && 5 * (exactZeros + blanked) > total;
}

struct RegionFlux
{
    bool valid{ false };
    double flux{ 0.0 };   ///< in `unit`
    double error{ std::numeric_limits<double>::quiet_NaN() };   ///< NaN without a noise
    QString unit;
};

/// The flux of @p n valid pixels summing to @p sum on a map in @p unit, with
/// @p beamAreaPx pixels per beam. @p pixelsPerSample is the full-resolution
/// pixels each measured pixel stands for (1 at full resolution, the block area
/// on a reduced map). S = (Σ − N·mean)·A / Ω. Its error is the noise of the
/// region's N·A/Ω beams, σ·√(N·A/Ω), and with a background, in quadrature,
/// that of its mean taken over N_bg·A/Ω beams and subtracted N times,
/// σ·N·A/√(N_bg·A·Ω). A σ that is not positive (a clipped map) gives no error.
inline RegionFlux regionFlux(double sum, long n, double beamAreaPx, const FluxUnit &unit,
                             const Noise &noise, double pixelsPerSample = 1.0)
{
    RegionFlux out;
    if (!unit.perBeam || !(beamAreaPx > 0.0) || n <= 0 || !std::isfinite(sum)
        || !(pixelsPerSample > 0.0))
        return out;
    const double a = pixelsPerSample;
    const double mean = noise.subtract ? noise.mean : 0.0;
    out.flux = (sum - double(n) * mean) * a / beamAreaPx * unit.scale;
    if (std::isfinite(noise.sigma) && noise.sigma > 0.0) {
        const double nFull = double(n) * a;
        double var = noise.sigma * noise.sigma * nFull / beamAreaPx;
        if (noise.subtract && noise.count > 0) {
            const double bgFull = double(noise.count) * a;
            var += noise.sigma * noise.sigma * nFull * nFull / (bgFull * beamAreaPx);
        }
        out.error = std::sqrt(var) * unit.scale;
    }
    out.unit = unit.label;
    out.valid = std::isfinite(out.flux);
    return out;
}

/// σ from the median absolute deviation (1.4826 × MAD) of the finite values;
/// NaN when there are none. Robust to the sources of a map, as a noise
/// estimate of a whole moment-0 map must be. @p values is reordered.
inline double madSigma(std::vector<double> &values)
{
    values.erase(std::remove_if(values.begin(), values.end(),
                                [](double v) { return !std::isfinite(v); }),
                 values.end());
    if (values.empty())
        return std::numeric_limits<double>::quiet_NaN();
    const auto median = [](std::vector<double> &v) {
        const std::size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + long(mid), v.end());
        double m = v[mid];
        if (v.size() % 2 == 0) {
            m = 0.5 * (m + *std::max_element(v.begin(), v.begin() + long(mid)));
        }
        return m;
    };
    const double med = median(values);
    for (double &v : values)
        v = std::abs(v - med);
    return 1.4826 * median(values);
}

/// The background region for @p target: among the background regions with
/// an area (other than the target), the one whose centre is nearest the
/// target's. Hidden ones count: hiding a background annulus to see the map
/// does not stop it being the background. Null when there is none.
inline const regions::Region *nearestBackground(const std::vector<regions::Region> &all,
                                                const regions::Region &target)
{
    const auto centre = [](const regions::Region &r) {
        if (regions::isPointList(r.shape) && !r.points.empty()) {
            regions::Vec2 c{ 0.0, 0.0 };
            for (const auto &p : r.points) {
                c[0] += p[0];
                c[1] += p[1];
            }
            return regions::Vec2{ c[0] / double(r.points.size()), c[1] / double(r.points.size()) };
        }
        return r.center;
    };
    const regions::Vec2 t = centre(target);
    const regions::Region *best = nullptr;
    double bestD = std::numeric_limits<double>::infinity();
    for (const regions::Region &r : all) {
        if (r.id == target.id || !regions::isBackground(r) || !regions::hasArea(r.shape))
            continue;
        const regions::Vec2 c = centre(r);
        const double d = std::hypot(c[0] - t[0], c[1] - t[1]);
        if (d < bestD) {
            bestD = d;
            best = &r;
        }
    }
    return best;
}

/// HI mass in solar masses of a flux @p sJyKms (Jy km/s) at @p dMpc:
/// M_HI = 2.356·10⁵ D² S (optically thin; the constant of the backend's HI
/// products). With the luminosity distance of a redshift @p z, the flux
/// measured in the observed frame: M_HI = 2.356·10⁵ D_L² S / (1 + z)
/// (Meyer et al. 2017, PASA 34, 52).
inline double hiMassSolar(double sJyKms, double dMpc, double z = 0.0)
{
    return 2.356e5 * dMpc * dMpc * sJyKms / (1.0 + std::max(0.0, z));
}

/// Luminosity distance (Mpc) of redshift @p z in Planck18, flat:
/// (1 + z)·D_C(z), with the local Planck18 integrator of the catalogue
/// viewer. Its Ωm = 0.3111 is astropy's Om0 + Onu0 (0.30966 + 0.00144, the
/// massive neutrino), and it agrees with astropy's Planck18, which the backend
/// uses, to 10⁻⁴ up to z = 2 (tests/test_region_flux.cpp).
inline double luminosityDistanceMpc(double z)
{
    return z > 0.0 ? (1.0 + z) * Catalogue3DParser::detail::comovingDistanceMpc(z) : 0.0;
}

/// A distance as typed: "25", "25 Mpc" (Mpc), or a redshift "z=0.0046",
/// "z 0.0046". `fromRedshift` says which; `mpc()` is the distance either way.
struct TypedDistance
{
    bool ok{ false };
    bool fromRedshift{ false };
    double value{ 0.0 };   ///< Mpc, or the redshift
    double mpc() const { return !ok ? 0.0 : fromRedshift ? luminosityDistanceMpc(value) : value; }
    /// The redshift the distance came from, 0 for a typed distance.
    double redshift() const { return ok && fromRedshift ? value : 0.0; }
    /// The HI mass of a flux (Jy km/s) at this distance, with the 1/(1 + z)
    /// of a redshift distance.
    double hiMass(double sJyKms) const { return hiMassSolar(sJyKms, mpc(), redshift()); }
    /// Below cz = 1000 km/s peculiar velocities (~300 km/s) dominate the
    /// distance from a redshift (the catalogue viewer's kUncertainCzKms).
    bool uncertain() const
    {
        return ok && fromRedshift
                && Catalogue3DParser::detail::kSpeedOfLightKms * value
                < Catalogue3DParser::detail::kUncertainCzKms;
    }
    /// "25 Mpc", "z = 0.0046 (D_L 20.5 Mpc, Planck18)".
    QString describe() const
    {
        if (!ok)
            return QString();
        if (!fromRedshift)
            return QStringLiteral("%1 Mpc").arg(value, 0, 'g', 6);
        QString text = QStringLiteral("z = %1 (D_L %2 Mpc, Planck18; M_HI divided by 1 + z)")
                               .arg(value, 0, 'g', 6)
                               .arg(mpc(), 0, 'f', 1);
        if (uncertain())
            text += QStringLiteral("; cz below 1000 km/s: peculiar velocities dominate this distance");
        return text;
    }
};

inline TypedDistance parseDistance(const QString &text)
{
    TypedDistance out;
    static const QRegularExpression z(QStringLiteral("^\\s*z\\s*[=:]?\\s*([0-9.eE+-]+)\\s*$"),
                                      QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression d(QStringLiteral("^\\s*([0-9.eE+-]+)\\s*(mpc)?\\s*$"),
                                      QRegularExpression::CaseInsensitiveOption);
    bool ok = false;
    if (const auto m = z.match(text); m.hasMatch()) {
        const double v = m.captured(1).toDouble(&ok);
        if (ok && std::isfinite(v) && v > 0.0)
            out = { true, true, v };
        return out;
    }
    if (const auto m = d.match(text); m.hasMatch()) {
        const double v = m.captured(1).toDouble(&ok);
        if (ok && std::isfinite(v) && v > 0.0)
            out = { true, false, v };
    }
    return out;
}

/// "3.32 ± 0.23 Jy km/s", the error to two significant figures and the flux to
/// the same decimal place; "3.32 Jy km/s" without an error.
inline QString formatFlux(const RegionFlux &f)
{
    if (!f.valid)
        return QString();
    if (!std::isfinite(f.error) || f.error <= 0.0)
        return QStringLiteral("%1 %2").arg(f.flux, 0, 'g', 6).arg(f.unit);
    const int decimals = std::max(0, 1 - int(std::floor(std::log10(f.error))));
    return QStringLiteral("%1 ± %2 %3")
            .arg(f.flux, 0, 'f', decimals)
            .arg(f.error, 0, 'f', decimals)
            .arg(f.unit);
}

/// "(4.89 ± 0.34) × 10^8 M☉", or "4.89 × 10^8 M☉" without an error (NaN).
/// A mass at or below zero (a flux under its background) is written as is.
inline QString formatHiMass(double mass, double error)
{
    if (!std::isfinite(mass))
        return QString();
    const QString sun = QStringLiteral(" M☉");
    if (!(mass > 0.0))
        return QStringLiteral("%1").arg(mass, 0, 'g', 3) + sun;
    const int e = int(std::floor(std::log10(mass)));
    const double scale = std::pow(10.0, e);
    if (!std::isfinite(error) || error < 0.0)
        return QStringLiteral("%1 × 10^%2").arg(mass / scale, 0, 'f', 2).arg(e) + sun;
    return QStringLiteral("(%1 ± %2) × 10^%3")
                   .arg(mass / scale, 0, 'f', 2)
                   .arg(error / scale, 0, 'f', 2)
                   .arg(e)
            + sun;
}

} // namespace visivo::flux

#endif // REGION_FLUX_H

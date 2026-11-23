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
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace visivo::flux {

/// What a map's pixel values are, as far as the sum over a region goes.
enum class UnitKind {
    Unknown,        ///< no unit, or one not read here: nothing to build a flux on
    PerBeam,        ///< Jy/beam: Σ ÷ beam area is a flux
    PerPixel,       ///< Jy/pixel: Σ is a flux
    PerSteradian,   ///< Jy/sr, MJy/sr: Σ × pixel solid angle is a flux
    Temperature,    ///< K (T_A*, T_mb, T_b): not converted to Jy (η_mb, ν unknown)
};

/// What the pixel sum of a map in @p bunit is.
struct FluxUnit
{
    UnitKind kind{ UnitKind::Unknown };
    bool perBeam{ false };    ///< the brightness is Jy (or mJy, µJy) per beam
    double scale{ 1.0 };      ///< from the map's unit to `label` (mJy → Jy, m/s → km/s)
    bool velocity{ false };   ///< the map is integrated over a velocity: Jy km/s, K km/s
    QString spectral;         ///< the spectral part as given ("m/s", "Hz"), empty for none
    QString label;            ///< "Jy", "Jy km/s", "Jy Hz", "K", "K km/s"
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

/// "/word", "word-1", "word^-1", "word**-1", case-insensitive.
inline QRegularExpression perUnit(const QString &words)
{
    return QRegularExpression(
            QStringLiteral("(?:/\\s*(?:%1)(?:\\s*-\\s*1)?|(?:%1)\\s*(?:\\^|\\*\\*)?\\s*-\\s*1)"
                           "(?![A-Za-z])")
                    .arg(words),
            QRegularExpression::CaseInsensitiveOption);
}

/// The spectral unit left after the brightness, into @p out.
inline void readSpectralPart(QString rest, const QString &base, FluxUnit &out)
{
    // The separators left around it: "Jy/beam.km/s", "Jy km/s/beam".
    static const QRegularExpression edges(QStringLiteral("^[\\s.*/]+|[\\s.*/]+$"));
    rest.remove(edges);
    out.spectral = rest;
    if (rest.isEmpty()) {
        out.label = base;
        return;
    }
    const double kms = velocityToKms(rest);
    if (kms > 0.0) {
        out.velocity = true;
        out.scale *= kms;
        out.label = base + QStringLiteral(" km/s");
        return;
    }
    // A frequency stays in its own unit (Jy Hz) unless withRestFrequency()
    // converts it. Anything else ("K km/s/beam", "K (T_A*)") is not a unit
    // read here, and a label built from it would name nothing.
    out.frequencyToHz = frequencyToHz(rest);
    if (!(out.frequencyToHz > 0.0)) {
        out.kind = UnitKind::Unknown;
        out.perBeam = false;
        return;
    }
    out.label = base + QLatin1Char(' ') + rest;
}

} // namespace detail

/// Read a brightness unit: "Jy/beam", "JY/BEAM", "Jy beam-1", "mJy/beam",
/// "Jy/pixel", "MJy/sr", "K", and a moment-0 unit, the brightness and the
/// spectral unit in any order: "Jy/beam km/s", "JY/BEAM m.s**-1",
/// "Jy/beam.km/s" (CASA), "Jy km s-1 beam-1", "Jy km/s/beam", "K km/s". The
/// prefix is case-sensitive as in the FITS standard: mJy is milli, MJy mega.
/// Anything else, an empty unit included, is UnitKind::Unknown.
inline FluxUnit parseFluxUnit(const QString &bunit)
{
    FluxUnit out;
    static const QRegularExpression kelvin(QStringLiteral("^\\s*(m?)K(?![A-Za-z])"));
    if (const auto mk = kelvin.match(bunit); mk.hasMatch()) {
        out.kind = UnitKind::Temperature;
        out.scale = mk.captured(1).isEmpty() ? 1.0 : 1e-3;
        detail::readSpectralPart(bunit.mid(mk.capturedEnd(0)), QStringLiteral("K"), out);
        return out;
    }
    // The brightness: Jy with an optional, case-sensitive prefix, first.
    static const QRegularExpression jy(QStringLiteral("^\\s*([muµμMk]?)[Jj][Yy](?![A-Za-z])"));
    const auto mj = jy.match(bunit);
    if (!mj.hasMatch())
        return out;
    static const QRegularExpression beam = detail::perUnit(QStringLiteral("beam"));
    static const QRegularExpression pixel = detail::perUnit(QStringLiteral("pixel|pix"));
    static const QRegularExpression steradian = detail::perUnit(QStringLiteral("sr"));
    QString rest = bunit.mid(mj.capturedEnd(0));
    QRegularExpressionMatch per = beam.match(rest);
    if (per.hasMatch()) {
        out.kind = UnitKind::PerBeam;
    } else if ((per = pixel.match(rest)).hasMatch()) {
        out.kind = UnitKind::PerPixel;
    } else if ((per = steradian.match(rest)).hasMatch()) {
        out.kind = UnitKind::PerSteradian;
    } else {
        return out;
    }
    rest.remove(per.capturedStart(0), per.capturedLength(0));
    out.perBeam = out.kind == UnitKind::PerBeam;
    const QString prefix = mj.captured(1);
    out.scale = prefix.isEmpty()                 ? 1.0
            : prefix == QLatin1String("m")      ? 1e-3
            : prefix == QLatin1String("M")      ? 1e6
            : prefix == QLatin1String("k")      ? 1e3
                                                : 1e-6;   // u, µ
    detail::readSpectralPart(rest, QStringLiteral("Jy"), out);
    return out;
}

/// A frequency-integrated map (Jy/beam Hz) in Jy km/s through the rest
/// frequency: Δv = c·Δν/ν₀, as the backend's HI products convert it
/// (`frequency_width_to_kms`). Unchanged when the map is not integrated over
/// a frequency or @p restHz is not positive.
inline FluxUnit withRestFrequency(FluxUnit u, double restHz)
{
    if (u.kind == UnitKind::Unknown || !(u.frequencyToHz > 0.0) || !(restHz > 0.0)
        || !std::isfinite(restHz))
        return u;
    u.scale *= u.frequencyToHz * Catalogue3DParser::detail::kSpeedOfLightKms / restHz;
    u.velocity = true;
    u.fromFrequency = true;
    u.label = u.kind == UnitKind::Temperature ? QStringLiteral("K km/s")
                                              : QStringLiteral("Jy km/s");
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

/// The integral of @p n valid pixels summing to @p sum on a map in @p unit.
/// @p beamAreaPx is the pixels per beam (0 when there is no beam),
/// @p pixelAreaDeg2 a pixel's solid angle |CDELT1·CDELT2| in deg², and
/// @p pixelsPerSample the full-resolution pixels each measured pixel stands
/// for (1 at full resolution, the block area on a reduced map).
///
/// The sum is weighted per pixel by what the unit needs (issue #416):
///   Jy/beam   S = (Σ − N·mean)·A / Ω                    Jy
///   Jy/pixel  S = (Σ − N·mean)·A                        Jy
///   Jy/sr     S = (Σ − N·mean)·A·Ω_pix[sr]              Jy
///   K         Σ W ΔΩ = (Σ − N·mean)·A·Ω_pix[arcsec²]    K arcsec² (K km/s arcsec²)
/// A temperature is not turned into Jy: that needs η_mb for T_A* and the
/// frequency, and a wrong η_mb is a silent factor of two.
///
/// The error counts the independent samples: σ²·N·min(N, Ω) per-pixel
/// weights squared for the region (N/Ω beams when the region is larger than a
/// beam, one beam when smaller), plus, with a background, its mean's error
/// σ²·min(N_bg, Ω)/N_bg subtracted N times. Without a beam the pixels count
/// as independent. A σ that is not positive (a clipped map) gives no error.
inline RegionFlux regionFlux(double sum, long n, double beamAreaPx, const FluxUnit &unit,
                             const Noise &noise, double pixelsPerSample = 1.0,
                             double pixelAreaDeg2 = 0.0)
{
    RegionFlux out;
    if (n <= 0 || !std::isfinite(sum) || !(pixelsPerSample > 0.0))
        return out;
    const bool haveBeam = beamAreaPx > 0.0 && std::isfinite(beamAreaPx);
    const bool havePixel = pixelAreaDeg2 > 0.0 && std::isfinite(pixelAreaDeg2);
    constexpr double kDegToRad = regions::kPi / 180.0;
    double weight = 0.0;
    QString label = unit.label;
    switch (unit.kind) {
    case UnitKind::PerBeam:
        if (!haveBeam)
            return out;
        weight = unit.scale / beamAreaPx;
        break;
    case UnitKind::PerPixel:
        weight = unit.scale;
        break;
    case UnitKind::PerSteradian:
        if (!havePixel)
            return out;
        weight = unit.scale * pixelAreaDeg2 * kDegToRad * kDegToRad;
        break;
    case UnitKind::Temperature:
        if (!havePixel)
            return out;
        weight = unit.scale * pixelAreaDeg2 * 3600.0 * 3600.0;
        label = unit.velocity ? QStringLiteral("K km/s arcsec²")
                              : unit.label + QStringLiteral(" arcsec²");
        break;
    case UnitKind::Unknown:
        return out;
    }
    const double a = pixelsPerSample;
    const double mean = noise.subtract ? noise.mean : 0.0;
    out.flux = (sum - double(n) * mean) * a * weight;
    if (std::isfinite(noise.sigma) && noise.sigma > 0.0) {
        const double nFull = double(n) * a;
        const double samples = haveBeam ? nFull * std::min(nFull, beamAreaPx) : nFull;
        double var = samples;
        if (noise.subtract && noise.count > 0) {
            const double bgFull = double(noise.count) * a;
            const double bgCorrelation = haveBeam ? std::min(bgFull, beamAreaPx) : 1.0;
            var += nFull * nFull * bgCorrelation / bgFull;
        }
        out.error = noise.sigma * std::sqrt(var) * std::abs(weight);
    }
    out.unit = label;
    out.valid = std::isfinite(out.flux);
    return out;
}

/// The HI rest frequency, Hz.
constexpr double kHiRestHz = 1420405751.768;

/// Whether an HI mass may be computed from a flux in @p unit: a Jy flux
/// integrated over velocity, of the 21-cm line. A cube whose rest frequency
/// is another line's (CO, say) gets none; without a rest frequency the mass
/// is given with @p note set, since then the line is assumed.
inline bool hiMassApplies(const FluxUnit &unit, double restHz, QString *note = nullptr)
{
    const bool jansky = unit.kind == UnitKind::PerBeam || unit.kind == UnitKind::PerPixel
            || unit.kind == UnitKind::PerSteradian;
    if (!jansky || !unit.velocity)
        return false;
    if (restHz > 0.0 && std::isfinite(restHz))
        return std::abs(restHz - kHiRestHz) < 1e6;
    if (note)
        *note = QStringLiteral("assumes the HI 21-cm line (no rest frequency)");
    return true;
}

/// Why a region of a map in @p unit has no flux, or an empty string when it
/// can have one with what is known. @p rawUnit is the unit as shown.
inline QString fluxRefusal(const FluxUnit &unit, const QString &rawUnit, double beamAreaPx,
                           double pixelAreaDeg2)
{
    switch (unit.kind) {
    case UnitKind::Unknown:
        return rawUnit.trimmed().isEmpty() || rawUnit.contains(QStringLiteral("unknown"))
                ? QStringLiteral("no flux: the intensity unit is unknown (no BUNIT)")
                : QStringLiteral("no flux: the unit %1 is not one a flux can be computed from")
                          .arg(rawUnit.trimmed());
    case UnitKind::PerBeam:
        if (beamAreaPx > 0.0)
            return QString();
        return pixelAreaDeg2 > 0.0
                ? QStringLiteral("no flux: the map is per beam and has no beam (BMAJ/BMIN); "
                                 "set one in Inspector ▸ Beam")
                : QStringLiteral("no flux: the pixel size is unknown, so the beam area "
                                 "cannot be computed");
    case UnitKind::PerPixel:
        return QString();
    case UnitKind::PerSteradian:
    case UnitKind::Temperature:
        return pixelAreaDeg2 > 0.0 ? QString()
                                   : QStringLiteral("no flux: the pixel size is unknown");
    }
    return QString();
}

/// The caveat that goes with a measurement in @p unit; empty when none.
inline QString fluxCaveat(const FluxUnit &unit, bool haveBeam)
{
    QStringList parts;
    if (unit.kind == UnitKind::Temperature)
        parts << QStringLiteral("a temperature is not converted to Jy: T_A* is not corrected "
                                "for the main-beam efficiency (T_mb = T_A*/η_mb), and no flux "
                                "density is given");
    if (unit.kind == UnitKind::PerSteradian)
        parts << QStringLiteral("MJy/sr maps such as SPIRE's are calibrated for extended "
                                "sources; no point-source correction is applied");
    if (unit.kind != UnitKind::PerBeam && !haveBeam)
        parts << QStringLiteral("the error assumes independent pixels (no beam): a lower limit "
                                "on a beam-smoothed map");
    return parts.join(QStringLiteral("; "));
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

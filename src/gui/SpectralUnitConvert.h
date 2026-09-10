#ifndef SPECTRAL_UNIT_CONVERT_H
#define SPECTRAL_UNIT_CONVERT_H

#include <QLatin1Char>
#include <QString>

#include <cmath>

/// Spectral-axis unit parsing + same-quantity value conversion.
///
/// Extracted verbatim from the anonymous namespace of vtkWindowCube_Link.cpp
/// (E-04 god-object decomposition, a safe shared-helper lift): the linked-channel
/// sync matches peers by physical velocity/frequency, which needs these small but
/// subtle pure functions (km/s↔m/s conversion; the "mhz must not look like a
/// velocity" disambiguation; the astropy-normalised `m.s**-1` spelling). They had
/// no unit tests where they lived; pulling them into a header locks that behaviour
/// under `tests/test_spectral_unit_convert.cpp`. Pure, GUI/VTK-free.
namespace spectral {

/// A parsed spectral unit. quantity: 1 = velocity (SI m/s), 2 = frequency (SI Hz);
/// ok=false for an unrecognised unit. siFactor converts a value in this unit to SI.
struct SpectralUnit
{
    int quantity{ 0 };
    double siFactor{ 1.0 };
    bool ok{ false };
};

/// Parse a spectral-axis unit string. Handles the header and astropy-normalised
/// spellings (km/s, m/s, m.s**-1, Hz/kHz/MHz/GHz); whitespace/`.`/`/` and `**`
/// are stripped before matching, and frequency is checked before velocity so
/// "mhz" is never mistaken for a velocity.
inline SpectralUnit parseSpectralUnit(QString u)
{
    u = u.trimmed().toLower();
    u.remove(QLatin1Char(' '));
    u.remove(QLatin1Char('.'));
    u.replace(QStringLiteral("**"), QString());
    u.remove(QLatin1Char('/'));
    // frequency (check before velocity: "mhz" must not look like a velocity)
    if (u == QStringLiteral("hz"))  return { 2, 1.0, true };
    if (u == QStringLiteral("khz")) return { 2, 1.0e3, true };
    if (u == QStringLiteral("mhz")) return { 2, 1.0e6, true };
    if (u == QStringLiteral("ghz")) return { 2, 1.0e9, true };
    // velocity: km/s→"kms", m/s→"ms", m.s**-1→"ms-1", km.s**-1→"kms-1".
    // Exact normalised matches (not startsWith) so an unrelated "ms…" string is
    // not mistaken for a velocity.
    if (u == QStringLiteral("kms") || u == QStringLiteral("kms-1")) return { 1, 1.0e3, true };
    if (u == QStringLiteral("ms") || u == QStringLiteral("ms-1"))   return { 1, 1.0, true };
    return {};
}

/// Convert a spectral value between two units of the SAME physical quantity.
/// Returns false (no conversion) if either unit is unknown or they differ in
/// quantity (e.g. frequency vs velocity, which would need a rest frequency).
inline bool convertSpectralValue(double value, const QString &fromUnit, const QString &toUnit,
                                 double &out)
{
    const SpectralUnit f = parseSpectralUnit(fromUnit);
    const SpectralUnit t = parseSpectralUnit(toUnit);
    if (!f.ok || !t.ok || f.quantity != t.quantity) {
        return false;
    }
    out = value * f.siFactor / t.siFactor;
    return true;
}

/// ── Cross-quantity: placing a rest-frame line on an observed axis ───────────
///
/// A spectral-line list carries REST frequencies. To draw one on a spectrum two
/// things have to happen, and skipping either puts the marker in the wrong place
/// (or off-plot entirely, which is how the first version of the line overlay
/// behaved): the line must be redshifted to the observed frame of the source, and
/// the observed frequency must be expressed in the axis's own quantity and
/// velocity convention. The convention is not a detail — optical and radio
/// velocities of the same line differ by ~v²/c, which at 1000 km/s is already
/// 3 km/s, several channels of an HI cube.
constexpr double kCLightMs = 299792458.0;

/// Spectral-axis flavour, from the FITS CTYPE of the spectral axis.
enum class AxisKind { Unknown, Frequency, VelocityOptical, VelocityRadio, VelocityRelativistic };

/// Classify a spectral CTYPE ("FREQ", "VOPT", "VRAD", "VELO", "FELO-HEL", …).
/// The trailing frame code (-LSR, -HEL, -BAR) does not change the arithmetic
/// here, so it is ignored; an unknown or non-spectral CTYPE gives Unknown.
inline AxisKind axisKindFromCtype(QString ctype)
{
    ctype = ctype.trimmed().toUpper();
    const int dash = ctype.indexOf(QLatin1Char('-'));
    const QString base = dash > 0 ? ctype.left(dash) : ctype;
    if (base == QStringLiteral("FREQ"))
        return AxisKind::Frequency;
    if (base == QStringLiteral("VOPT") || base == QStringLiteral("FELO"))
        return AxisKind::VelocityOptical;
    if (base == QStringLiteral("VRAD"))
        return AxisKind::VelocityRadio;
    // "VELO" alone is the apparent radial velocity; the FITS standard reads it as
    // relativistic, and AIPS-era files use it for optical. Relativistic is the
    // standard's meaning, and the difference only shows in the third digit.
    if (base == QStringLiteral("VELO"))
        return AxisKind::VelocityRelativistic;
    return AxisKind::Unknown;
}

/// Observed frequency of a line of rest frequency @p restHz at redshift @p z.
inline double observedFrequencyHz(double restHz, double z)
{
    return (1.0 + z) > 0.0 ? restHz / (1.0 + z) : 0.0;
}

/// Redshift equivalent of an optical systemic velocity in m/s (z = v/c). The
/// optical convention is what source catalogues quote, which is why it is the
/// one exposed for user input.
inline double redshiftFromOpticalVelocity(double vMs) { return vMs / kCLightMs; }

/// Where an observed frequency @p fObsHz lands on an axis of @p kind whose own
/// rest frequency is @p axisRestHz — in SI (Hz for a frequency axis, m/s for a
/// velocity one). False when the axis kind is unknown, or when a velocity axis
/// has no rest frequency to convert against (no RESTFRQ in the header).
inline bool axisValueForFrequency(double fObsHz, AxisKind kind, double axisRestHz, double &outSi)
{
    if (!(fObsHz > 0.0))
        return false;
    if (kind == AxisKind::Frequency) {
        outSi = fObsHz;
        return true;
    }
    if (!(axisRestHz > 0.0))
        return false;
    const double r = axisRestHz;
    switch (kind) {
    case AxisKind::VelocityOptical:
        outSi = kCLightMs * (r / fObsHz - 1.0);
        return true;
    case AxisKind::VelocityRadio:
        outSi = kCLightMs * (1.0 - fObsHz / r);
        return true;
    case AxisKind::VelocityRelativistic:
        outSi = kCLightMs * (r * r - fObsHz * fObsHz) / (r * r + fObsHz * fObsHz);
        return true;
    default:
        return false;
    }
}

/// The inverse of axisValueForFrequency: the frequency an axis coordinate
/// (SI: Hz on a frequency axis, m/s on a velocity one) corresponds to. Needed
/// to shift a list given in VELOCITY: a redshift acts on frequency, so the
/// value has to be taken back to a frequency in the axis's own convention,
/// redshifted there, and converted forward again. Adding `z·c` to a velocity
/// instead is only right for the optical convention, and wrong by ~v²/c on a
/// radio or relativistic axis.
inline bool frequencyForAxisValue(double axisSi, AxisKind kind, double axisRestHz, double &outHz)
{
    if (kind == AxisKind::Frequency) {
        if (!(axisSi > 0.0))
            return false;
        outHz = axisSi;
        return true;
    }
    if (!(axisRestHz > 0.0))
        return false;
    const double r = axisRestHz;
    const double beta = axisSi / kCLightMs;
    switch (kind) {
    case AxisKind::VelocityOptical:
        if (!(1.0 + beta > 0.0))
            return false;
        outHz = r / (1.0 + beta);
        return true;
    case AxisKind::VelocityRadio:
        outHz = r * (1.0 - beta);
        return outHz > 0.0;
    case AxisKind::VelocityRelativistic:
        if (!(1.0 + beta > 0.0) || !(1.0 - beta > 0.0))
            return false;
        outHz = r * std::sqrt((1.0 - beta) / (1.0 + beta));
        return true;
    default:
        return false;
    }
}

}  // namespace spectral

#endif  // SPECTRAL_UNIT_CONVERT_H

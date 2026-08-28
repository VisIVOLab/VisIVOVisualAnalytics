#ifndef SPECTRAL_UNIT_CONVERT_H
#define SPECTRAL_UNIT_CONVERT_H

#include <QLatin1Char>
#include <QString>

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

}  // namespace spectral

#endif  // SPECTRAL_UNIT_CONVERT_H

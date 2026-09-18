#ifndef VlkbPosString_h
#define VlkbPosString_h

// VlkbPosString.h
// Build the POS string a VLKB SIAv2 / SODA request is shaped around.
//
// Header-only and free of widgets because the arithmetic is where this went
// wrong and stayed wrong: a negative galactic longitude was normalised by
// adding **180**, not 360. l = −2° became 178° instead of 358°, so a query
// anywhere near l = 0 — the Galactic centre, the most observed longitude there
// is — silently returned datasets from the opposite side of the Galaxy, and a
// box straddling l = 0 came back with its edges swapped around.

#include <QString>

#include <algorithm>
#include <cmath>

namespace visivo {

/// Galactic longitude folded into [0, 360).
///
/// Longitude is periodic with 360°, which is the whole of the fix: −2° is 358°,
/// 362° is 2°. std::fmod alone keeps the sign of the dividend, so a negative
/// input needs the extra turn.
inline double normalisedGalacticLongitude(double longitudeDeg)
{
    if (!std::isfinite(longitudeDeg))
        return 0.0;
    double wrapped = std::fmod(longitudeDeg, 360.0);
    if (wrapped < 0.0)
        wrapped += 360.0;
    return wrapped;
}

/// Latitude clamped to [-90, 90]. Unlike longitude it does not wrap: a query
/// past the pole is a typo, and folding it would quietly move the region.
inline double clampedGalacticLatitude(double latitudeDeg)
{
    if (!std::isfinite(latitudeDeg))
        return 0.0;
    return std::clamp(latitudeDeg, -90.0, 90.0);
}

/// ``CIRCLE l b r`` for a cone search, in degrees.
inline QString circlePosString(double longitudeDeg, double latitudeDeg, double radiusDeg)
{
    return QStringLiteral("CIRCLE %1 %2 %3")
            .arg(normalisedGalacticLongitude(longitudeDeg), 0, 'f', 6)
            .arg(clampedGalacticLatitude(latitudeDeg), 0, 'f', 6)
            .arg(std::abs(radiusDeg), 0, 'f', 6);
}

/// ``RANGE l1 l2 b1 b2`` for a box of half-widths @p halfWidthLonDeg /
/// @p halfWidthLatDeg around (@p longitudeDeg, @p latitudeDeg).
///
/// A box that straddles l = 0 is emitted as it stands (l1 > l2), which is how
/// SIAv2 RANGE expresses a wrap: normalising each edge independently would give
/// l1 = 358, l2 = 2 and, read as an ordinary interval, select the 356° of sky
/// the user did *not* ask for.
inline QString rangePosString(double longitudeDeg, double latitudeDeg,
                              double halfWidthLonDeg, double halfWidthLatDeg)
{
    // A box 360° wide covers every longitude. Normalising its two edges
    // independently gives them the same value — RANGE 0 0 — which reads as a
    // box of zero width, the exact opposite of what was asked for.
    if (std::abs(halfWidthLonDeg) * 2.0 >= 360.0) {
        return QStringLiteral("RANGE 0.000000 360.000000 %1 %2")
                .arg(clampedGalacticLatitude(latitudeDeg - std::abs(halfWidthLatDeg)), 0, 'f', 6)
                .arg(clampedGalacticLatitude(latitudeDeg + std::abs(halfWidthLatDeg)), 0, 'f', 6);
    }
    const double lonMin = normalisedGalacticLongitude(longitudeDeg - std::abs(halfWidthLonDeg));
    const double lonMax = normalisedGalacticLongitude(longitudeDeg + std::abs(halfWidthLonDeg));
    const double latMin = clampedGalacticLatitude(latitudeDeg - std::abs(halfWidthLatDeg));
    const double latMax = clampedGalacticLatitude(latitudeDeg + std::abs(halfWidthLatDeg));
    return QStringLiteral("RANGE %1 %2 %3 %4")
            .arg(lonMin, 0, 'f', 6)
            .arg(lonMax, 0, 'f', 6)
            .arg(latMin, 0, 'f', 6)
            .arg(latMax, 0, 'f', 6);
}

/// Whether a RANGE string produced above wraps through l = 0. The caller shows
/// this to the user: a wrapped box is legitimate but unusual, and a service
/// that does not honour the convention returns nothing rather than an error.
inline bool rangeWrapsThroughZero(double longitudeDeg, double halfWidthLonDeg)
{
    if (std::abs(halfWidthLonDeg) * 2.0 >= 360.0)
        return false;   // the whole circle is emitted as 0 → 360, which does not wrap
    const double lonMin = normalisedGalacticLongitude(longitudeDeg - std::abs(halfWidthLonDeg));
    const double lonMax = normalisedGalacticLongitude(longitudeDeg + std::abs(halfWidthLonDeg));
    return lonMin > lonMax;
}

}   // namespace visivo

#endif   // VlkbPosString_h

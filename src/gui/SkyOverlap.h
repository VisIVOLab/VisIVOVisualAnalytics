#ifndef SkyOverlap_h
#define SkyOverlap_h

// SkyOverlap.h
// Do two images cover any of the same sky?
//
// A layer is only a layer if it lands on top of the image it is added to. When
// it does not, the viewer used to accept it anyway: the WCS alignment puts it
// at its true position, which is off the edge of the master, so the load
// "succeeds" and nothing appears. The user is left looking for a layer that is
// there, correctly placed, and invisible.
//
// The rule lives here — no VTK, no FITS, no widgets — so it can be tested
// against the cases that actually break it: a field straddling RA = 0, where
// the bounds come back as ra_min > ra_max and a naive interval test reports no
// overlap for two images that share the same sky.

#include <algorithm>
#include <cmath>

namespace SkyOverlap {

/// Sky bounds of an image, in degrees, as `wcsrange()` reports them: `raMin`
/// may exceed `raMax`, which means the range wraps through RA = 0.
struct Bounds
{
    double raMin{ 0. };
    double raMax{ 0. };
    double decMin{ 0. };
    double decMax{ 0. };
};

inline bool isFinite(const Bounds &b)
{
    return std::isfinite(b.raMin) && std::isfinite(b.raMax) && std::isfinite(b.decMin)
            && std::isfinite(b.decMax);
}

/// Whether two closed intervals share more than a single point.
inline bool intervalsOverlap(double aMin, double aMax, double bMin, double bMax)
{
    return std::max(aMin, bMin) < std::min(aMax, bMax);
}

/// Whether the two declination ranges overlap. Declination never wraps.
inline bool decOverlap(const Bounds &a, const Bounds &b)
{
    return intervalsOverlap(std::min(a.decMin, a.decMax), std::max(a.decMin, a.decMax),
                            std::min(b.decMin, b.decMax), std::max(b.decMin, b.decMax));
}

/// Whether the two right-ascension ranges overlap, including through RA = 0.
/// A wrapping range is two intervals — [raMin, 360) and [0, raMax] — and it
/// overlaps if either half does.
inline bool raOverlap(const Bounds &a, const Bounds &b)
{
    const auto wraps = [](const Bounds &x) { return x.raMin > x.raMax; };
    if (!wraps(a) && !wraps(b))
        return intervalsOverlap(a.raMin, a.raMax, b.raMin, b.raMax);
    if (wraps(a) && wraps(b))
        // Both cross RA = 0, so both contain it.
        return true;
    const Bounds &wrapped = wraps(a) ? a : b;
    const Bounds &plain = wraps(a) ? b : a;
    return intervalsOverlap(wrapped.raMin, 360., plain.raMin, plain.raMax)
            || intervalsOverlap(0., wrapped.raMax, plain.raMin, plain.raMax);
}

/// Whether the two images cover any common sky.
///
/// Bounds that are not finite — an image with no celestial solution, a
/// simulation in pixel coordinates — answer `false`: nothing can be said about
/// where they are, and a layer placed by guesswork is worse than one refused.
inline bool overlaps(const Bounds &a, const Bounds &b)
{
    if (!isFinite(a) || !isFinite(b))
        return false;
    return decOverlap(a, b) && raOverlap(a, b);
}

}   // namespace SkyOverlap

#endif   // SkyOverlap_h

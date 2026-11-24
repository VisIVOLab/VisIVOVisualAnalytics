#ifndef SkyWcs_h
#define SkyWcs_h

// SkyWcs.h
// The celestial WCS of a 2-D plane, and the rules that match two of them
// (issue #522): an image and a cube link in sky coordinates, as CARTA's "XY
// matching" does, so a pan, a zoom, a cursor, a region or a PV path made in
// one lands on the same sky in the other, whatever each one's projection,
// pixel scale, rotation or axis direction.
//
// The windows keep only CRVAL/CRPIX/CDELT of their dataset, which cannot say
// where a pixel is on a rotated or a non-linear solution. This is built from
// the FITS header cards the backend serves (`/v1/files/header`) and does the
// whole transform of FITS Paper II (intermediate coordinates, projection,
// spherical rotation) for the projections radio and survey images come in:
// TAN, SIN, ARC, STG and CAR. Equatorial and Galactic axes are both read, so
// a Hi-GAL map in Galactic CAR links to an ALMA cube in ICRS TAN.
//
// Pixels are 0-based with centres on integers: the frame the region list and
// both viewers' worlds use.
//
// Pure QtCore + std, so it is unit-tested without a window or a FITS file
// (tests/test_sky_wcs.cpp).

#include "FitsHeaderCard.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace visivo::skylink {

/// A direction on the sky: a unit vector in ICRS/J2000 equatorial axes.
using Vec3 = std::array<double, 3>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

inline double dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline Vec3 cross(const Vec3 &a, const Vec3 &b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

/// The angle between two directions, in degrees. atan2 of the cross and dot
/// products rather than acos of the dot: a pixel is arcseconds, where acos
/// of a number within 1e-11 of 1 is noise.
inline double separationDeg(const Vec3 &a, const Vec3 &b)
{
    const Vec3 c = cross(a, b);
    return std::atan2(std::sqrt(dot(c, c)), dot(a, b)) / kDeg;
}

class SkyWcs
{
public:
    enum class Projection { None, Tan, Sin, Arc, Stg, Car };
    enum class Frame { Equatorial, Galactic };

    /// Read the celestial solution of axes 1 and 2 from raw 80-column cards.
    /// Not valid() when the header has none this class can use (a pixel-only
    /// simulation, an unsupported projection, a singular matrix, FK4 axes).
    static SkyWcs fromCards(const QStringList &cards)
    {
        QHash<QString, QString> k;
        for (const QString &raw : cards) {
            const FitsHeaderCard card = parseFitsHeaderCard(raw);
            if (!card.commentary && !k.contains(card.key))
                k.insert(card.key, card.value);
        }
        return fromKeys(k);
    }

    static SkyWcs fromKeys(const QHash<QString, QString> &k)
    {
        SkyWcs w;
        const auto num = [&k](const char *key, double fallback, bool *have = nullptr) {
            bool ok = false;
            const double v = k.value(QString::fromLatin1(key)).trimmed().replace(u'D', u'E').toDouble(&ok);
            if (have)
                *have = ok;
            return ok ? v : fallback;
        };
        const QString ctype1 = k.value(QStringLiteral("CTYPE1")).trimmed().toUpper();
        const QString ctype2 = k.value(QStringLiteral("CTYPE2")).trimmed().toUpper();
        if (ctype1.size() < 8 || ctype2.size() < 8)
            return w;
        const QString lon = ctype1.left(4), lat = ctype2.left(4);
        if (lon == QLatin1String("RA--") && lat == QLatin1String("DEC-")) {
            w.m_frame = Frame::Equatorial;
        } else if (lon == QLatin1String("GLON") && lat == QLatin1String("GLAT")) {
            w.m_frame = Frame::Galactic;
        } else {
            return w;
        }
        const QString code = ctype1.mid(5, 3);
        if (code != ctype2.mid(5, 3))
            return w;
        Projection proj = Projection::None;
        if (code == QLatin1String("TAN"))
            proj = Projection::Tan;
        else if (code == QLatin1String("SIN"))
            proj = Projection::Sin;
        else if (code == QLatin1String("ARC"))
            proj = Projection::Arc;
        else if (code == QLatin1String("STG"))
            proj = Projection::Stg;
        else if (code == QLatin1String("CAR"))
            proj = Projection::Car;
        if (proj == Projection::None)
            return w;
        // The slanted SIN (non-zero PV2_1 or PV2_2, used for aperture synthesis
        // with a non-coplanar array) is not the plain orthographic projection.
        if (proj == Projection::Sin && (num("PV2_1", 0.0) != 0.0 || num("PV2_2", 0.0) != 0.0))
            return w;
        // FK4 (B1950) axes are not J2000: refusing them is better than a
        // half-degree error that looks like a bug in the link.
        if (w.m_frame == Frame::Equatorial) {
            const QString sys = k.value(QStringLiteral("RADESYS")).trimmed().toUpper();
            bool haveEquinox = false;
            const double equinox = num("EQUINOX", 2000.0, &haveEquinox);
            if (sys.startsWith(QLatin1String("FK4")) || (sys.isEmpty() && haveEquinox && equinox < 1984.0))
                return w;
        }

        bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
        w.m_crpix = { num("CRPIX1", 0.0, &ok1), num("CRPIX2", 0.0, &ok2) };
        w.m_crval = { num("CRVAL1", 0.0, &ok3), num("CRVAL2", 0.0, &ok4) };
        if (!ok1 || !ok2 || !ok3 || !ok4)
            return w;

        // The linear part: CDi_j when there is one, else CDELT x PC, else the
        // old CROTA2 convention. All give degrees per pixel.
        bool haveCd = false;
        std::array<double, 4> cd{ 0.0, 0.0, 0.0, 0.0 };
        const char *cdKeys[4] = { "CD1_1", "CD1_2", "CD2_1", "CD2_2" };
        for (int i = 0; i < 4; ++i) {
            bool have = false;
            cd[i] = num(cdKeys[i], 0.0, &have);
            haveCd = haveCd || have;
        }
        if (!haveCd) {
            const double d1 = num("CDELT1", 1.0), d2 = num("CDELT2", 1.0);
            const bool havePc = k.contains(QStringLiteral("PC1_1")) || k.contains(QStringLiteral("PC1_2"))
                    || k.contains(QStringLiteral("PC2_1")) || k.contains(QStringLiteral("PC2_2"));
            if (havePc) {
                cd = { d1 * num("PC1_1", 1.0), d1 * num("PC1_2", 0.0), d2 * num("PC2_1", 0.0),
                       d2 * num("PC2_2", 1.0) };
            } else {
                const double rot = num("CROTA2", 0.0) * kDeg;
                cd = { d1 * std::cos(rot), -d2 * std::sin(rot), d1 * std::sin(rot), d2 * std::cos(rot) };
            }
        }
        const double det = cd[0] * cd[3] - cd[1] * cd[2];
        if (!std::isfinite(det) || std::abs(det) < 1e-30)
            return w;
        w.m_cd = cd;
        w.m_inv = { cd[3] / det, -cd[1] / det, -cd[2] / det, cd[0] / det };
        w.m_proj = proj;

        // The native pole of the spherical rotation (Paper II, section 2.4).
        // A zenithal projection has its reference point at the native pole.
        // A cylindrical one at (0, 0): its pole follows from the reference
        // latitude, and LONPOLE takes the value the standard gives it.
        const double a0 = w.m_crval[0] * kDeg, d0 = w.m_crval[1] * kDeg;
        if (proj == Projection::Car) {
            w.m_phiP = d0 >= 0.0 ? 0.0 : kPi;
            w.m_deltaP = kPi / 2.0 - std::abs(d0);
            // The reference point is native (0, 0): its longitude gives alpha_p.
            const double cp = d0 >= 0.0 ? 1.0 : -1.0;   // cos(phi_p)
            w.m_alphaP = a0 - std::atan2(0.0, -std::sin(w.m_deltaP) * cp);
        } else {
            w.m_alphaP = a0;
            w.m_deltaP = d0;
            w.m_phiP = num("LONPOLE", d0 >= kPi / 2.0 ? 0.0 : 180.0) * kDeg;   // Paper II default
        }
        return w;
    }

    bool valid() const { return m_proj != Projection::None; }
    Frame frame() const { return m_frame; }
    Projection projection() const { return m_proj; }

    /// The sky direction of 0-based pixel (px, py). False for a pixel the
    /// projection does not reach (beyond the horizon of a SIN map).
    bool pixelToSky(double px, double py, Vec3 &out) const
    {
        Vec3 own;
        if (!pixelToOwnAxes(px, py, own))
            return false;
        out = toEquatorial(own);
        return true;
    }

    /// The longitude and latitude of pixel (px, py) in the WCS's own axes
    /// (RA/Dec or l/b), in degrees: what the header's CTYPE says.
    bool pixelToLonLat(double px, double py, double &lonDeg, double &latDeg) const
    {
        Vec3 own;
        if (!pixelToOwnAxes(px, py, own))
            return false;
        lonDeg = std::remainder(std::atan2(own[1], own[0]) / kDeg - 180.0, 360.0) + 180.0;
        latDeg = std::asin(std::clamp(own[2], -1.0, 1.0)) / kDeg;
        return true;
    }

    /// The sky direction of 0-based pixel (px, py) as a unit vector in the
    /// WCS's own axes (not yet turned to ICRS).
    bool pixelToOwnAxes(double px, double py, Vec3 &out) const
    {
        if (!valid() || !std::isfinite(px) || !std::isfinite(py))
            return false;
        const double dx = px + 1.0 - m_crpix[0], dy = py + 1.0 - m_crpix[1];
        const double x = m_cd[0] * dx + m_cd[1] * dy;   // degrees
        const double y = m_cd[2] * dx + m_cd[3] * dy;
        double phi = 0.0, theta = 0.0;   // native longitude and latitude, radians
        if (m_proj == Projection::Car) {
            phi = x * kDeg;
            theta = y * kDeg;
            if (std::abs(theta) > kPi / 2.0)
                return false;
        } else {
            const double r = std::hypot(x, y) * kDeg;
            phi = r == 0.0 ? 0.0 : std::atan2(x, -y);
            switch (m_proj) {
            case Projection::Tan: theta = std::atan2(1.0, r); break;
            case Projection::Sin:
                if (r > 1.0)
                    return false;
                theta = std::acos(r);
                break;
            case Projection::Arc: theta = kPi / 2.0 - r; break;
            case Projection::Stg: theta = kPi / 2.0 - 2.0 * std::atan(r / 2.0); break;
            default: return false;
            }
            if (theta < -kPi / 2.0)
                return false;
        }
        // Native to celestial (Paper II, eq. 2).
        const double sd = std::sin(m_deltaP), cd = std::cos(m_deltaP);
        const double st = std::sin(theta), ct = std::cos(theta);
        const double dphi = phi - m_phiP;
        const double sinDelta = st * sd + ct * cd * std::cos(dphi);
        const double alpha = m_alphaP
                + std::atan2(-ct * std::sin(dphi), st * cd - ct * sd * std::cos(dphi));
        const double delta = std::asin(std::clamp(sinDelta, -1.0, 1.0));
        out = unit(alpha, delta);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }

    /// The 0-based pixel under a sky direction. False when the direction is
    /// outside what the projection can draw (the far hemisphere of a TAN map).
    bool skyToPixel(const Vec3 &dir, double &px, double &py) const
    {
        if (!valid())
            return false;
        const Vec3 v = fromEquatorial(dir);
        const double delta = std::asin(std::clamp(v[2], -1.0, 1.0));
        const double alpha = std::atan2(v[1], v[0]);
        // Celestial to native (Paper II, eq. 5).
        const double sd = std::sin(m_deltaP), cd = std::cos(m_deltaP);
        const double sDelta = std::sin(delta), cDelta = std::cos(delta);
        const double da = alpha - m_alphaP;
        const double sinTheta = sDelta * sd + cDelta * cd * std::cos(da);
        const double theta = std::asin(std::clamp(sinTheta, -1.0, 1.0));
        double phi = m_phiP + std::atan2(-cDelta * std::sin(da), sDelta * cd - cDelta * sd * std::cos(da));
        phi = std::remainder(phi, 2.0 * kPi);
        double x = 0.0, y = 0.0;   // degrees
        if (m_proj == Projection::Car) {
            x = phi / kDeg;
            y = theta / kDeg;
        } else {
            double r = 0.0;   // radians
            switch (m_proj) {
            case Projection::Tan:
                if (theta <= 1e-9)
                    return false;
                r = 1.0 / std::tan(theta);
                break;
            case Projection::Sin:
                if (theta < -1e-12)
                    return false;
                r = std::cos(theta);
                break;
            case Projection::Arc: r = kPi / 2.0 - theta; break;
            case Projection::Stg: r = 2.0 * std::tan((kPi / 2.0 - theta) / 2.0); break;
            default: return false;
            }
            x = r / kDeg * std::sin(phi);
            y = -r / kDeg * std::cos(phi);
        }
        const double dx = m_inv[0] * x + m_inv[1] * y;
        const double dy = m_inv[2] * x + m_inv[3] * y;
        px = dx + m_crpix[0] - 1.0;
        py = dy + m_crpix[1] - 1.0;
        return std::isfinite(px) && std::isfinite(py);
    }

    /// Degrees on the sky of one pixel at (px, py): the geometric mean of the
    /// angular length of a step along x and one along y, so a rotated or
    /// non-square pixel gets one honest number. NaN when it cannot be told.
    double pixelScaleDeg(double px, double py) const
    {
        Vec3 c, x, y;
        if (!pixelToSky(px, py, c) || !pixelToSky(px + 0.5, py, x) || !pixelToSky(px, py + 0.5, y))
            return std::numeric_limits<double>::quiet_NaN();
        return 2.0 * std::sqrt(separationDeg(c, x) * separationDeg(c, y));
    }

private:
    static Vec3 unit(double lon, double lat)
    {
        return { std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat) };
    }

    // Equatorial to Galactic axes (Hipparcos, vol. 1, eq. 1.5.11).
    static constexpr double kGal[3][3] = {
        { -0.0548755604162154, -0.8734370902348850, -0.4838350155487132 },
        { +0.4941094278755837, -0.4448296299600112, +0.7469822444972189 },
        { -0.8676661490190047, -0.1980763734312015, +0.4559837761750669 }
    };

    /// A direction in this WCS's own axes to ICRS.
    Vec3 toEquatorial(const Vec3 &v) const
    {
        if (m_frame == Frame::Equatorial)
            return v;
        return { kGal[0][0] * v[0] + kGal[1][0] * v[1] + kGal[2][0] * v[2],
                 kGal[0][1] * v[0] + kGal[1][1] * v[1] + kGal[2][1] * v[2],
                 kGal[0][2] * v[0] + kGal[1][2] * v[1] + kGal[2][2] * v[2] };
    }
    Vec3 fromEquatorial(const Vec3 &v) const
    {
        if (m_frame == Frame::Equatorial)
            return v;
        return { kGal[0][0] * v[0] + kGal[0][1] * v[1] + kGal[0][2] * v[2],
                 kGal[1][0] * v[0] + kGal[1][1] * v[1] + kGal[1][2] * v[2],
                 kGal[2][0] * v[0] + kGal[2][1] * v[1] + kGal[2][2] * v[2] };
    }

    Projection m_proj{ Projection::None };
    Frame m_frame{ Frame::Equatorial };
    std::array<double, 2> m_crpix{ 0.0, 0.0 };   ///< FITS, 1-based
    std::array<double, 2> m_crval{ 0.0, 0.0 };   ///< degrees
    std::array<double, 4> m_cd{ 1.0, 0.0, 0.0, 1.0 };    ///< degrees per pixel, row-major
    std::array<double, 4> m_inv{ 1.0, 0.0, 0.0, 1.0 };   ///< its inverse
    double m_alphaP{ 0.0 }, m_deltaP{ 0.0 }, m_phiP{ 0.0 };   ///< native pole, radians
};

// ── Matching two views ───────────────────────────────────────────────────────

/// What a 2-D view shows, in its own world (0-based pixels of its dataset):
/// the pixel at the centre of the data area and how many pixels tall it is.
struct View
{
    double cx{ 0.0 };
    double cy{ 0.0 };
    double height{ 0.0 };
};

/// The view of @p to that shows the same sky as @p v does in @p from: the
/// same centre, the same angular height. North is not turned (a pan and a
/// zoom, as CARTA's XY matching does), so two images rotated against each
/// other keep their own orientation. False when the centre is not on @p to's
/// projection or either pixel scale is unknown.
inline bool matchView(const SkyWcs &from, const View &v, const SkyWcs &to, View &out)
{
    Vec3 sky;
    double tx = 0.0, ty = 0.0;
    if (!(v.height > 0.0) || !from.pixelToSky(v.cx, v.cy, sky) || !to.skyToPixel(sky, tx, ty))
        return false;
    const double sFrom = from.pixelScaleDeg(v.cx, v.cy);
    const double sTo = to.pixelScaleDeg(tx, ty);
    if (!(sFrom > 0.0) || !(sTo > 0.0) || !std::isfinite(sFrom) || !std::isfinite(sTo))
        return false;
    out = { tx, ty, v.height * sFrom / sTo };
    return true;
}

/// The pixel of @p to at the same sky position as pixel (px, py) of @p from.
inline bool matchPixel(const SkyWcs &from, double px, double py, const SkyWcs &to, double &tx,
                       double &ty)
{
    Vec3 sky;
    return from.pixelToSky(px, py, sky) && to.skyToPixel(sky, tx, ty);
}

/// How a small step in @p from's pixels at (px, py) lands in @p to's: the 2x2
/// matrix {a, b, c, d} (row-major) with d(tx) = a dpx + b dpy and
/// d(ty) = c dpx + d dpy, by a central difference of half a pixel. Regions
/// use it to carry a size and an angle across: the centre goes through the
/// full transform, the shape through this.
inline bool pixelJacobian(const SkyWcs &from, double px, double py, const SkyWcs &to,
                          std::array<double, 4> &m)
{
    constexpr double h = 0.5;
    double xp, yp, xm, ym, xq, yq, xr, yr;
    if (!matchPixel(from, px + h, py, to, xp, yp) || !matchPixel(from, px - h, py, to, xm, ym)
        || !matchPixel(from, px, py + h, to, xq, yq) || !matchPixel(from, px, py - h, to, xr, yr))
        return false;
    m = { (xp - xm) / (2.0 * h), (xq - xr) / (2.0 * h), (yp - ym) / (2.0 * h), (yq - yr) / (2.0 * h) };
    return true;
}

}   // namespace visivo::skylink

#endif   // SkyWcs_h

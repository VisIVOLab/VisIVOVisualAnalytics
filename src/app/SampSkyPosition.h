// SampSkyPosition.h – the sky position of an image pixel, for coord.pointAt.sky
//
// The FALLBACK for a backend older than POST /v1/datasets/pixel_to_sky, which
// places a pixel with the file's full WCS (rotation, CD/PC, SIP and other
// distortions, every projection). Use that whenever the backend has it.
//
// coord.pointAt.sky carries ICRS degrees. A viewer knows its image by the
// header's CTYPE/CRVAL/CRPIX/CDELT of the two sky axes (no CD matrix, no
// rotation, no distortion: that is what the client is given), so a pixel is
// turned into the native frame with the projection the CTYPE names, then
// rotated into ICRS:
//
//  * TAN (gnomonic, also "TAN-SIP" without its distortion) and SIN
//    (orthographic, the usual interferometer image) are inverted about CRVAL;
//  * anything else (CAR, as the Hi-GAL cutouts, or no projection code) uses
//    the linear model the viewers' coordinate readout uses,
//    CRVAL + (pixel + 1 − CRPIX)·CDELT, which is exact for CAR on the equator.
//
// Galactic is rotated with the IAU/Hipparcos matrix, ecliptic with the J2000
// mean obliquity. FK5 J2000 is taken as ICRS (they differ by under 25 mas).
//
// Plain logic on QtCore: unit-tested in tests/test_samp.cpp.

#ifndef SAMPSKYPOSITION_H
#define SAMPSKYPOSITION_H

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace visivo::samp {

enum class SkyFrame { Equatorial, Galactic, Ecliptic };

/// The two sky axes of an image as its header gives them. Pixels are 0-based.
struct ImageSkyWcs
{
    std::array<QString, 2> ctype;
    std::array<double, 2> crval{ 0.0, 0.0 };
    std::array<double, 2> crpix{ 0.0, 0.0 };
    std::array<double, 2> cdelt{ 0.0, 0.0 };
};

struct SkyPosition
{
    double raDeg{ 0.0 };
    double decDeg{ 0.0 };
};

namespace detail {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

inline std::optional<SkyFrame> frameOfLongitude(const QString &ctype)
{
    const QString t = ctype.trimmed().toUpper();
    if (t.startsWith(u"RA"))
        return SkyFrame::Equatorial;
    if (t.startsWith(u"GLON"))
        return SkyFrame::Galactic;
    if (t.startsWith(u"ELON"))
        return SkyFrame::Ecliptic;
    return std::nullopt;
}

inline std::optional<SkyFrame> frameOfLatitude(const QString &ctype)
{
    const QString t = ctype.trimmed().toUpper();
    if (t.startsWith(u"DEC"))
        return SkyFrame::Equatorial;
    if (t.startsWith(u"GLAT"))
        return SkyFrame::Galactic;
    if (t.startsWith(u"ELAT"))
        return SkyFrame::Ecliptic;
    return std::nullopt;
}

/// The projection code of a celestial CTYPE: characters 6-8 ("RA---TAN-SIP"
/// → "TAN"), whatever follows them.
inline QString projectionCode(const QString &ctype)
{
    return ctype.trimmed().toUpper().mid(5, 3);
}

inline std::array<double, 3> toVector(double lonDeg, double latDeg)
{
    const double lon = lonDeg * kDeg;
    const double lat = latDeg * kDeg;
    return { std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat) };
}

inline SkyPosition fromVector(const std::array<double, 3> &v)
{
    double ra = std::atan2(v[1], v[0]) / kDeg;
    if (ra < 0.0)
        ra += 360.0;
    const double z = std::max(-1.0, std::min(1.0, v[2]));
    return { ra, std::asin(z) / kDeg };
}

}  // namespace detail

/// The frame of an image's sky axes, or nullopt when they are not a
/// longitude/latitude pair of one frame (in either order).
inline std::optional<SkyFrame> skyFrameOf(const std::array<QString, 2> &ctype)
{
    const auto lon0 = detail::frameOfLongitude(ctype[0]);
    const auto lat1 = detail::frameOfLatitude(ctype[1]);
    if (lon0 && lat1 && *lon0 == *lat1)
        return lon0;
    const auto lat0 = detail::frameOfLatitude(ctype[0]);
    const auto lon1 = detail::frameOfLongitude(ctype[1]);
    if (lat0 && lon1 && *lat0 == *lon1)
        return lon1;
    return std::nullopt;
}

/// (@p lonDeg, @p latDeg) of @p frame in ICRS.
inline SkyPosition toIcrs(SkyFrame frame, double lonDeg, double latDeg)
{
    const std::array<double, 3> v = detail::toVector(lonDeg, latDeg);
    switch (frame) {
    case SkyFrame::Equatorial:
        return detail::fromVector(v);
    case SkyFrame::Galactic: {
        // Rows: the galactic axes in ICRS (Hipparcos, ESA 1997, §1.5.3);
        // ICRS = transpose · galactic.
        static constexpr double m[3][3] = {
            { -0.0548755604162154, -0.8734370902348850, -0.4838350155487132 },
            { 0.4941094278755837, -0.4448296299600112, 0.7469822444972189 },
            { -0.8676661490190047, -0.1980763734312015, 0.4559837761750669 },
        };
        const std::array<double, 3> e{
            m[0][0] * v[0] + m[1][0] * v[1] + m[2][0] * v[2],
            m[0][1] * v[0] + m[1][1] * v[1] + m[2][1] * v[2],
            m[0][2] * v[0] + m[1][2] * v[1] + m[2][2] * v[2],
        };
        return detail::fromVector(e);
    }
    case SkyFrame::Ecliptic: {
        const double eps = 23.4392911 * detail::kDeg;   // J2000 mean obliquity
        const std::array<double, 3> e{
            v[0],
            v[1] * std::cos(eps) - v[2] * std::sin(eps),
            v[1] * std::sin(eps) + v[2] * std::cos(eps),
        };
        return detail::fromVector(e);
    }
    }
    return detail::fromVector(v);
}

/// The native (lon, lat) of 0-based pixel (@p px, @p py), in degrees, or
/// nullopt off the projection (beyond a SIN image's horizon) or without a
/// usable scale.
inline std::optional<std::array<double, 2>> pixelToNative(const ImageSkyWcs &wcs, double px,
                                                          double py)
{
    for (int i = 0; i < 2; ++i) {
        if (!std::isfinite(wcs.cdelt[i]) || std::abs(wcs.cdelt[i]) < 1e-15
            || !std::isfinite(wcs.crval[i]) || !std::isfinite(wcs.crpix[i]))
            return std::nullopt;
    }
    if (!std::isfinite(px) || !std::isfinite(py))
        return std::nullopt;
    // Intermediate world coordinates (degrees); FITS pixels are 1-based.
    const double x = (px + 1.0 - wcs.crpix[0]) * wcs.cdelt[0];
    const double y = (py + 1.0 - wcs.crpix[1]) * wcs.cdelt[1];
    const QString code = detail::projectionCode(wcs.ctype[0]);
    const bool tan = code == u"TAN";
    const bool sin = code == u"SIN";
    if (!tan && !sin)
        return std::array<double, 2>{ wcs.crval[0] + x, wcs.crval[1] + y };

    // The longitude axis may come second (DEC--TAN, RA---TAN): the projection
    // is about (lon0, lat0) whatever the order.
    const bool latFirst = detail::frameOfLatitude(wcs.ctype[0]).has_value();
    const double xi = (latFirst ? y : x) * detail::kDeg;
    const double eta = (latFirst ? x : y) * detail::kDeg;
    const double lon0 = (latFirst ? wcs.crval[1] : wcs.crval[0]) * detail::kDeg;
    const double lat0 = (latFirst ? wcs.crval[0] : wcs.crval[1]) * detail::kDeg;
    const double rho = std::hypot(xi, eta);
    double lon = lon0;
    double lat = lat0;
    if (rho > 0.0) {
        if (sin && rho > 1.0)
            return std::nullopt;
        const double c = tan ? std::atan(rho) : std::asin(rho);
        const double sc = std::sin(c);
        const double cc = std::cos(c);
        lat = std::asin(cc * std::sin(lat0) + eta * sc * std::cos(lat0) / rho);
        lon = lon0 + std::atan2(xi * sc, rho * std::cos(lat0) * cc - eta * std::sin(lat0) * sc);
    }
    double lonDeg = std::fmod(lon / detail::kDeg, 360.0);
    if (lonDeg < 0.0)
        lonDeg += 360.0;
    const double latDeg = lat / detail::kDeg;
    return latFirst ? std::array<double, 2>{ latDeg, lonDeg }
                    : std::array<double, 2>{ lonDeg, latDeg };
}

/// The ICRS position of 0-based pixel (@p px, @p py) of an image, or nullopt
/// when its axes are not sky axes or the pixel is off the projection.
inline std::optional<SkyPosition> pixelToIcrs(const ImageSkyWcs &wcs, double px, double py)
{
    const auto frame = skyFrameOf(wcs.ctype);
    if (!frame)
        return std::nullopt;
    const auto native = pixelToNative(wcs, px, py);
    if (!native)
        return std::nullopt;
    const bool latFirst = detail::frameOfLatitude(wcs.ctype[0]).has_value();
    const double lon = latFirst ? (*native)[1] : (*native)[0];
    const double lat = latFirst ? (*native)[0] : (*native)[1];
    if (!std::isfinite(lat) || std::abs(lat) > 90.0)
        return std::nullopt;
    return toIcrs(*frame, lon, lat);
}

/// "RA 10.684708°, Dec +41.268750°": how a sent position is reported.
inline QString describe(const SkyPosition &p)
{
    return QStringLiteral("RA %1°, Dec %2%3°")
            .arg(p.raDeg, 0, 'f', 6)
            .arg(p.decDeg < 0.0 ? QString() : QStringLiteral("+"))
            .arg(p.decDeg, 0, 'f', 6);
}

}  // namespace visivo::samp

#endif  // SAMPSKYPOSITION_H

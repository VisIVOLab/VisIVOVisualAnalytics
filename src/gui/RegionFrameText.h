#ifndef REGION_FRAME_TEXT_H
#define REGION_FRAME_TEXT_H
// RegionFrameText.h
// Typing a region in a chosen frame (issue #341): the frames a properties
// editor offers, and the text of a position in each of them.
//
// Only text lives here. Moving a position from one frame to another is the
// backend's job (`POST /v1/regions/convert`, astropy on the dataset's full
// WCS); this file reads and writes the numbers of ONE frame: image pixels as
// ds9 counts them (1-based), decimal degrees, or sexagesimal (hours for the
// right ascension of an equatorial frame, as ds9 and CRTF write it).
//
// Pure QtCore and header-only: tests/test_region_model.cpp.

#include "RegionModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cmath>
#include <optional>
#include <vector>

namespace visivo::regions {

enum class FrameChoice { Pixels, IcrsDegrees, IcrsSexagesimal, Fk5Degrees, Fk5Sexagesimal, GalacticDegrees };

struct FrameInfo
{
    FrameChoice choice;
    QString label;          ///< what the frame combo shows
    QString backendFrame;   ///< "image", "icrs", "fk5", "galactic"
    bool sexagesimal;
};

inline const std::vector<FrameInfo> &frameInfos()
{
    static const std::vector<FrameInfo> infos{
        { FrameChoice::Pixels, QStringLiteral("Image pixels (1-based, as ds9)"),
          QStringLiteral("image"), false },
        { FrameChoice::IcrsDegrees, QStringLiteral("ICRS, degrees"), QStringLiteral("icrs"), false },
        { FrameChoice::IcrsSexagesimal, QStringLiteral("ICRS, sexagesimal"), QStringLiteral("icrs"), true },
        { FrameChoice::Fk5Degrees, QStringLiteral("FK5 (J2000), degrees"), QStringLiteral("fk5"), false },
        { FrameChoice::Fk5Sexagesimal, QStringLiteral("FK5 (J2000), sexagesimal"), QStringLiteral("fk5"), true },
        { FrameChoice::GalacticDegrees, QStringLiteral("Galactic, degrees"), QStringLiteral("galactic"), false },
    };
    return infos;
}

inline FrameInfo frameInfo(FrameChoice c)
{
    for (const FrameInfo &f : frameInfos()) {
        if (f.choice == c)
            return f;
    }
    return frameInfos().front();
}

inline bool isSky(FrameChoice c) { return c != FrameChoice::Pixels; }

/// The frames worth offering: sky frames only when the dataset has a celestial
/// WCS to reach them through (the backend reports it as a native frame).
inline std::vector<FrameChoice> availableFrames(bool hasCelestialWcs)
{
    std::vector<FrameChoice> out;
    for (const FrameInfo &f : frameInfos()) {
        if (f.choice == FrameChoice::Pixels || hasCelestialWcs)
            out.push_back(f.choice);
    }
    return out;
}

/// The choice that shows a region kept in `backendFrame` (a region typed in
/// FK5 opens in FK5 again). Sexagesimal for an equatorial frame, the way
/// catalogues and ds9 write positions.
inline FrameChoice frameChoiceFor(const QString &backendFrame)
{
    if (backendFrame == QStringLiteral("icrs"))
        return FrameChoice::IcrsSexagesimal;
    if (backendFrame == QStringLiteral("fk5"))
        return FrameChoice::Fk5Sexagesimal;
    if (backendFrame == QStringLiteral("galactic"))
        return FrameChoice::GalacticDegrees;
    return FrameChoice::Pixels;
}

/// The size unit of a frame in the editor: pixels on the image, arcseconds on the sky.
inline QString sizeUnit(FrameChoice c)
{
    return isSky(c) ? QStringLiteral("arcsec") : QStringLiteral("pix");
}

// ── Sexagesimal text ─────────────────────────────────────────────────────────

/// `deg` as "hh:mm:ss.ssss" (hours, 0 to 24) or "±dd:mm:ss.sss", carrying
/// rounding up through the fields so 59.99995 s never prints as "60".
inline QString formatSexagesimal(double deg, bool hours, int secondDecimals)
{
    double v = hours ? std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0) / 15.0 : deg;
    const bool negative = !hours && v < 0.0;
    v = std::abs(v);
    const double scale = std::pow(10.0, secondDecimals);
    // Everything in units of the last printed digit of the seconds.
    long long ticks = std::llround(v * 3600.0 * scale);
    const long long perMinute = static_cast<long long>(60 * scale);
    const long long perUnit = 60 * perMinute;
    if (hours)
        ticks %= 24 * perUnit;
    const long long whole = ticks / perUnit;
    const long long minutes = (ticks % perUnit) / perMinute;
    const double seconds = static_cast<double>(ticks % perMinute) / scale;
    const int secondsWidth = secondDecimals > 0 ? 3 + secondDecimals : 2;
    return QStringLiteral("%1%2:%3:%4")
            .arg(negative ? QStringLiteral("-") : (hours ? QString() : QStringLiteral("+")))
            .arg(whole, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, secondsWidth, 'f', secondDecimals, QLatin1Char('0'));
}

/// Read one coordinate typed by a person. Accepts decimal degrees ("202.4695"),
/// and sexagesimal with colons, spaces or unit letters ("13:29:52.7",
/// "13h29m52.7s", "+47 11 43", "47d11m43s", "-5°23'10\""). For the longitude of
/// an equatorial frame (`raHours`) a sexagesimal value is in hours unless its
/// first field ends in "d" or "°", as in ds9. Range-checked: latitudes within
/// ±90, longitudes wrapped into [0, 360).
inline std::optional<double> parseCoordinate(const QString &text, bool longitude, bool raHours)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return std::nullopt;
    bool ok = false;
    double value = t.toDouble(&ok);
    if (!ok) {
        static const QRegularExpression rx(QStringLiteral(
                "^([+-]?)\\s*(\\d+(?:\\.\\d*)?)\\s*([:hHdD°\\s])\\s*(\\d+(?:\\.\\d*)?)"
                "(?:\\s*[:mM'\\s]\\s*(\\d+(?:\\.\\d*)?))?\\s*[sS\"]?$"));
        const QRegularExpressionMatch m = rx.match(t);
        if (!m.hasMatch())
            return std::nullopt;
        const double a = m.captured(2).toDouble();
        const double b = m.captured(4).toDouble();
        const double c = m.captured(5).isEmpty() ? 0.0 : m.captured(5).toDouble();
        if (b >= 60.0 || c >= 60.0)
            return std::nullopt;
        const QString sep = m.captured(3).toLower();
        const bool inHours = longitude && raHours && sep != QStringLiteral("d")
                && sep != QStringLiteral("°");
        if (sep == QStringLiteral("h") && !(longitude && raHours))
            return std::nullopt;   // hours make sense only for a right ascension
        value = a + b / 60.0 + c / 3600.0;
        if (inHours)
            value *= 15.0;
        if (m.captured(1) == QStringLiteral("-"))
            value = -value;
    }
    if (!std::isfinite(value))
        return std::nullopt;
    if (longitude) {
        if (value < -360.0 || value > 360.0 + 1e-9)
            return std::nullopt;
        value = std::fmod(value + 360.0, 360.0);
        return value;
    }
    if (value < -90.0 - 1e-12 || value > 90.0 + 1e-12)
        return std::nullopt;
    return value;
}

/// One position as text in a frame: pixels with 3 decimals, degrees with 8
/// (about 0.04 mas), sexagesimal to 0.1 ms of time and 1 mas of arc.
inline std::array<QString, 2> formatPosition(const Vec2 &p, FrameChoice c)
{
    if (!isSky(c))
        return { QString::number(p[0], 'f', 3), QString::number(p[1], 'f', 3) };
    if (frameInfo(c).sexagesimal)
        return { formatSexagesimal(p[0], true, 4), formatSexagesimal(p[1], false, 3) };
    return { QString::number(p[0], 'f', 8), QString::number(p[1], 'f', 8) };
}

inline std::optional<Vec2> parsePosition(const QString &lon, const QString &lat, FrameChoice c)
{
    if (!isSky(c)) {
        bool okx = false, oky = false;
        const double x = lon.trimmed().toDouble(&okx), y = lat.trimmed().toDouble(&oky);
        if (!okx || !oky || !std::isfinite(x) || !std::isfinite(y))
            return std::nullopt;
        return Vec2{ x, y };
    }
    const bool equatorial = c != FrameChoice::GalacticDegrees;
    const auto a = parseCoordinate(lon, true, equatorial);
    const auto b = parseCoordinate(lat, false, equatorial);
    if (!a || !b)
        return std::nullopt;
    return Vec2{ *a, *b };
}

// ── The editor's fields ──────────────────────────────────────────────────────

/// A region as the properties editor shows it in one frame: the centre (or
/// the vertices) as text, the sizes in that frame's unit, the angle in degrees.
/// `sizes` per shape: circle {radius}; ellipse {semi-major, semi-minor}; box
/// {width, height}; annulus {inner, outer}; vector {length}; elliptical
/// annulus {inner a, inner b, outer a, outer b}; box annulus {inner width,
/// inner height, outer width, outer height}; none for a point, a polygon, a
/// line or a text.
struct RegionFields
{
    Shape shape{ Shape::Box };
    std::array<QString, 2> center;
    std::vector<std::array<QString, 2>> vertices;
    std::vector<double> sizes;
    double angleDeg{ 0.0 };
};

inline std::vector<QString> sizeLabels(Shape s)
{
    switch (s) {
    case Shape::Circle:  return { QStringLiteral("Radius") };
    case Shape::Ellipse: return { QStringLiteral("Semi-major axis"), QStringLiteral("Semi-minor axis") };
    case Shape::Box:     return { QStringLiteral("Width"), QStringLiteral("Height") };
    case Shape::Annulus: return { QStringLiteral("Inner radius"), QStringLiteral("Outer radius") };
    case Shape::Vector:  return { QStringLiteral("Length") };
    case Shape::EllipseAnnulus:
        return { QStringLiteral("Inner semi-major"), QStringLiteral("Inner semi-minor"),
                 QStringLiteral("Outer semi-major"), QStringLiteral("Outer semi-minor") };
    case Shape::BoxAnnulus:
        return { QStringLiteral("Inner width"), QStringLiteral("Inner height"),
                 QStringLiteral("Outer width"), QStringLiteral("Outer height") };
    case Shape::Point:
    case Shape::Polygon:
    case Shape::Line:
    case Shape::Text:    return {};
    }
    return {};
}

inline bool hasAngle(Shape s)
{
    return s == Shape::Ellipse || s == Shape::Box || s == Shape::Vector
            || s == Shape::EllipseAnnulus || s == Shape::BoxAnnulus;
}

/// The fewest vertices a polygon or a line keeps.
inline int minVertices(Shape s) { return s == Shape::Line ? 2 : 3; }

/// The fields of a canonical region already expressed in frame `c` (pixels
/// 1-based in the image frame, sizes in `sizeUnit(c)`).
inline RegionFields fieldsFromCanonical(const QJsonObject &o, FrameChoice c)
{
    RegionFields f;
    f.shape = shapeFromName(o.value(QStringLiteral("shape")).toString()).value_or(Shape::Box);
    const auto pair = [&o](const QString &key) {
        const QJsonArray a = o.value(key).toArray();
        return Vec2{ a.at(0).toDouble(), a.at(1).toDouble() };
    };
    if (isPointList(f.shape)) {
        for (const QJsonValue &v : o.value(QStringLiteral("points")).toArray()) {
            const QJsonArray p = v.toArray();
            f.vertices.push_back(formatPosition({ p.at(0).toDouble(), p.at(1).toDouble() }, c));
        }
    } else {
        f.center = formatPosition(pair(QStringLiteral("center")), c);
    }
    switch (f.shape) {
    case Shape::Circle:
        f.sizes = { o.value(QStringLiteral("radius")).toDouble() };
        break;
    case Shape::Ellipse: {
        const Vec2 s = pair(QStringLiteral("semi_axes"));
        f.sizes = { s[0], s[1] };
        break;
    }
    case Shape::Box: {
        const Vec2 s = pair(QStringLiteral("size"));
        f.sizes = { s[0], s[1] };
        break;
    }
    case Shape::Annulus: {
        const Vec2 s = pair(QStringLiteral("radii"));
        f.sizes = { s[0], s[1] };
        break;
    }
    case Shape::Vector:
        f.sizes = { o.value(QStringLiteral("length")).toDouble() };
        break;
    case Shape::EllipseAnnulus:
    case Shape::BoxAnnulus: {
        const Vec2 i = pair(QStringLiteral("inner")), out = pair(QStringLiteral("outer"));
        f.sizes = { i[0], i[1], out[0], out[1] };
        break;
    }
    case Shape::Point:
    case Shape::Polygon:
    case Shape::Line:
    case Shape::Text:
        break;
    }
    f.angleDeg = o.value(QStringLiteral("angle_deg")).toDouble(0.0);
    return f;
}

/// The canonical region the fields describe in frame `c`, or false with the
/// reason a person can act on ("Centre: … is not a right ascension").
inline bool canonicalFromFields(const RegionFields &f, FrameChoice c, QJsonObject *out,
                                QString *error = nullptr)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const FrameInfo info = frameInfo(c);
    QJsonObject o{ { QStringLiteral("shape"), shapeName(f.shape) },
                   { QStringLiteral("frame"), info.backendFrame },
                   { QStringLiteral("unit"), sizeUnit(c) } };
    const QString what = isSky(c) ? (info.sexagesimal ? QStringLiteral("a position (hh:mm:ss, dd:mm:ss or degrees)")
                                                      : QStringLiteral("a position in degrees"))
                                  : QStringLiteral("a pixel position");
    if (isPointList(f.shape)) {
        if (static_cast<int>(f.vertices.size()) < minVertices(f.shape))
            return fail(f.shape == Shape::Line ? QStringLiteral("A line needs at least two vertices.")
                                               : QStringLiteral("A polygon needs at least three vertices."));
        QJsonArray pts;
        for (std::size_t i = 0; i < f.vertices.size(); ++i) {
            const auto p = parsePosition(f.vertices[i][0], f.vertices[i][1], c);
            if (!p)
                return fail(QStringLiteral("Vertex %1: \"%2, %3\" is not %4.")
                                    .arg(static_cast<int>(i) + 1)
                                    .arg(f.vertices[i][0], f.vertices[i][1], what));
            pts.append(QJsonArray{ (*p)[0], (*p)[1] });
        }
        o[QStringLiteral("points")] = pts;
    } else {
        const auto p = parsePosition(f.center[0], f.center[1], c);
        if (!p)
            return fail(QStringLiteral("Centre: \"%1, %2\" is not %3.").arg(f.center[0], f.center[1], what));
        o[QStringLiteral("center")] = QJsonArray{ (*p)[0], (*p)[1] };
    }
    const auto labels = sizeLabels(f.shape);
    if (f.sizes.size() < labels.size())
        return fail(QStringLiteral("Missing size."));
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const double v = f.sizes[i];
        const bool innerMayBeZero = (f.shape == Shape::Annulus && i == 0)
                || ((f.shape == Shape::EllipseAnnulus || f.shape == Shape::BoxAnnulus) && i < 2);
        if (!std::isfinite(v) || v < 0.0 || (v == 0.0 && !innerMayBeZero))
            return fail(QStringLiteral("%1 must be greater than zero.").arg(labels[i]));
    }
    switch (f.shape) {
    case Shape::Circle:
        o[QStringLiteral("radius")] = f.sizes[0];
        break;
    case Shape::Ellipse:
        o[QStringLiteral("semi_axes")] = QJsonArray{ f.sizes[0], f.sizes[1] };
        break;
    case Shape::Box:
        o[QStringLiteral("size")] = QJsonArray{ f.sizes[0], f.sizes[1] };
        break;
    case Shape::Annulus:
        if (f.sizes[0] >= f.sizes[1])
            return fail(QStringLiteral("The inner radius must be smaller than the outer radius."));
        o[QStringLiteral("radii")] = QJsonArray{ f.sizes[0], f.sizes[1] };
        break;
    case Shape::Vector:
        o[QStringLiteral("length")] = f.sizes[0];
        break;
    case Shape::EllipseAnnulus:
    case Shape::BoxAnnulus:
        if (f.sizes[0] >= f.sizes[2] || f.sizes[1] >= f.sizes[3])
            return fail(QStringLiteral("The inner edge must be smaller than the outer one."));
        o[QStringLiteral("inner")] = QJsonArray{ f.sizes[0], f.sizes[1] };
        o[QStringLiteral("outer")] = QJsonArray{ f.sizes[2], f.sizes[3] };
        break;
    case Shape::Point:
    case Shape::Polygon:
    case Shape::Line:
    case Shape::Text:
        break;
    }
    if (hasAngle(f.shape)) {
        if (!std::isfinite(f.angleDeg))
            return fail(QStringLiteral("The angle is not a number."));
        o[QStringLiteral("angle_deg")] = f.angleDeg;
    }
    *out = o;
    return true;
}

}   // namespace visivo::regions

#endif   // REGION_FRAME_TEXT_H

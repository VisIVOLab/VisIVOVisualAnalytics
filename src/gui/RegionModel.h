#ifndef REGION_MODEL_H
#define REGION_MODEL_H
// RegionModel.h
// The regions of one window as data (issue #341).
//
// A window used to hold one transient region in plain members: drawing a second
// replaced the first, nothing could be selected, moved or typed in, and the
// shapes were whole displayed voxels. Here a window keeps a LIST of regions the
// user owns until they delete them, one of which is selected.
//
// Coordinates. Every region is stored in the image's own 0-based pixel frame at
// FULL resolution, in double precision, with pixel centres on integers (the
// frame of FITS minus one, and the world frame the viewers draw in). That frame
// does not change when the viewer swaps a decimated overview for the full
// image, so a region drawn on one is still right on the other. Sky positions
// are never computed here: the backend converts with the dataset's full WCS
// (`POST /v1/regions/convert`). The frame a region was last typed or read in is
// remembered so the properties editor and the export can offer it again.
//
// Everything below is pure QtCore and header-only so it is unit-tested without
// a window (tests/test_region_model.cpp): hit tests on the body and on the
// handles, moves, handle drags, and the undo stack.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace visivo::regions {

enum class Shape { Point, Circle, Ellipse, Box, Polygon, Annulus };

using Vec2 = std::array<double, 2>;

/// The canonical shape names of the backend's region model (docs/backend-api/regions.md).
inline QString shapeName(Shape s)
{
    switch (s) {
    case Shape::Point:   return QStringLiteral("point");
    case Shape::Circle:  return QStringLiteral("circle");
    case Shape::Ellipse: return QStringLiteral("ellipse");
    case Shape::Box:     return QStringLiteral("box");
    case Shape::Polygon: return QStringLiteral("polygon");
    case Shape::Annulus: return QStringLiteral("annulus");
    }
    return QString();
}

inline std::optional<Shape> shapeFromName(const QString &name)
{
    for (Shape s : { Shape::Point, Shape::Circle, Shape::Ellipse, Shape::Box, Shape::Polygon,
                     Shape::Annulus }) {
        if (shapeName(s) == name)
            return s;
    }
    return std::nullopt;
}

/// What a person calls it: "Box", "Annulus".
inline QString shapeLabel(Shape s)
{
    QString n = shapeName(s);
    if (!n.isEmpty())
        n[0] = n[0].toUpper();
    return n;
}

struct Region
{
    int id{ 0 };
    Shape shape{ Shape::Box };
    QString name;              ///< "Box 1"; written as the region's text on export
    QString color;             ///< "#rrggbb"; empty = the viewer's default colour
    Vec2 center{ 0.0, 0.0 };   ///< every shape but the polygon
    double radius{ 0.0 };      ///< circle
    Vec2 semiAxes{ 0.0, 0.0 }; ///< ellipse: along the angle, then across it
    Vec2 size{ 0.0, 0.0 };     ///< box: full width along the angle, full height across it
    Vec2 radii{ 0.0, 0.0 };    ///< annulus: inner, outer
    double angleDeg{ 0.0 };    ///< ellipse and box: counter-clockwise from pixel +x
    std::vector<Vec2> points;  ///< polygon vertices
    /// Frame and size unit the region was last typed or read in ("icrs",
    /// "galactic", …); empty when it was drawn on the image.
    QString sourceFrame;
    QString sourceUnit;
};

inline bool sameGeometry(const Region &a, const Region &b)
{
    return a.shape == b.shape && a.center == b.center && a.radius == b.radius
            && a.semiAxes == b.semiAxes && a.size == b.size && a.radii == b.radii
            && a.angleDeg == b.angleDeg && a.points == b.points;
}

// ── Geometry ─────────────────────────────────────────────────────────────────

constexpr double kPi = 3.14159265358979323846;

inline Vec2 rotated(const Vec2 &v, double deg)
{
    const double t = deg * kPi / 180.0;
    const double c = std::cos(t), s = std::sin(t);
    return { c * v[0] - s * v[1], s * v[0] + c * v[1] };
}

/// `p` in the region's own axes: origin at the centre, x along its angle.
inline Vec2 toLocal(const Region &r, const Vec2 &p)
{
    return rotated({ p[0] - r.center[0], p[1] - r.center[1] }, -r.angleDeg);
}

inline Vec2 toWorld(const Region &r, const Vec2 &local)
{
    const Vec2 v = rotated(local, r.angleDeg);
    return { r.center[0] + v[0], r.center[1] + v[1] };
}

/// The four corners of a box, counter-clockwise from (-w/2, -h/2) in its own axes.
inline std::array<Vec2, 4> boxCorners(const Region &r)
{
    const double hw = r.size[0] / 2.0, hh = r.size[1] / 2.0;
    return { toWorld(r, { -hw, -hh }), toWorld(r, { hw, -hh }), toWorld(r, { hw, hh }),
             toWorld(r, { -hw, hh }) };
}

/// Is the pixel centred at (x, y) in the region? The backend's rules for the
/// statistics (`_region_mask`): inside a circle, ellipse or rotated box, between
/// an annulus's radii with both edges included, inside a polygon by the
/// even-odd rule, or the pixel nearest a point. Edges count as inside, within
/// a hair, so a box from pixel 4 to pixel 10 holds both.
inline bool contains(const Region &r, double x, double y)
{
    constexpr double eps = 1e-9;
    const double dx = x - r.center[0], dy = y - r.center[1];
    switch (r.shape) {
    case Shape::Point:
        return std::abs(dx) <= 0.5 && std::abs(dy) <= 0.5;
    case Shape::Circle:
        return dx * dx + dy * dy <= r.radius * r.radius + eps;
    case Shape::Annulus: {
        const double d = std::sqrt(dx * dx + dy * dy);
        return d >= std::max(0.0, r.radii[0]) - eps && d <= r.radii[1] + eps;
    }
    case Shape::Ellipse: {
        if (r.semiAxes[0] <= 0.0 || r.semiAxes[1] <= 0.0)
            return false;
        const Vec2 l = toLocal(r, { x, y });
        const double u = l[0] / r.semiAxes[0], v = l[1] / r.semiAxes[1];
        return u * u + v * v <= 1.0 + eps;
    }
    case Shape::Box: {
        const Vec2 l = toLocal(r, { x, y });
        return std::abs(l[0]) <= r.size[0] / 2.0 + eps && std::abs(l[1]) <= r.size[1] / 2.0 + eps;
    }
    case Shape::Polygon: {
        const auto &v = r.points;
        if (v.size() < 3)
            return false;
        bool inside = false;
        for (std::size_t i = 0, j = v.size() - 1; i < v.size(); j = i++) {
            const double xi = v[i][0], yi = v[i][1], xj = v[j][0], yj = v[j][1];
            if ((yi > y) != (yj > y)) {
                const double xCross = (xj - xi) * (y - yi) / (yj - yi) + xi;
                if (x < xCross)
                    inside = !inside;
            }
        }
        return inside;
    }
    }
    return false;
}

/// {xmin, xmax, ymin, ymax} of the region.
inline std::array<double, 4> bounds(const Region &r)
{
    const auto square = [&](double rad) {
        return std::array<double, 4>{ r.center[0] - rad, r.center[0] + rad, r.center[1] - rad,
                                      r.center[1] + rad };
    };
    switch (r.shape) {
    case Shape::Point:   return square(0.5);
    case Shape::Circle:  return square(r.radius);
    case Shape::Annulus: return square(r.radii[1]);
    case Shape::Ellipse: {
        // Exact extent of a rotated ellipse.
        const double t = r.angleDeg * kPi / 180.0;
        const double a = r.semiAxes[0], b = r.semiAxes[1];
        const double ex = std::sqrt(a * a * std::cos(t) * std::cos(t) + b * b * std::sin(t) * std::sin(t));
        const double ey = std::sqrt(a * a * std::sin(t) * std::sin(t) + b * b * std::cos(t) * std::cos(t));
        return { r.center[0] - ex, r.center[0] + ex, r.center[1] - ey, r.center[1] + ey };
    }
    case Shape::Box:
    case Shape::Polygon: {
        std::vector<Vec2> pts;
        if (r.shape == Shape::Box) {
            const auto c = boxCorners(r);
            pts.assign(c.begin(), c.end());
        } else {
            pts = r.points;
        }
        if (pts.empty())
            return { 0, 0, 0, 0 };
        std::array<double, 4> b{ pts[0][0], pts[0][0], pts[0][1], pts[0][1] };
        for (const Vec2 &p : pts) {
            b[0] = std::min(b[0], p[0]);
            b[1] = std::max(b[1], p[0]);
            b[2] = std::min(b[2], p[1]);
            b[3] = std::max(b[3], p[1]);
        }
        return b;
    }
    }
    return { 0, 0, 0, 0 };
}

/// The closed outlines to draw: one loop for most shapes, two for an annulus
/// (outer, then inner; the inner one is empty at radius 0), none for a point,
/// which the viewer marks itself. A loop does not repeat its first vertex.
inline std::vector<std::vector<Vec2>> outlines(const Region &r, int segments = 96)
{
    const auto ellipse = [&](double a, double b) {
        std::vector<Vec2> loop;
        loop.reserve(static_cast<std::size_t>(segments));
        for (int i = 0; i < segments; ++i) {
            const double t = 2.0 * kPi * i / segments;
            loop.push_back(toWorld(r, { a * std::cos(t), b * std::sin(t) }));
        }
        return loop;
    };
    switch (r.shape) {
    case Shape::Point:   return {};
    case Shape::Circle:  return { ellipse(r.radius, r.radius) };
    case Shape::Ellipse: return { ellipse(r.semiAxes[0], r.semiAxes[1]) };
    case Shape::Annulus: {
        std::vector<std::vector<Vec2>> loops{ ellipse(r.radii[1], r.radii[1]) };
        loops.push_back(r.radii[0] > 0.0 ? ellipse(r.radii[0], r.radii[0]) : std::vector<Vec2>{});
        return loops;
    }
    case Shape::Box: {
        const auto c = boxCorners(r);
        return { std::vector<Vec2>(c.begin(), c.end()) };
    }
    case Shape::Polygon: return { r.points };
    }
    return {};
}

inline double distanceToSegment(const Vec2 &p, const Vec2 &a, const Vec2 &b)
{
    const double vx = b[0] - a[0], vy = b[1] - a[1];
    const double len2 = vx * vx + vy * vy;
    double t = len2 > 0.0 ? ((p[0] - a[0]) * vx + (p[1] - a[1]) * vy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p[0] - (a[0] + t * vx), p[1] - (a[1] + t * vy));
}

/// Shortest distance from `p` to the region's drawn outline (to the centre for a point).
inline double distanceToOutline(const Region &r, const Vec2 &p)
{
    if (r.shape == Shape::Point)
        return std::hypot(p[0] - r.center[0], p[1] - r.center[1]);
    double best = std::numeric_limits<double>::infinity();
    for (const auto &loop : outlines(r)) {
        for (std::size_t i = 0; i < loop.size(); ++i)
            best = std::min(best, distanceToSegment(p, loop[i], loop[(i + 1) % loop.size()]));
    }
    return best;
}

// ── Handles ──────────────────────────────────────────────────────────────────
// What the selected region offers to drag: a box's four corners, a polygon's
// vertices, a circle's rim, an annulus's two radii, an ellipse's two semi-axes,
// and a rotation knob standing off the top edge of a box or an ellipse.

enum class HandleKind { Vertex, Corner, Radius, InnerRadius, MajorAxis, MinorAxis, Rotate };

struct Handle
{
    HandleKind kind{ HandleKind::Vertex };
    int index{ 0 };   ///< polygon vertex / box corner
    Vec2 pos{ 0.0, 0.0 };
};

/// The handles of a region. `knobOffset` is how far the rotation knob stands
/// off the edge, in pixels of the image (the viewer passes a fixed distance on
/// screen converted at the current zoom).
inline std::vector<Handle> handles(const Region &r, double knobOffset)
{
    std::vector<Handle> out;
    switch (r.shape) {
    case Shape::Point:
        break;
    case Shape::Circle:
        out.push_back({ HandleKind::Radius, 0, { r.center[0] + r.radius, r.center[1] } });
        break;
    case Shape::Annulus:
        out.push_back({ HandleKind::Radius, 0, { r.center[0] + r.radii[1], r.center[1] } });
        // On the other side, so the two never sit on top of each other.
        out.push_back({ HandleKind::InnerRadius, 0, { r.center[0] - r.radii[0], r.center[1] } });
        break;
    case Shape::Ellipse:
        out.push_back({ HandleKind::MajorAxis, 0, toWorld(r, { r.semiAxes[0], 0.0 }) });
        out.push_back({ HandleKind::MinorAxis, 0, toWorld(r, { 0.0, r.semiAxes[1] }) });
        out.push_back({ HandleKind::Rotate, 0, toWorld(r, { 0.0, r.semiAxes[1] + knobOffset }) });
        break;
    case Shape::Box: {
        const auto c = boxCorners(r);
        for (int i = 0; i < 4; ++i)
            out.push_back({ HandleKind::Corner, i, c[static_cast<std::size_t>(i)] });
        out.push_back({ HandleKind::Rotate, 0, toWorld(r, { 0.0, r.size[1] / 2.0 + knobOffset }) });
        break;
    }
    case Shape::Polygon:
        for (std::size_t i = 0; i < r.points.size(); ++i)
            out.push_back({ HandleKind::Vertex, static_cast<int>(i), r.points[i] });
        break;
    }
    return out;
}

struct Hit
{
    enum Kind { None, Body, OnHandle } kind{ None };
    Handle handle;
};

/// What a press at `p` lands on. Handles are tried first (only meaningful on
/// the selected region), then the body: inside it, or within `tolerance` of
/// its outline, so a thin box or the ring of an annulus can still be picked.
inline Hit hitTest(const Region &r, const Vec2 &p, double tolerance, double knobOffset,
                   bool withHandles)
{
    if (withHandles) {
        double best = tolerance;
        std::optional<Handle> found;
        for (const Handle &h : handles(r, knobOffset)) {
            const double d = std::hypot(p[0] - h.pos[0], p[1] - h.pos[1]);
            if (d <= best) {
                best = d;
                found = h;
            }
        }
        if (found)
            return { Hit::OnHandle, *found };
    }
    if (contains(r, p[0], p[1]) || distanceToOutline(r, p) <= tolerance)
        return { Hit::Body, {} };
    return {};
}

inline Region moved(Region r, double dx, double dy)
{
    r.center = { r.center[0] + dx, r.center[1] + dy };
    for (Vec2 &v : r.points)
        v = { v[0] + dx, v[1] + dy };
    return r;
}

/// The region with handle `h` dragged to `p`. A box corner moves while the
/// opposite corner stays put (in the box's own axes, so a rotated box stays
/// rotated); radii and semi-axes follow the distance from the centre along
/// their own direction; the rotation knob turns the shape about its centre.
/// Sizes never drop below `minSize` pixels, so a drag cannot make a region
/// vanish under the cursor.
inline Region withHandleAt(Region r, const Handle &h, const Vec2 &p, double minSize = 0.5)
{
    const double dx = p[0] - r.center[0], dy = p[1] - r.center[1];
    const double dist = std::hypot(dx, dy);
    switch (h.kind) {
    case HandleKind::Vertex:
        if (h.index >= 0 && h.index < static_cast<int>(r.points.size()))
            r.points[static_cast<std::size_t>(h.index)] = p;
        break;
    case HandleKind::Corner: {
        if (r.shape != Shape::Box || h.index < 0 || h.index > 3)
            break;
        const Vec2 opposite = boxCorners(r)[static_cast<std::size_t>((h.index + 2) % 4)];
        const Vec2 span = rotated({ p[0] - opposite[0], p[1] - opposite[1] }, -r.angleDeg);
        const double w = std::max(minSize, std::abs(span[0]));
        const double hgt = std::max(minSize, std::abs(span[1]));
        // Keep the side the corner was dragged to, even when clamped.
        const Vec2 local{ std::copysign(w, span[0] == 0.0 ? 1.0 : span[0]),
                          std::copysign(hgt, span[1] == 0.0 ? 1.0 : span[1]) };
        const Vec2 half = rotated({ local[0] / 2.0, local[1] / 2.0 }, r.angleDeg);
        r.center = { opposite[0] + half[0], opposite[1] + half[1] };
        r.size = { w, hgt };
        break;
    }
    case HandleKind::Radius:
        if (r.shape == Shape::Circle)
            r.radius = std::max(minSize, dist);
        else if (r.shape == Shape::Annulus)
            r.radii[1] = std::max(r.radii[0] + minSize, dist);
        break;
    case HandleKind::InnerRadius:
        if (r.shape == Shape::Annulus)
            r.radii[0] = std::clamp(dist, 0.0, std::max(0.0, r.radii[1] - minSize));
        break;
    case HandleKind::MajorAxis:
    case HandleKind::MinorAxis: {
        if (r.shape != Shape::Ellipse)
            break;
        const Vec2 l = toLocal(r, p);
        if (h.kind == HandleKind::MajorAxis)
            r.semiAxes[0] = std::max(minSize, std::abs(l[0]));
        else
            r.semiAxes[1] = std::max(minSize, std::abs(l[1]));
        break;
    }
    case HandleKind::Rotate: {
        if (dist <= 0.0)
            break;
        // The knob sits on the shape's +y axis: that axis now points at the cursor.
        double a = std::atan2(dy, dx) * 180.0 / kPi - 90.0;
        a = std::fmod(a, 360.0);
        if (a < 0.0)
            a += 360.0;
        r.angleDeg = a;
        break;
    }
    }
    return r;
}

/// A region whose numbers can be drawn and measured.
inline bool isUsable(const Region &r)
{
    const auto finite = [](const Vec2 &v) { return std::isfinite(v[0]) && std::isfinite(v[1]); };
    if (r.shape != Shape::Polygon && !finite(r.center))
        return false;
    switch (r.shape) {
    case Shape::Point:   return true;
    case Shape::Circle:  return std::isfinite(r.radius) && r.radius > 0.0;
    case Shape::Ellipse: return finite(r.semiAxes) && r.semiAxes[0] > 0.0 && r.semiAxes[1] > 0.0
                                && std::isfinite(r.angleDeg);
    case Shape::Box:     return finite(r.size) && r.size[0] > 0.0 && r.size[1] > 0.0
                                && std::isfinite(r.angleDeg);
    case Shape::Annulus: return finite(r.radii) && r.radii[0] >= 0.0 && r.radii[1] > r.radii[0];
    case Shape::Polygon:
        return r.points.size() >= 3
                && std::all_of(r.points.begin(), r.points.end(), finite);
    }
    return false;
}

// ── Canonical JSON (the backend's region model) ──────────────────────────────

/// The region as a canonical `image`-frame region in pixels. `originOffset` is
/// added to every position: 0 for the backend's statistics (0-based), 1 for a
/// region file or a conversion run with `origin: 1`.
inline QJsonObject toCanonical(const Region &r, double originOffset)
{
    const auto pos = [originOffset](const Vec2 &v) {
        return QJsonArray{ v[0] + originOffset, v[1] + originOffset };
    };
    QJsonObject o{ { QStringLiteral("shape"), shapeName(r.shape) },
                   { QStringLiteral("frame"), QStringLiteral("image") },
                   { QStringLiteral("unit"), QStringLiteral("pix") } };
    switch (r.shape) {
    case Shape::Point:
        o[QStringLiteral("center")] = pos(r.center);
        break;
    case Shape::Circle:
        o[QStringLiteral("center")] = pos(r.center);
        o[QStringLiteral("radius")] = r.radius;
        break;
    case Shape::Ellipse:
        o[QStringLiteral("center")] = pos(r.center);
        o[QStringLiteral("semi_axes")] = QJsonArray{ r.semiAxes[0], r.semiAxes[1] };
        o[QStringLiteral("angle_deg")] = r.angleDeg;
        break;
    case Shape::Box:
        o[QStringLiteral("center")] = pos(r.center);
        o[QStringLiteral("size")] = QJsonArray{ r.size[0], r.size[1] };
        o[QStringLiteral("angle_deg")] = r.angleDeg;
        break;
    case Shape::Annulus:
        o[QStringLiteral("center")] = pos(r.center);
        o[QStringLiteral("radii")] = QJsonArray{ r.radii[0], r.radii[1] };
        break;
    case Shape::Polygon: {
        QJsonArray pts;
        for (const Vec2 &v : r.points)
            pts.append(pos(v));
        o[QStringLiteral("points")] = pts;
        break;
    }
    }
    QJsonObject props;
    if (!r.name.isEmpty())
        props[QStringLiteral("text")] = r.name;
    if (!r.color.isEmpty())
        props[QStringLiteral("color")] = r.color;
    if (!props.isEmpty())
        o[QStringLiteral("props")] = props;
    return o;
}

namespace detail {
inline bool readPair(const QJsonObject &o, const QString &key, Vec2 *out)
{
    const QJsonArray a = o.value(key).toArray();
    if (a.size() != 2 || !a.at(0).isDouble() || !a.at(1).isDouble())
        return false;
    *out = { a.at(0).toDouble(), a.at(1).toDouble() };
    return std::isfinite((*out)[0]) && std::isfinite((*out)[1]);
}
}   // namespace detail

/// Read a canonical `image`-frame region in pixels (sizes in `pix`) into a
/// Region, subtracting `originOffset` from the positions. Fails, with a reason,
/// on anything else: a sky region must be converted by the backend first.
inline bool fromCanonical(const QJsonObject &o, double originOffset, Region *out,
                          QString *error = nullptr)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QString frame = o.value(QStringLiteral("frame")).toString(QStringLiteral("image"));
    const QString unit = o.value(QStringLiteral("unit")).toString(QStringLiteral("pix"));
    if (frame != QStringLiteral("image") || unit != QStringLiteral("pix"))
        return fail(QStringLiteral("the region is in %1 (%2), not in pixels").arg(frame, unit));
    const auto shape = shapeFromName(o.value(QStringLiteral("shape")).toString());
    if (!shape)
        return fail(QStringLiteral("unknown shape '%1'")
                            .arg(o.value(QStringLiteral("shape")).toString()));
    Region r;
    r.shape = *shape;
    const auto shift = [originOffset](Vec2 v) { return Vec2{ v[0] - originOffset, v[1] - originOffset }; };
    if (r.shape == Shape::Polygon) {
        for (const QJsonValue &pv : o.value(QStringLiteral("points")).toArray()) {
            const QJsonArray p = pv.toArray();
            if (p.size() != 2 || !p.at(0).isDouble() || !p.at(1).isDouble())
                return fail(QStringLiteral("a polygon vertex is not a pair of numbers"));
            r.points.push_back(shift({ p.at(0).toDouble(), p.at(1).toDouble() }));
        }
    } else {
        Vec2 c;
        if (!detail::readPair(o, QStringLiteral("center"), &c))
            return fail(QStringLiteral("the region has no centre"));
        r.center = shift(c);
    }
    const double angle = o.value(QStringLiteral("angle_deg")).toDouble(0.0);
    switch (r.shape) {
    case Shape::Circle:
        r.radius = o.value(QStringLiteral("radius")).toDouble(-1.0);
        break;
    case Shape::Ellipse:
        if (!detail::readPair(o, QStringLiteral("semi_axes"), &r.semiAxes))
            return fail(QStringLiteral("the ellipse has no semi-axes"));
        r.angleDeg = angle;
        break;
    case Shape::Box:
        if (!detail::readPair(o, QStringLiteral("size"), &r.size))
            return fail(QStringLiteral("the box has no size"));
        r.angleDeg = angle;
        break;
    case Shape::Annulus:
        if (!detail::readPair(o, QStringLiteral("radii"), &r.radii))
            return fail(QStringLiteral("the annulus has no radii"));
        break;
    case Shape::Point:
    case Shape::Polygon:
        break;
    }
    if (!isUsable(r))
        return fail(QStringLiteral("the %1 has no area (a size is zero, negative or missing)")
                            .arg(shapeName(r.shape)));
    const QJsonObject props = o.value(QStringLiteral("props")).toObject();
    r.name = props.value(QStringLiteral("text")).toString();
    r.color = props.value(QStringLiteral("color")).toString();
    *out = r;
    return true;
}

// ── The list ─────────────────────────────────────────────────────────────────

/// The regions of one window, the selected one, and an undo stack. Every
/// change pushes the state before it; a drag is one change however many mouse
/// moves it takes (beginGesture / endGesture).
class RegionList
{
public:
    const std::vector<Region> &regions() const { return m_regions; }
    int count() const { return static_cast<int>(m_regions.size()); }
    bool isEmpty() const { return m_regions.empty(); }

    const Region *find(int id) const
    {
        for (const Region &r : m_regions) {
            if (r.id == id)
                return &r;
        }
        return nullptr;
    }

    int selectedId() const { return m_selected; }
    const Region *selected() const { return find(m_selected); }

    /// Select `id` (-1 for none). Selection alone is not an undoable change.
    bool select(int id)
    {
        if (id != -1 && !find(id))
            return false;
        m_selected = id;
        return true;
    }

    /// Add a region, give it an id and, when it has none, a name ("Box 3"),
    /// and select it. Returns the id.
    int add(Region r)
    {
        pushUndo();
        r.id = m_nextId++;
        if (r.name.isEmpty())
            r.name = QStringLiteral("%1 %2").arg(shapeLabel(r.shape)).arg(++m_named[static_cast<std::size_t>(r.shape)]);
        m_regions.push_back(r);
        m_selected = r.id;
        return r.id;
    }

    /// Replace the geometry/properties of the region with `r.id`.
    bool replace(const Region &r)
    {
        for (Region &mine : m_regions) {
            if (mine.id != r.id)
                continue;
            if (!m_inGesture)
                pushUndo();
            else
                m_gestureChanged = true;
            mine = r;
            return true;
        }
        return false;
    }

    bool remove(int id)
    {
        const auto it = std::find_if(m_regions.begin(), m_regions.end(),
                                     [id](const Region &r) { return r.id == id; });
        if (it == m_regions.end())
            return false;
        pushUndo();
        m_regions.erase(it);
        if (m_selected == id)
            m_selected = -1;
        return true;
    }

    void clear()
    {
        if (m_regions.empty())
            return;
        pushUndo();
        m_regions.clear();
        m_selected = -1;
    }

    /// A drag starts: the state before it is what Undo goes back to.
    void beginGesture()
    {
        if (m_inGesture)
            return;
        m_gestureSnapshot = snapshot();
        m_inGesture = true;
        m_gestureChanged = false;
    }

    /// The drag ends. Returns whether it changed anything (nothing to undo otherwise).
    bool endGesture()
    {
        if (!m_inGesture)
            return false;
        m_inGesture = false;
        if (!m_gestureChanged)
            return false;
        m_undo.push_back(m_gestureSnapshot);
        trimUndo();
        m_redo.clear();
        return true;
    }

    bool inGesture() const { return m_inGesture; }
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    bool undo()
    {
        if (m_inGesture || m_undo.empty())
            return false;
        m_redo.push_back(snapshot());
        restore(m_undo.back());
        m_undo.pop_back();
        return true;
    }

    bool redo()
    {
        if (m_inGesture || m_redo.empty())
            return false;
        m_undo.push_back(snapshot());
        restore(m_redo.back());
        m_redo.pop_back();
        return true;
    }

    /// The region a press at `p` lands on: the selected region's handles
    /// first; then an outline within `tolerance`, topmost (last drawn) first;
    /// then the smallest region the press is inside. Smallest, not topmost: a
    /// small region drawn before a large one around it must stay reachable.
    /// -1 when none.
    int regionAt(const Vec2 &p, double tolerance, double knobOffset, Hit *hit) const
    {
        if (hit)
            *hit = {};
        if (const Region *sel = selected()) {
            const Hit h = hitTest(*sel, p, tolerance, knobOffset, /*withHandles=*/true);
            if (h.kind == Hit::OnHandle) {
                if (hit)
                    *hit = h;
                return sel->id;
            }
        }
        for (auto it = m_regions.rbegin(); it != m_regions.rend(); ++it) {
            if (distanceToOutline(*it, p) <= tolerance) {
                if (hit)
                    hit->kind = Hit::Body;
                return it->id;
            }
        }
        int best = -1;
        double bestArea = std::numeric_limits<double>::infinity();
        for (auto it = m_regions.rbegin(); it != m_regions.rend(); ++it) {
            if (!contains(*it, p[0], p[1]))
                continue;
            const auto b = bounds(*it);
            const double area = (b[1] - b[0]) * (b[3] - b[2]);
            if (area < bestArea) {
                bestArea = area;
                best = it->id;
            }
        }
        if (best >= 0 && hit)
            hit->kind = Hit::Body;
        return best;
    }

    static constexpr int kUndoDepth = 100;

private:
    struct Snapshot
    {
        std::vector<Region> regions;
        int selected{ -1 };
    };

    Snapshot snapshot() const { return { m_regions, m_selected }; }
    void restore(const Snapshot &s)
    {
        m_regions = s.regions;
        m_selected = find(s.selected) ? s.selected : -1;
    }
    void pushUndo()
    {
        m_undo.push_back(snapshot());
        trimUndo();
        m_redo.clear();
    }
    void trimUndo()
    {
        if (static_cast<int>(m_undo.size()) > kUndoDepth)
            m_undo.erase(m_undo.begin());
    }

    std::vector<Region> m_regions;
    int m_selected{ -1 };
    int m_nextId{ 1 };
    std::array<int, 6> m_named{};   ///< per shape, so names read "Box 1", "Circle 1", "Box 2"
    std::vector<Snapshot> m_undo, m_redo;
    Snapshot m_gestureSnapshot;
    bool m_inGesture{ false };
    bool m_gestureChanged{ false };
};

}   // namespace visivo::regions

#endif   // REGION_MODEL_H

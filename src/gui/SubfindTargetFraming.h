#pragma once

// SubfindTargetFraming.h
// Framing a selected target in a 3-D catalogue view (issue #448): the camera
// keeps the direction it looks from and moves so that the target fills the
// view. A SubFind group or subhalo is framed by its physical radius; any
// other source keeps a fraction of the current distance. The same camera goes
// to whichever view is on screen (local VTK, the backend's Remote frames, the
// ray-traced view), so it is a plain value in the JSON terms those views
// already exchange (position, focal_point, view_up, view_angle, parallel,
// parallel_scale).
//
// Header-only, no widgets and no VTK, so it can be tested.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace visivo::framing {

struct Camera
{
    std::array<double, 3> position{ 0.0, 0.0, 1.0 };
    std::array<double, 3> focal{ 0.0, 0.0, 0.0 };
    std::array<double, 3> viewUp{ 0.0, 1.0, 0.0 };
    double viewAngle = 30.0;   ///< degrees, full vertical angle
    bool parallel = false;
    double parallelScale = 1.0;
};

/// The radius fills this much more than itself on screen: the target and a
/// margin around it.
inline constexpr double kRadiusMargin = 1.25;

/// The camera a view describes in JSON, or nothing when a vector is missing
/// or not finite (a view with no camera yet).
inline std::optional<Camera> cameraFromJson(const QJsonObject &json)
{
    const auto vec = [&json](const char *key, std::array<double, 3> &out) {
        const QJsonArray a = json.value(QLatin1String(key)).toArray();
        if (a.size() != 3)
            return false;
        for (int i = 0; i < 3; ++i) {
            if (!a.at(i).isDouble() || !std::isfinite(a.at(i).toDouble()))
                return false;
            out[static_cast<std::size_t>(i)] = a.at(i).toDouble();
        }
        return true;
    };
    Camera cam;
    if (!vec("position", cam.position) || !vec("focal_point", cam.focal)
        || !vec("view_up", cam.viewUp))
        return std::nullopt;
    const double angle = json.value(QLatin1String("view_angle")).toDouble(30.0);
    cam.viewAngle = angle > 0.0 && angle < 180.0 ? angle : 30.0;
    cam.parallel = json.value(QLatin1String("parallel")).toBool(false);
    const double scale = json.value(QLatin1String("parallel_scale")).toDouble(1.0);
    cam.parallelScale = scale > 0.0 && std::isfinite(scale) ? scale : 1.0;
    return cam;
}

inline QJsonObject cameraToJson(const Camera &cam)
{
    const auto arr = [](const std::array<double, 3> &v) {
        return QJsonArray{ v[0], v[1], v[2] };
    };
    return QJsonObject{ { QStringLiteral("position"), arr(cam.position) },
                        { QStringLiteral("focal_point"), arr(cam.focal) },
                        { QStringLiteral("view_up"), arr(cam.viewUp) },
                        { QStringLiteral("view_angle"), cam.viewAngle },
                        { QStringLiteral("parallel"), cam.parallel },
                        { QStringLiteral("parallel_scale"), cam.parallelScale } };
}

/// How far a perspective camera with @p viewAngleDeg must stand from a sphere
/// of @p radius for the sphere, with its margin, to fill the view's height.
/// Never closer than 1.5 radii: a very wide angle would put the camera inside
/// the halo. Zero for a radius that is not a positive number.
inline double framingDistance(double radius, double viewAngleDeg)
{
    if (!(radius > 0.0) || !std::isfinite(radius))
        return 0.0;
    const double angle = viewAngleDeg > 0.0 && viewAngleDeg < 180.0 ? viewAngleDeg : 30.0;
    const double halfAngle = angle * 0.5 * 3.14159265358979323846 / 180.0;
    return std::max(radius * kRadiusMargin / std::tan(halfAngle), radius * 1.5);
}

/// @p cam moved to look at @p target from the same direction. With a
/// positive @p radius the target is framed by it (distance, or a parallel
/// view's scale); otherwise the distance is divided by @p zoomFactor (1: the
/// same distance, a recentre only).
inline Camera frameTarget(const Camera &cam, const std::array<double, 3> &target,
                          double radius, double zoomFactor)
{
    Camera out = cam;
    std::array<double, 3> back{ cam.position[0] - cam.focal[0],
                                cam.position[1] - cam.focal[1],
                                cam.position[2] - cam.focal[2] };
    double oldDistance = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
    if (!(oldDistance > 0.0) || !std::isfinite(oldDistance)) {
        back = { 0.0, 0.0, 1.0 };   // no direction to keep: from the front
        oldDistance = 1.0;
    }
    double distance = oldDistance / (zoomFactor > 0.0 ? zoomFactor : 1.0);
    if (radius > 0.0 && std::isfinite(radius)) {
        distance = framingDistance(radius, cam.viewAngle);
        if (cam.parallel)
            out.parallelScale = radius * kRadiusMargin;
    }
    const double scale = distance / oldDistance;
    out.focal = target;
    for (std::size_t i = 0; i < 3; ++i)
        out.position[i] = target[i] + back[i] * scale;
    return out;
}

/// Whether @p a and @p b put the view in the same place, to a millionth of
/// the larger camera distance: the status line says the view was framed only
/// when it moved.
inline bool sameView(const Camera &a, const Camera &b)
{
    const auto dist = [](const std::array<double, 3> &p, const std::array<double, 3> &q) {
        const double dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const double reach = std::max({ dist(a.position, a.focal), dist(b.position, b.focal),
                                    std::numeric_limits<double>::min() });
    const double tolerance = reach * 1e-6;
    if (dist(a.position, b.position) > tolerance || dist(a.focal, b.focal) > tolerance)
        return false;
    if (a.parallel != b.parallel)
        return false;
    return !a.parallel
            || std::abs(a.parallelScale - b.parallelScale)
            <= std::max(a.parallelScale, b.parallelScale) * 1e-6;
}

/// The radius columns a SubFind target is framed by, in order of preference:
/// @p preferred (the column the markers are sized by) first when it is one of
/// them, so the frame matches the sphere drawn around the target.
inline QStringList radiusFieldOrder(const QString &preferred = QString())
{
    QStringList order{ QStringLiteral("rvir"), QStringLiteral("r500"),
                       QStringLiteral("r2500"), QStringLiteral("half_mass_radius"),
                       QStringLiteral("rmax") };
    const qsizetype at = order.indexOf(preferred);
    if (at > 0)
        order.move(at, 0);
    return order;
}

/// A SubFind radius with what it is defined against, as the backend names it
/// ("500 times mean background density"): "r500 (500 times the mean background
/// density)". The name alone when there is no definition.
inline QString radiusWithDefinition(const QString &field, const QString &definition)
{
    const QString d = definition.trimmed();
    if (d.isEmpty())
        return field;
    // "mean background density" alone is the virial overdensity's reference;
    // the threshold itself depends on cosmology and redshift.
    if (d.compare(QStringLiteral("mean background density"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("%1 (relative to the mean background density)").arg(field);
    QString text = d;
    text.replace(QStringLiteral("times mean"), QStringLiteral("times the mean"));
    return QStringLiteral("%1 (%2)").arg(field, text);
}

/// The status line after a double-click on a SubFind target: what is framed
/// and by which radius. Shown also when the view was already framed (a second
/// double-click), so the selection line the gesture's clicks wrote does not
/// stay in its place.
inline QString framingStatus(const QString &target, const QString &field,
                             const QString &definition, double radius)
{
    return QStringLiteral("Framed %1 at %2 × %3 = %4.")
            .arg(target.isEmpty() ? QStringLiteral("target") : target)
            .arg(kRadiusMargin)
            .arg(radiusWithDefinition(field, definition))
            .arg(radius, 0, 'g', 6);
}

} // namespace visivo::framing

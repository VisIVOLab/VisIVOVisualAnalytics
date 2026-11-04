#include "OrbitPath.h"

#include <QJsonArray>

#include <algorithm>
#include <array>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace {

std::array<double, 3> vec(const QJsonObject &o, const QString &key)
{
    const QJsonArray a = o.value(key).toArray();
    return { a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble() };
}

QJsonArray json(const std::array<double, 3> &v)
{
    return QJsonArray{ v[0], v[1], v[2] };
}

using Vec = std::array<double, 3>;

Vec sub(const Vec &a, const Vec &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
Vec add(const Vec &a, const Vec &b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
Vec mul(const Vec &a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
double dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec &a, const Vec &b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
Vec unit(const Vec &a)
{
    const double n = std::sqrt(dot(a, a));
    return n > 0 ? mul(a, 1.0 / n) : a;
}
/// ``v`` turned by ``degrees`` about the unit axis ``k`` (Rodrigues).
Vec rotate(const Vec &v, const Vec &k, double degrees)
{
    const double t = degrees * M_PI / 180.0, c = std::cos(t), s = std::sin(t);
    return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1.0 - c)));
}

} // namespace

namespace visivo {

QList<QJsonObject> orbitCameras(const QJsonObject &camera, int frames, double degrees)
{
    QList<QJsonObject> cameras;
    if (frames < 1)
        return cameras;
    const auto pos = vec(camera, u"position"_s);
    const auto fp = vec(camera, u"focal_point"_s);
    auto k = vec(camera, u"view_up"_s);
    const double norm = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    if (norm > 0)
        for (double &c : k)
            c /= norm;
    const std::array<double, 3> v{ pos[0] - fp[0], pos[1] - fp[1], pos[2] - fp[2] };
    // A whole turn ends one step short of where it starts, so a looped movie
    // does not show the same frame twice; any other arc ends on its angle.
    const bool loop = std::fmod(std::abs(degrees), 360.0) < 1e-9 && degrees != 0.0;
    const int steps = loop ? frames : std::max(1, frames - 1);
    for (int i = 0; i < frames; ++i) {
        const double t = degrees * M_PI / 180.0 * i / steps;
        const double c = std::cos(t), s = std::sin(t);
        const double kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
        const std::array<double, 3> kxv{ k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2],
                                         k[0] * v[1] - k[1] * v[0] };
        std::array<double, 3> p{};
        for (int d = 0; d < 3; ++d)   // Rodrigues: the offset turned about the up axis
            p[d] = fp[d] + v[d] * c + kxv[d] * s + k[d] * kv * (1.0 - c);
        QJsonObject cam = camera;
        cam.insert(u"position"_s, json(p));
        cameras << cam;
    }
    return cameras;
}

QJsonObject turnCamera(const QJsonObject &camera, double azimuth, double elevation)
{
    const Vec fp = vec(camera, u"focal_point"_s);
    Vec offset = sub(vec(camera, u"position"_s), fp);
    Vec up = unit(vec(camera, u"view_up"_s));
    offset = rotate(offset, up, azimuth);
    // The right axis, from the turned offset: elevation tilts the view about it.
    const Vec right = unit(cross(mul(offset, -1.0), up));
    // Positive raises the camera, as VTK's Elevation.
    offset = rotate(offset, right, -elevation);
    up = unit(rotate(up, right, -elevation));
    QJsonObject cam = camera;
    cam.insert(u"position"_s, json(add(fp, offset)));
    cam.insert(u"view_up"_s, json(up));
    return cam;
}

QJsonObject dollyCamera(const QJsonObject &camera, double factor)
{
    if (!(factor > 0))
        return camera;
    QJsonObject cam = camera;
    if (camera.value(u"parallel"_s).toBool()) {
        cam.insert(u"parallel_scale"_s, camera.value(u"parallel_scale"_s).toDouble(1.0) / factor);
        return cam;
    }
    const Vec fp = vec(camera, u"focal_point"_s);
    const Vec offset = sub(vec(camera, u"position"_s), fp);
    cam.insert(u"position"_s, json(add(fp, mul(offset, 1.0 / factor))));
    return cam;
}

QJsonObject panCamera(const QJsonObject &camera, double dx, double dy)
{
    const Vec pos = vec(camera, u"position"_s), fp = vec(camera, u"focal_point"_s);
    const Vec forward = sub(fp, pos);
    const Vec up = unit(vec(camera, u"view_up"_s));
    const Vec right = unit(cross(forward, up));
    const double height = camera.value(u"parallel"_s).toBool()
            ? 2.0 * camera.value(u"parallel_scale"_s).toDouble(1.0)
            : 2.0 * std::sqrt(dot(forward, forward))
                    * std::tan(camera.value(u"view_angle"_s).toDouble(30.0) * M_PI / 360.0);
    // The scene follows the pointer: the camera moves the other way.
    const Vec shift = add(mul(right, -dx * height), mul(unit(cross(right, forward)), dy * height));
    QJsonObject cam = camera;
    cam.insert(u"position"_s, json(add(pos, shift)));
    cam.insert(u"focal_point"_s, json(add(fp, shift)));
    return cam;
}

} // namespace visivo

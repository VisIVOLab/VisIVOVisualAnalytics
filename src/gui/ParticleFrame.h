#ifndef PARTICLEFRAME_H
#define PARTICLEFRAME_H

// The frame the ray-traced view's particles come in (issue #208): float32
// offsets from an origin, in units of one scale, so that particles far from
// 0 (1e9, 1e9 + 1, 1e9 + 2) stay apart where a float32 cast of their
// coordinates merges them. The table's coordinates are
// origin + scale * local; the camera, in the table's coordinates, is moved
// into the frame in double precision before anything is drawn in float.
// Pure geometry on the backend's camera objects, kept apart from the GL view
// so it can be tested alone.

#include <QByteArray>
#include <QJsonObject>

#include <array>

namespace visivo {

struct ParticleFrame
{
    std::array<double, 3> origin{ 0.0, 0.0, 0.0 };
    double scale = 1.0;

    /// From the reply's X-Visivo-Particle-Origin ("x,y,z") and
    /// X-Visivo-Particle-Scale: the identity when they are missing (a
    /// backend that predates them sends the table's coordinates) or not
    /// three finite numbers and a positive finite scale.
    static ParticleFrame fromHeaders(const QByteArray &origin, const QByteArray &scale);
    /// The frame centred on a box (x0, x1, y0, y1, z0, z1), scale 1: what a
    /// box and its camera are drawn in, so float keeps them apart far from 0.
    static ParticleFrame centredOn(const std::array<double, 6> &box);

    bool isIdentity() const
    {
        return origin[0] == 0.0 && origin[1] == 0.0 && origin[2] == 0.0 && scale == 1.0;
    }
    /// A local position in the table's coordinates.
    std::array<double, 3> toTable(const float *local) const;
    /// A box (x0, x1, y0, y1, z0, z1) of the table in this frame.
    std::array<double, 6> toLocal(const std::array<double, 6> &box) const;
    /// The camera (table coordinates) in this frame: position and focal point
    /// shifted and scaled, the parallel scale scaled; the rest kept.
    QJsonObject localCamera(const QJsonObject &camera) const;
};

} // namespace visivo

#endif // PARTICLEFRAME_H

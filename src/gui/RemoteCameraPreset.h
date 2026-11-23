#pragma once

// RemoteCameraPreset.h
// The Camera menu's presets (Front, Back, Left, Right, Top, Bottom) in a
// Remote 3-D view (issue #390). This window's camera is in its preview's
// coordinates and fits its own widget, so the view is named rather than sent:
// the backend fits its own camera from the front and turns it by the same
// azimuth and elevation (render_session/_common.py, camera_preset).
//
// Header-only, no widgets, so it can be tested.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace visivo::remote {

/// The ``camera_preset`` command: the cube from the front (looking down -z,
/// y up), turned by @p azimuth about y, then by @p elevation, in degrees.
inline QVariantMap cameraPresetCommand(double azimuth, double elevation)
{
    return QVariantMap{ { QStringLiteral("type"), QStringLiteral("camera_preset") },
                        { QStringLiteral("azimuth"), azimuth },
                        { QStringLiteral("elevation"), elevation } };
}

/// The ``set_camera`` command that puts a session's camera where @p camera
/// (an answer of GET /v1/render/{id}/camera) says: a session that replaces a
/// lost one starts from the view the user had (issue #411). Empty when
/// @p camera lacks a position, a focal point or a view up of three numbers.
inline QVariantMap setCameraCommand(const QJsonObject &camera)
{
    static const char *const vectors[] = { "position", "focal_point", "view_up" };
    QVariantMap command{ { QStringLiteral("type"), QStringLiteral("set_camera") } };
    for (const char *key : vectors) {
        const QJsonArray v = camera.value(QLatin1String(key)).toArray();
        if (v.size() != 3)
            return {};
        for (const QJsonValue &x : v)
            if (!x.isDouble())
                return {};
        command.insert(QLatin1String(key), v.toVariantList());
    }
    // Optional, and passed on only as the backend takes them.
    const QJsonValue angle = camera.value(QLatin1String("view_angle"));
    if (angle.isDouble())
        command.insert(QStringLiteral("view_angle"), angle.toDouble());
    const QJsonValue parallel = camera.value(QLatin1String("parallel"));
    if (parallel.isBool())
        command.insert(QStringLiteral("parallel"), parallel.toBool());
    const QJsonValue scale = camera.value(QLatin1String("parallel_scale"));
    if (scale.isDouble())
        command.insert(QStringLiteral("parallel_scale"), scale.toDouble());
    return command;
}

} // namespace visivo::remote

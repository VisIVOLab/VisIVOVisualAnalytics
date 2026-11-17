#pragma once

// RemoteCameraPreset.h
// The Camera menu's presets (Front, Back, Left, Right, Top, Bottom) in a
// Remote 3-D view (issue #390). This window's camera is in its preview's
// coordinates and fits its own widget, so the view is named rather than sent:
// the backend fits its own camera from the front and turns it by the same
// azimuth and elevation (render_session/_common.py, camera_preset).
//
// Header-only, no widgets, so it can be tested.

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

} // namespace visivo::remote

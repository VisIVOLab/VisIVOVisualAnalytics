#ifndef ORBITPATH_H
#define ORBITPATH_H

// The cameras of an orbit movie (issue #101): pure geometry on the backend's
// camera objects ({position, focal_point, view_up, ...}), kept apart from the
// dialog that uses it so it can be tested alone.

#include <QJsonObject>
#include <QList>

namespace visivo {

/// ``frames`` cameras turning ``degrees`` about the camera's view-up axis
/// through its focal point. A whole number of turns stops one step short of
/// the start, so a looped movie does not show a frame twice; any other arc
/// ends on its angle.
QList<QJsonObject> orbitCameras(const QJsonObject &camera, int frames, double degrees);

/// The camera turned about its focal point: ``azimuth`` degrees about its
/// view-up axis, then ``elevation`` degrees about its right axis (the view up
/// turning with it), as a trackball drag turns a VTK camera.
QJsonObject turnCamera(const QJsonObject &camera, double azimuth, double elevation);
/// The camera moved towards its focal point (``factor`` > 1) or away from it.
QJsonObject dollyCamera(const QJsonObject &camera, double factor);
/// Position and focal point moved together in the view plane, by ``dx`` and
/// ``dy`` times the height the view shows at the focal point.
QJsonObject panCamera(const QJsonObject &camera, double dx, double dy);

} // namespace visivo

#endif // ORBITPATH_H

#ifndef CubeAxesEdgeOn_h
#define CubeAxesEdgeOn_h

#include <array>
#include <cmath>

// ---------------------------------------------------------------------------
// CubeAxesEdgeOn — which axes of a labelled box can carry their text (issue
// #569).
//
// vtkCubeAxesActor lays an axis title (and its tick labels) along the axis in
// 3-D, so an axis seen nearly end-on foreshortens them to a long diagonal white
// streak instead of text. The foreshortening of an axis is the sine of the angle
// between the axis and the line of sight (1 across the view, 0 along it); below
// EdgeOnSine the axis is left out until the view turns away from it. 0.3 is about
// 17 degrees: the text is garbled at 12 and legible at 25.
//
// The backend's point session applies the same rule (render_session/points.py,
// hide_edge_on_axes), so a remote view and a local one drop the same axes.
// ---------------------------------------------------------------------------
namespace CubeAxesEdgeOn {

constexpr double EdgeOnSine = 0.3;

/// The sine of the angle between each axis of the box and the line of sight
/// from `eye` to `aim`. A perspective view aims at the middle of the box; a
/// parallel one looks along its direction of projection, so it aims at the
/// camera's focal point, which need not be the middle. `aim` on `eye` gives 1
/// for every axis.
inline std::array<double, 3> axisSines(const double eye[3], const double aim[3])
{
    double sight[3];
    double norm = 0.0;
    for (int i = 0; i < 3; ++i) {
        sight[i] = aim[i] - eye[i];
        norm += sight[i] * sight[i];
    }
    norm = std::sqrt(norm);
    if (norm <= 0.0)
        return { 1.0, 1.0, 1.0 };
    std::array<double, 3> sines{};
    for (int i = 0; i < 3; ++i) {
        const double along = sight[i] / norm;
        sines[i] = std::sqrt(std::fmax(0.0, 1.0 - along * along));
    }
    return sines;
}

/// Whether an axis with this sine carries a readable title and labels.
inline bool readable(double sine) { return sine >= EdgeOnSine; }

}  // namespace CubeAxesEdgeOn

#endif // CubeAxesEdgeOn_h

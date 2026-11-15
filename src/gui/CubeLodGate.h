#ifndef CUBE_LOD_GATE_H
#define CUBE_LOD_GATE_H
// CubeLodGate.h
// When the cube viewer may show finer detail over the coarse 3-D preview
// (issue #321), as pure inline logic in the CubeToolGate.h pattern: no Qt
// widgets, no VTK, unit-tested in tests/test_cube_lod_gate.cpp.
//
// The level of detail is drawn by the client's own volume mapper over the
// preview it holds, so it only makes sense where that preview is what the
// 3-D view shows: a cube opened through the backend, shown as its strided or
// block-mean preview, rendered locally, in Composite blend. Everywhere else
// the plain view stays as it is, and the reason says why, so a status line or
// a tooltip never leaves the user guessing.
//
// The checks run in a fixed order and the first that fails gives the reason:
// the user's setting first (nothing else matters when it is off), then what
// the data is, then what the view is doing.

#include <QString>

namespace CubeLodGate {

struct Inputs
{
    /// "Level of detail" is on in Settings.
    bool settingEnabled{ true };
    /// The cube was opened through the backend (local or remote), which can
    /// read finer levels of it.
    bool remoteDataset{ false };
    /// The 3-D view shows the preview, not the full-resolution cube or a
    /// loaded region (usingHighResCube is false).
    bool showingPreview{ false };
    /// Cube voxels per preview sample; 1 means the preview is the cube.
    int previewStride{ 1 };
    /// The client renders the 3-D view (not the backend's render session).
    bool localRender{ false };
    /// Composite blend: MIP and MinIP take an extremum along the whole ray,
    /// which separate volumes cannot combine.
    bool compositeBlend{ false };
    /// The isosurface is drawn instead of the volume.
    bool isosurfaceShown{ false };
    /// The local view drives a headset (the cube is shown in VR from here).
    bool localVrActive{ false };
};

struct Decision
{
    bool active{ false };
    /// Empty when active; otherwise one sentence saying why not.
    QString reason;
};

inline Decision evaluate(const Inputs &in)
{
    using namespace Qt::StringLiterals;
    auto no = [](const QString &why) { return Decision{ false, why }; };
    if (!in.settingEnabled)
        return no(u"Level of detail is off in Settings."_s);
    if (!in.remoteDataset)
        return no(u"Level of detail needs a cube opened through the backend."_s);
    if (!in.showingPreview)
        return no(u"Full-resolution data is shown (the whole cube or a region): there is no finer detail to add."_s);
    if (in.previewStride <= 1)
        return no(u"The preview already holds every voxel of the cube."_s);
    if (!in.localRender)
        return no(u"The backend renders this view: level of detail applies to local rendering."_s);
    if (!in.compositeBlend)
        return no(u"MIP and MinIP show the whole preview: finer detail needs Composite blending."_s);
    if (in.isosurfaceShown)
        return no(u"The isosurface is shown instead of the volume."_s);
    if (in.localVrActive)
        return no(u"The cube is shown in the headset: level of detail is paused."_s);
    return Decision{ true, QString() };
}

}   // namespace CubeLodGate

#endif   // CUBE_LOD_GATE_H

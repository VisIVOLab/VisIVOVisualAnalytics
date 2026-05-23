#pragma once

#include <QObject>
#include <QString>

class vtkRenderer;
class vtkVolume;
class vtkProp3D;

/// Optional VR (OpenXR) session controller for the cube viewer.
///
/// **The header is always compileable.** When the project is built without
/// VR support (default), every method is a safe no-op:
///
/// * ``isCompiledIn()`` → false
/// * ``isRuntimeAvailable()`` → false (returns false without ever touching
///   OpenXR-specific headers)
/// * ``open()`` → returns false and sets ``lastError()`` with an explanatory
///   message so the menu action can display it to the user.
///
/// When the project is built with ``-DVISIVO_ENABLE_VR=ON`` *and* the VTK
/// build provides ``VTK::RenderingOpenXR``, this controller takes the same
/// ``vtkVolume`` / actors that the desktop cube viewer already renders and
/// presents them on the HMD via a second OpenXR-backed render window.
///
/// Pattern: the desktop ``vtkVolume`` / ``vtkGPUVolumeRayCastMapper`` are
/// added to a separate ``vtkOpenXRRenderWindow`` — sharing the volume mapper
/// (NOT the renderer) means live changes to the LUT / threshold / opacity
/// TF in the desktop UI propagate to the HMD without any extra IPC. The
/// caller is responsible for ensuring those VTK objects are not destroyed
/// while the VR session is running.
class CubeVRController : public QObject
{
    Q_OBJECT

public:
    explicit CubeVRController(QObject *parent = nullptr);
    ~CubeVRController() override;

    /// True when the binary was built with VR support compiled in
    /// (``VISIVO_HAS_VR``). Cheap, can be called from UI thread to enable /
    /// disable menu entries.
    static bool isCompiledIn();

    /// True when an OpenXR runtime is available and an HMD is connected.
    /// Only meaningful when ``isCompiledIn()`` returns true; otherwise
    /// always false. Safe to call from the UI thread (cached after first
    /// successful probe).
    static bool isRuntimeAvailable();

    /// Open the given desktop-side cube volume on the HMD. Returns true on
    /// success. On failure, populate ``lastError()`` with a message suitable
    /// for the user (e.g. "VR support is not compiled in", "No OpenXR
    /// runtime found"). The host can keep changing the volume mapper's LUT /
    /// opacity TF after a successful open — those changes will be visible
    /// in the headset on the next frame.
    bool open(vtkRenderer *desktopRenderer, vtkVolume *cubeVolume);

    /// Close any active VR session. Safe to call repeatedly.
    void close();

    QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
    // Implementation detail is hidden behind the .cpp — when VR is not
    // compiled in this stays empty and the methods above are no-ops.
    struct Impl;
    Impl *d{ nullptr };
};

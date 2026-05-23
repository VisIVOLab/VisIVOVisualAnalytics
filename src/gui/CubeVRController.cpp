#include "CubeVRController.h"

#include <QDebug>

#ifdef VISIVO_HAS_VR
#  include <vtkOpenXRManager.h>
#  include <vtkOpenXRRenderWindow.h>
#  include <vtkOpenXRRenderWindowInteractor.h>
#  include <vtkOpenXRRenderer.h>
#  include <vtkRenderer.h>
#  include <vtkVolume.h>
#  include <vtkNew.h>
#  include <vtkSmartPointer.h>
#endif

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Impl (PIMPL) — only defined when VR is compiled in. The base class methods
// branch on the pointer being non-null, so the VR-less build is a clean stub
// without dragging OpenXR headers into translation units that don't need them.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef VISIVO_HAS_VR
struct CubeVRController::Impl
{
    vtkSmartPointer<vtkOpenXRRenderWindow>            renderWindow;
    vtkSmartPointer<vtkOpenXRRenderWindowInteractor>  interactor;
    // Must be the OpenXR subclass, not the plain factory default. The render
    // window's AddRenderer() rejects vtkOpenGLRenderer with a stern message
    // ("A vtkOpenXRRenderer is expected") and leaves the scene unattached.
    vtkSmartPointer<vtkOpenXRRenderer>                vrRenderer;
    bool active{ false };
};
#else
struct CubeVRController::Impl
{
    // Empty placeholder for the no-VR build. Keeps `d` pointers semantically
    // valid (== nullptr) without forward-declaration headaches.
};
#endif

// ─────────────────────────────────────────────────────────────────────────────

CubeVRController::CubeVRController(QObject *parent)
    : QObject(parent)
{
}

CubeVRController::~CubeVRController()
{
    close();
}

bool CubeVRController::isCompiledIn()
{
#ifdef VISIVO_HAS_VR
    return true;
#else
    return false;
#endif
}

bool CubeVRController::isRuntimeAvailable()
{
#ifdef VISIVO_HAS_VR
    // We used to probe by creating a vtkOpenXRRenderWindow and calling
    // Initialize/Finalize, then caching the result. In practice this is
    // actively harmful: the Oculus / SteamVR loaders enforce a single
    // xrInstance per process, and their teardown after the probe leaks
    // enough state that a subsequent open() fails with
    // XR_ERROR_LIMIT_REACHED ("Loader does not support simultaneous
    // XrInstances"). The probe also can't tell "no HMD connected" from
    // "runtime not installed" — Initialize succeeds for the OpenGL side
    // either way and the form-factor check only fails later.
    //
    // Cheaper and more honest: assume the runtime is reachable whenever
    // VR support is compiled in. open() will do the real attempt and
    // surface a clean error if there's no HMD or no runtime.
    return true;
#else
    return false;
#endif
}

bool CubeVRController::open(vtkRenderer *desktopRenderer, vtkVolume *cubeVolume)
{
#ifdef VISIVO_HAS_VR
    if (!desktopRenderer || !cubeVolume) {
        m_lastError = u"Internal error: missing renderer or volume."_s;
        return false;
    }
    if (!d) {
        d = new Impl();
    }
    if (d->active) {
        // Already running — re-raising is sufficient.
        return true;
    }

    d->renderWindow = vtkSmartPointer<vtkOpenXRRenderWindow>::New();
    d->renderWindow->Initialize();

    // vtkOpenXRRenderWindow::Initialize() does NOT clear `Initialized` when
    // the OpenXR session creation fails (no HMD, runtime missing, form-factor
    // unavailable, etc.) — the OpenGL half is independent and reports success.
    // The canonical signal is vtkOpenXRManager::IsSessionRunning(), which only
    // flips true after BeginSession() succeeds. If it's still false we MUST
    // bail before constructing the interactor: interactor->Initialize() calls
    // renderWindow->Initialize() a second time, which trips the loader's
    // single-XrInstance rule (XR_ERROR_LIMIT_REACHED) and any subsequent
    // Render() then blocks against a null XrSession.
    //
    // Finalize() is also required even on the failure path: a partially-built
    // XrInstance is left behind by the failed Initialize(), and only Finalize()
    // releases it so the *next* open() attempt isn't poisoned.
    if (!vtkOpenXRManager::GetInstance().IsSessionRunning()) {
        m_lastError = u"Could not start an OpenXR session. Make sure an HMD is "
                      u"connected and the OpenXR runtime (SteamVR / Oculus / "
                      u"WMR) is running, then try again."_s;
        if (d->renderWindow->GetInitialized()) {
            d->renderWindow->Finalize();
        }
        d->renderWindow = nullptr;
        return false;
    }

    // Build the OpenXR-flavoured renderer (not the factory default, which is
    // a plain vtkOpenGLRenderer and is rejected by vtkOpenXRRenderWindow).
    d->vrRenderer = vtkSmartPointer<vtkOpenXRRenderer>::New();
    // Share the volume actor — its mapper is the same one the desktop cube
    // viewer mutates when the user changes the LUT / threshold / opacity TF,
    // so those edits become visible in the HMD on the next frame "for free".
    d->vrRenderer->AddVolume(cubeVolume);
    d->vrRenderer->SetBackground(0.04, 0.05, 0.08);
    d->renderWindow->AddRenderer(d->vrRenderer);

    d->interactor = vtkSmartPointer<vtkOpenXRRenderWindowInteractor>::New();
    d->interactor->SetRenderWindow(d->renderWindow);
    d->interactor->Initialize();

    d->vrRenderer->ResetCamera();
    d->renderWindow->Render();

    // NOTE: ``Start()`` blocks the calling thread until the VR session ends.
    // For a no-regret first cut we run it on the UI thread on demand — the
    // user explicitly chose Open in VR and expects to stay in the headset
    // until they close it. A future iteration can move this to a worker
    // QThread if the desktop UI needs to remain interactive during VR.
    qInfo() << "[vr] entering OpenXR session — UI thread blocks until user exits VR";
    d->active = true;
    d->interactor->Start();
    d->active = false;
    qInfo() << "[vr] OpenXR session ended";
    return true;
#else
    Q_UNUSED(desktopRenderer);
    Q_UNUSED(cubeVolume);
    m_lastError = u"VR support is not compiled in. Rebuild with -DVISIVO_ENABLE_VR=ON "
                  u"(requires VTK built with VTK_MODULE_ENABLE_VTK_RenderingOpenXR=YES)."_s;
    return false;
#endif
}

void CubeVRController::close()
{
#ifdef VISIVO_HAS_VR
    if (!d) return;
    if (d->interactor && d->active) {
        d->interactor->TerminateApp();
    }
    if (d->renderWindow && d->renderWindow->GetInitialized()) {
        d->renderWindow->Finalize();
    }
    d->interactor   = nullptr;
    d->vrRenderer   = nullptr;
    d->renderWindow = nullptr;
    d->active = false;
    delete d;
    d = nullptr;
#endif
}

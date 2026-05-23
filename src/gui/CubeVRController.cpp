#include "CubeVRController.h"

#include <QDebug>

#ifdef VISIVO_HAS_VR
#  include <vtkOpenXRRenderWindow.h>
#  include <vtkOpenXRRenderWindowInteractor.h>
#  include <vtkRenderer.h>
#  include <vtkRendererCollection.h>
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
    vtkSmartPointer<vtkRenderer>                      vrRenderer;
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
    // Cheapest probe: try to create the render window and call Initialize().
    // If no OpenXR runtime is installed or no HMD is connected this fails
    // gracefully and we return false. The result is cached for the rest of
    // the session — probing repeatedly is wasteful and can be slow on some
    // runtimes (the spec allows up to ~1 s for xrEnumerateInstanceProperties).
    static bool probed = false;
    static bool available = false;
    if (probed) return available;
    probed = true;
    try {
        vtkNew<vtkOpenXRRenderWindow> rw;
        rw->Initialize();
        available = (rw->GetInitialized() != 0);
        rw->Finalize();
    } catch (...) {
        available = false;
    }
    return available;
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
    if (d->renderWindow->GetInitialized() == 0) {
        m_lastError = u"OpenXR runtime is not available or no HMD is connected."_s;
        d->renderWindow = nullptr;
        return false;
    }

    d->vrRenderer = vtkSmartPointer<vtkRenderer>::New();
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
